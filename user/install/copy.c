/* user/install/copy.c -- copying the running system's files
 *
 * The tree is copied as it is: directories, files (hard links stay hard
 * links), symbolic links and named pipes, with their owners, modes
 * (setuid included) and times. Device files are left out (the kernel makes
 * /dev), and so is what lies below the skipped directories, which are made
 * empty: /proc, /sys, /dev, /run, /tmp, /boot (the boot volume is copied
 * whole, as itself), and every other mount point. */
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

struct walk {
    const char *const *skip;
    copy_cb cb;
    void *ctx;
    struct copy_progress pr;
    uint64_t last_report;
    char *err;
    size_t errcap;
    /* hard links: (dev, ino) -> the first copy's path */
    struct hl { dev_t dev; ino_t ino; char *path; } *hl;
    size_t nhl, caphl;
    char *buf;
};

static bool skipped(const struct walk *w, const char *rel)
{
    for (int i = 0; w->skip && w->skip[i]; i++) if (!strcmp(w->skip[i], rel)) return true;
    return false;
}

static void report(struct walk *w, bool force)
{
    if (!w->cb) return;
    if (!force && w->pr.bytes - w->last_report < 4 * MIB) return;
    w->last_report = w->pr.bytes;
    w->cb(&w->pr, w->ctx);
}

static const char *find_hl(struct walk *w, const struct stat *st)
{
    for (size_t i = 0; i < w->nhl; i++) if (w->hl[i].dev == st->st_dev && w->hl[i].ino == st->st_ino) return w->hl[i].path;
    return NULL;
}

static void add_hl(struct walk *w, const struct stat *st, const char *path)
{
    if (w->nhl == w->caphl) {
        size_t n = w->caphl ? w->caphl * 2 : 256;
        struct hl *h = realloc(w->hl, n * sizeof *h);
        if (!h) return;
        w->hl = h;
        w->caphl = n;
    }
    w->hl[w->nhl++] = (struct hl){ st->st_dev, st->st_ino, strdup(path) };
}

static void attrs(const char *dst, const struct stat *st, bool is_link)
{
    if (lchown(dst, st->st_uid, st->st_gid) < 0) ilog("chown %s: %s", dst, strerror(errno));
    if (!is_link) chmod(dst, st->st_mode & 07777);
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    utimensat(AT_FDCWD, dst, ts, AT_SYMLINK_NOFOLLOW);
}

static int fail(struct walk *w, const char *what, const char *path)
{
    snprintf(w->err, w->errcap, "%s %s: %s", what, path, strerror(errno));
    ilog("%s", w->err);
    return -1;
}

static int copy_file(struct walk *w, const char *src, const char *dst, const struct stat *st)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return fail(w, "reading", src);
    int outfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (outfd < 0) { close(in); return fail(w, "creating", dst); }
    int r = 0;
    for (;;) {
        ssize_t n = read(in, w->buf, MIB);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { r = fail(w, "reading", src); break; }
        if (!n) break;
        for (ssize_t o = 0; o < n;) {
            ssize_t k = write(outfd, w->buf + o, (size_t)(n - o));
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { r = fail(w, "writing", dst); break; }
            o += k;
        }
        if (r) break;
        w->pr.bytes += (uint64_t)n;
        report(w, false);
    }
    close(in);
    if (close(outfd) < 0 && !r) r = fail(w, "writing", dst);
    if (!r) attrs(dst, st, false);
    return r;
}

static int walk_dir(struct walk *w, const char *src, const char *dst, const char *rel)
{
    DIR *d = opendir(src);
    if (!d) return fail(w, "reading the directory", src);
    struct dirent *e;
    int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *s = NULL, *t = NULL, *rr = NULL;
        if (asprintf(&s, "%s/%s", strcmp(src, "/") ? src : "", e->d_name) < 0 || asprintf(&t, "%s/%s", dst, e->d_name) < 0 ||
            asprintf(&rr, "%s/%s", rel, e->d_name) < 0) { free(s); free(t); free(rr); r = -1; break; }
        struct stat st;
        if (lstat(s, &st) < 0) { ilog("lstat %s: %s (left out)", s, strerror(errno)); free(s); free(t); free(rr); continue; }
        w->pr.current = rr;
        if (S_ISDIR(st.st_mode)) {
            if (mkdir(t, 0700) < 0 && errno != EEXIST) r = fail(w, "making", t);
            else {
                if (!skipped(w, rr)) r = walk_dir(w, s, t, rr);
                if (!r) attrs(t, &st, false);
            }
        } else if (S_ISREG(st.st_mode)) {
            const char *first = st.st_nlink > 1 ? find_hl(w, &st) : NULL;
            if (first) {
                if (link(first, t) < 0) r = copy_file(w, s, t, &st);   /* a different file system after all */
            } else {
                r = copy_file(w, s, t, &st);
                if (!r && st.st_nlink > 1) add_hl(w, &st, t);
            }
            w->pr.files++;
        } else if (S_ISLNK(st.st_mode)) {
            ssize_t n = readlink(s, w->buf, MIB - 1);
            if (n < 0) r = fail(w, "reading the link", s);
            else {
                w->buf[n] = 0;
                if (symlink(w->buf, t) < 0 && errno != EEXIST) r = fail(w, "making the link", t);
                else attrs(t, &st, true);
            }
            w->pr.files++;
        } else if (S_ISFIFO(st.st_mode)) {
            if (mkfifo(t, st.st_mode & 07777) == 0) attrs(t, &st, false);
        }                                       /* devices and sockets: not copied */
        w->pr.current = NULL;
        free(s); free(t); free(rr);
    }
    closedir(d);
    return r;
}

int copy_tree(const char *src, const char *dst, const char *const *skip, copy_cb cb, void *ctx,
              char *err, size_t errcap)
{
    struct walk w = { .skip = skip, .cb = cb, .ctx = ctx, .err = err, .errcap = errcap };
    w.buf = malloc(MIB);
    if (!w.buf) { snprintf(err, errcap, "out of memory"); return -1; }
    w.pr.total = tree_size(src, skip, NULL);
    int r = walk_dir(&w, src, dst, "");
    if (!r) {
        struct stat st;
        if (stat(src, &st) == 0) attrs(dst, &st, false);
    }
    report(&w, true);
    for (size_t i = 0; i < w.nhl; i++) free(w.hl[i].path);
    free(w.hl);
    free(w.buf);
    return r;
}

static uint64_t size_walk(const char *src, const char *rel, const char *const *skip, uint64_t *files, int depth)
{
    if (depth > 64) return 0;
    DIR *d = opendir(src);
    if (!d) return 0;
    uint64_t total = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *s = NULL, *rr = NULL;
        if (asprintf(&s, "%s/%s", strcmp(src, "/") ? src : "", e->d_name) < 0) break;
        if (asprintf(&rr, "%s/%s", rel, e->d_name) < 0) { free(s); break; }
        struct stat st;
        if (lstat(s, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                bool skip_it = false;
                for (int i = 0; skip && skip[i]; i++) if (!strcmp(skip[i], rr)) skip_it = true;
                if (!skip_it) total += size_walk(s, rr, skip, files, depth + 1);
            } else {
                if (S_ISREG(st.st_mode) && st.st_nlink == 1) total += (uint64_t)st.st_size;
                else if (S_ISREG(st.st_mode)) total += (uint64_t)st.st_size / st.st_nlink;   /* hard links: once, roughly */
                if (files) (*files)++;
            }
        }
        free(s);
        free(rr);
    }
    closedir(d);
    return total;
}

uint64_t tree_size(const char *src, const char *const *skip, uint64_t *files)
{
    if (files) *files = 0;
    return size_walk(src, "", skip, files, 0);
}
