/* fs/pipe.c -- pipes: anonymous ones (pipe(2), pipe2(2)) and named ones
 * (FIFOs, mknod(2) and mkfifo(3))
 *
 * One 16 KiB ring buffer shared by a read end and a write end, each an
 * anonymous vnode outside the file tree. Blocking follows POSIX: readers
 * wait for data while a writer exists (then read 0 = EOF), writers wait for
 * room while a reader exists (none: -EPIPE; there are no signals yet, so no
 * SIGPIPE). O_NONBLOCK on either end turns waiting into -EAGAIN. Waiting
 * uses the scheduler's sleep_on()/wakeup() on the pipe itself.
 *
 * A FIFO is a node in the file tree (vfs_mkfifo; in memory, also on a
 * disk, like a socket's name) whose opens share one pipe: it is made by
 * the first open and goes with the last close. As on Linux, an open for
 * reading waits for a writer and one for writing waits for a reader
 * (O_NONBLOCK: a reader goes on at once, a writer gets ENXIO), and
 * O_RDWR waits for nobody. The open file's access mode says which end it
 * is.
 *
 * Each pipe has its own spinlock: reads, writes and polls run without the
 * big kernel lock. The data is copied between kernel buffers (the system
 * call's bounce buffer), so nothing under the lock faults or sleeps. */
#include <kernel/process.h>
#include <kernel/vfs.h>
#include <kernel/task.h>
#include <kernel/mm.h>
#include <kernel/cpu.h>
#include <kernel/string.h>
#include <kernel/spinlock.h>

#define PIPE_BUF_SIZE   16384

struct pipe {
    struct spinlock lock;
    uint8_t  buf[PIPE_BUF_SIZE];
    size_t   head, count;           /* read position, bytes stored */
    int      readers, writers;
    uint64_t r_opens, w_opens;      /* FIFOs: opens so far, for the waits in fifo_open */
    struct vnode rd, wr;
};

static struct pipe *pipe_of(struct file *f) { return f->vn->data; }
static bool reads(struct file *f)  { return (f->flags & O_ACCMODE) != O_WRONLY; }
static bool writes(struct file *f) { return (f->flags & O_ACCMODE) != O_RDONLY; }

static ssize_t pipe_read(struct file *f, void *dst, size_t len)
{
    struct pipe *p = pipe_of(f);
    if (!len) return 0;
    uint64_t fl = spin_lock_irqsave(&p->lock);
    while (!p->count) {
        if (!p->writers) { spin_unlock_irqrestore(&p->lock, fl); return 0; }
        if (f->flags & O_NONBLOCK) { spin_unlock_irqrestore(&p->lock, fl); return -EAGAIN; }
        if (signal_pending()) { spin_unlock_irqrestore(&p->lock, fl); return -ERESTARTSYS; }
        sleep_on_locked(p, &p->lock);
    }
    size_t n = len < p->count ? len : p->count;
    for (size_t i = 0; i < n; i++) ((uint8_t *)dst)[i] = p->buf[(p->head + i) % PIPE_BUF_SIZE];
    p->head = (p->head + n) % PIPE_BUF_SIZE;
    p->count -= n;
    wakeup(p);
    spin_unlock_irqrestore(&p->lock, fl);
    return (ssize_t)n;
}

static ssize_t pipe_write(struct file *f, const void *src, size_t len)
{
    struct pipe *p = pipe_of(f);
    size_t done = 0;
    uint64_t fl = spin_lock_irqsave(&p->lock);
    while (done < len) {
        if (!p->readers) { spin_unlock_irqrestore(&p->lock, fl); return done ? (ssize_t)done : -EPIPE; }
        if (p->count == PIPE_BUF_SIZE) {
            if (f->flags & O_NONBLOCK) break;
            if (signal_pending()) { spin_unlock_irqrestore(&p->lock, fl); return done ? (ssize_t)done : -ERESTARTSYS; }
            sleep_on_locked(p, &p->lock);
            continue;
        }
        size_t room = PIPE_BUF_SIZE - p->count, n = len - done < room ? len - done : room;
        for (size_t i = 0; i < n; i++)
            p->buf[(p->head + p->count + i) % PIPE_BUF_SIZE] = ((const uint8_t *)src)[done + i];
        p->count += n;
        done += n;
        wakeup(p);
    }
    spin_unlock_irqrestore(&p->lock, fl);
    return done ? (ssize_t)done : -EAGAIN;
}

static int pipe_poll(struct file *f, int events)
{
    struct pipe *p = pipe_of(f);
    int r = 0;
    poll_wait(p);
    uint64_t fl = spin_lock_irqsave(&p->lock);
    if (reads(f)) {
        if (p->count) r |= POLLIN;
        if (!p->writers && (f->vn == &p->rd || p->w_opens)) r |= POLLHUP;  /* a FIFO: once a writer was there */
    }
    if (writes(f)) {
        if (p->count < PIPE_BUF_SIZE) r |= POLLOUT;
        if (!p->readers) r |= POLLERR;
    }
    spin_unlock_irqrestore(&p->lock, fl);
    return r & (events | POLLERR | POLLHUP);
}

static void pipe_release(struct file *f)
{
    struct pipe *p = pipe_of(f);
    uint64_t fl = spin_lock_irqsave(&p->lock);
    if (reads(f)) p->readers--;
    if (writes(f)) p->writers--;
    bool gone = !p->readers && !p->writers;
    wakeup(p);
    spin_unlock_irqrestore(&p->lock, fl);
    if (gone) kfree(p);
}

static const struct vnode_ops pipe_ops = {
    .fread = pipe_read, .fwrite = pipe_write, .poll = pipe_poll, .release = pipe_release,
    .nolock = true,
};

int pipe_create(struct file **rd, struct file **wr, int flags)
{
    struct pipe *p = kzalloc(sizeof *p);
    if (!p) return -ENOMEM;
    lockcheck_name(&p->lock, "pipe");
    struct vnode *v[2] = { &p->rd, &p->wr };
    for (int i = 0; i < 2; i++) {
        strlcpy(v[i]->name, i ? "pipe:[w]" : "pipe:[r]", sizeof v[i]->name);
        v[i]->type = VCHR;
        v[i]->mode = 0010600;                       /* S_IFIFO | rw------- */
        v[i]->ops = &pipe_ops;
        v[i]->data = p;
    }
    *rd = vfs_file_new(&p->rd, O_RDONLY | (flags & O_NONBLOCK));
    *wr = vfs_file_new(&p->wr, O_WRONLY | (flags & O_NONBLOCK));
    if (!*rd || !*wr) { kfree(*rd); kfree(*wr); kfree(p); return -ENOMEM; }
    p->readers = p->writers = 1;
    return 0;
}

/* ---- FIFOs ----------------------------------------------------------------------- */
/* A FIFO's node holds its pipe (vn->data) while any open file of it is
 * there; fifo_lock covers making, finding and dropping it. */
static struct spinlock fifo_lock;

/* Reads, writes and polls are a pipe's: pipe_of() finds it through the
 * node, as through an anonymous pipe's ends. */
static struct pipe *fifo_pipe(struct file *f) { return f->vn->data; }

static void fifo_release(struct file *f)
{
    struct pipe *p = fifo_pipe(f);
    uint64_t gl = spin_lock_irqsave(&fifo_lock);
    uint64_t fl = spin_lock_irqsave(&p->lock);
    if (reads(f)) p->readers--;
    if (writes(f)) p->writers--;
    bool gone = !p->readers && !p->writers;
    wakeup(p);
    spin_unlock_irqrestore(&p->lock, fl);
    if (gone && f->vn->data == p) f->vn->data = NULL;       /* the next open makes a new one */
    spin_unlock_irqrestore(&fifo_lock, gl);
    if (gone) kfree(p);
}

static int fifo_open(struct vnode *vn, int flags, struct file **out)
{
    struct file *f = vfs_file_new(vn, flags);
    if (!f) return -ENOMEM;
    vfs_vnode_put(vn);                  /* open_node() takes the open file's reference */
    struct pipe *fresh = NULL;
    uint64_t gl = spin_lock_irqsave(&fifo_lock);
    struct pipe *p = vn->data;
    if (!p) {
        spin_unlock_irqrestore(&fifo_lock, gl);
        if (!(fresh = kzalloc(sizeof *fresh))) { kfree(f); return -ENOMEM; }
        lockcheck_name(&fresh->lock, "fifo");
        gl = spin_lock_irqsave(&fifo_lock);
        if (!(p = vn->data)) { p = vn->data = fresh; fresh = NULL; }
    }
    uint64_t fl = spin_lock_irqsave(&p->lock);
    bool rd = reads(f), wr = writes(f);
    if (wr && !rd && (flags & O_NONBLOCK) && !p->readers) {     /* nobody to read it */
        bool gone = !p->readers && !p->writers;
        if (gone) vn->data = NULL;
        spin_unlock_irqrestore(&p->lock, fl);
        spin_unlock_irqrestore(&fifo_lock, gl);
        if (gone) kfree(p);
        kfree(fresh);
        kfree(f);
        return -ENXIO;
    }
    if (rd) { p->readers++; p->r_opens++; }
    if (wr) { p->writers++; p->w_opens++; }
    wakeup(p);
    spin_unlock_keep_irqs(&fifo_lock);                  /* p stays: this open counts in it */
    (void)fl;                                           /* interrupts stay off until gl is restored */
    /* wait for the other end (not with O_RDWR or O_NONBLOCK) */
    int r = 0;
    if (rd != wr && !(flags & O_NONBLOCK)) {
        uint64_t seen = rd ? p->w_opens : p->r_opens;
        while (!(rd ? p->writers || p->w_opens != seen : p->readers || p->r_opens != seen)) {
            if (signal_pending()) { r = -ERESTARTSYS; break; }
            sleep_on_locked(p, &p->lock);
        }
    }
    spin_unlock_irqrestore(&p->lock, gl);
    kfree(fresh);
    if (r) { fifo_release(f); kfree(f); return r; }
    *out = f;
    return 0;
}

const struct vnode_ops fifo_ops = {
    .open = fifo_open, .fread = pipe_read, .fwrite = pipe_write, .poll = pipe_poll, .release = fifo_release,
    .nolock = true, .write_on_rdonly = true,
};
