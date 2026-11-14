/* kernel/ns.c -- namespaces and capabilities (ns.h)
 *
 * Written from the Linux interfaces (clone(2), unshare(2),
 * user_namespaces(7), network_namespaces(7), capabilities(7)).
 *
 * Every task points at one namespace of each kind; NULL is the initial one,
 * so kernel threads and early tasks need no setup. Namespaces are
 * reference-counted by the tasks in them and by the namespaces they own; a
 * user namespace holds its parent. Kernel ids (in tcb->uid and on files)
 * are always those of the initial user namespace; a user namespace's maps
 * translate them to what its processes see. */
#include <kernel/ns.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <kernel/sysvshm.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <kernel/string.h>
#include <kernel/klog.h>
#include <kernel/posix.h>
#include <kernel/bootinfo.h>

#define MAX_EXTENTS      5              /* Linux's base size; Firefox writes one */
#define MAX_USERNS_LEVEL 32
#define NS_INIT_INO_USER 4026531837ull  /* Linux's numbers for the initial ones */
#define NS_INIT_INO_UTS  4026531838ull
#define NS_INIT_INO_IPC  4026531839ull
#define NS_INIT_INO_PID  4026531836ull
#define NS_INIT_INO_CGRP 4026531835ull
#define NS_INIT_INO_TIME 4026531834ull
#define NS_INIT_INO_NET  4026531840ull
#define NS_INIT_INO_MNT  4026531841ull

struct extent { uint32_t first, lower, count; };
struct idmap { int n; struct extent e[MAX_EXTENTS]; };

struct user_ns {
    int      refs;
    struct user_ns *parent;
    int      level;
    uint32_t owner, group;              /* kernel ids of its creator */
    uint64_t ino;
    struct idmap uid, gid;              /* lower: ids of the parent namespace */
    bool     setgroups_deny;
    bool     gid_written;
};

struct net_ns { int refs; uint64_t ino; struct user_ns *owner; };
struct ipc_ns { int refs; uint64_t ino; struct user_ns *owner; };
struct uts_ns { int refs; uint64_t ino; struct user_ns *owner; char host[65], domain[65]; };

static struct user_ns init_user = { .refs = 1, .ino = NS_INIT_INO_USER,
                                     .uid = { 1, { { 0, 0, 0xffffffffu } } }, .gid = { 1, { { 0, 0, 0xffffffffu } } } };
static struct uts_ns init_uts = { .refs = 1, .ino = NS_INIT_INO_UTS, .host = KESTREL_HOSTNAME, .domain = "(none)" };
static uint64_t next_ino = 4026532000ull;
static struct spinlock ns_lock;         /* uts names, maps, setgroups */

static uint64_t new_ino(void) { return __atomic_fetch_add(&next_ino, 1, __ATOMIC_RELAXED); }

struct user_ns *task_userns(const struct tcb *t) { return t && t->userns ? t->userns : &init_user; }
struct net_ns *task_netns(const struct tcb *t) { return t ? t->netns : NULL; }
bool netns_initial(const struct tcb *t) { return !t || !t->netns; }
uint64_t netns_id(const struct tcb *t) { return t && t->netns ? t->netns->ino : 0; }
struct ipc_ns *task_ipcns(const struct tcb *t) { return t ? t->ipcns : NULL; }
uint64_t ipcns_id(const struct tcb *t) { return t && t->ipcns ? t->ipcns->ino : 0; }
static struct uts_ns *task_utsns(const struct tcb *t) { return t && t->utsns ? t->utsns : &init_uts; }

/* ---- reference counts --------------------------------------------------- */
static void user_get(struct user_ns *u) { if (u && u != &init_user) __atomic_add_fetch(&u->refs, 1, __ATOMIC_RELAXED); }
static void user_put(struct user_ns *u)
{
    while (u && u != &init_user && __atomic_sub_fetch(&u->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        struct user_ns *p = u->parent;
        procfs_ns_gone(u->ino);
        kfree(u);
        u = p;
    }
}
#define SIMPLE_PUT(type, extra)                                                        \
    static void type##_get(struct type *n) { if (n) __atomic_add_fetch(&n->refs, 1, __ATOMIC_RELAXED); } \
    static void type##_put(struct type *n)                                             \
    {                                                                                  \
        if (!n || __atomic_sub_fetch(&n->refs, 1, __ATOMIC_ACQ_REL)) return;          \
        extra;                                                                         \
        procfs_ns_gone(n->ino);                                                        \
        user_put(n->owner);                                                            \
        kfree(n);                                                                      \
    }
SIMPLE_PUT(net_ns, )
SIMPLE_PUT(ipc_ns, sysv_shm_ns_gone(n->ino))
static void uts_ns_get(struct uts_ns *n) { if (n && n != &init_uts) __atomic_add_fetch(&n->refs, 1, __ATOMIC_RELAXED); }
static void uts_ns_put(struct uts_ns *n)
{
    if (!n || n == &init_uts || __atomic_sub_fetch(&n->refs, 1, __ATOMIC_ACQ_REL)) return;
    procfs_ns_gone(n->ino);
    user_put(n->owner);
    kfree(n);
}

/* ---- capabilities -------------------------------------------------------------- */
/* Linux's cap_capable(): the capability in ns itself, or being the owner
 * of ns (or of a namespace above it) while in its parent. */
static bool has_cap(const struct tcb *t, struct user_ns *ns, int cap)
{
    struct user_ns *mine = task_userns(t);
    for (;;) {
        if (ns == mine) return (t->cap_eff >> cap) & 1;
        if (ns == &init_user || !ns->parent) return false;
        if (ns->parent == mine && ns->owner == t->uid) return true;
        ns = ns->parent;
    }
}

bool ns_capable(struct user_ns *ns, int cap)
{
    struct tcb *t = current_task();
    if (!t || !t->user) return true;                    /* the kernel itself */
    return has_cap(t, ns ? ns : &init_user, cap);
}

bool capable(int cap) { return ns_capable(&init_user, cap); }

void caps_exec(struct tcb *t)
{
    /* execve() keeps the user namespace's notion of root: uid 0 there gets
     * every capability, anyone else none (no file capabilities) */
    bool root = uid_view(t, t->uid) == 0;
    t->cap_prm = t->cap_eff = root ? CAP_FULL_SET : 0;
    t->cap_inh &= t->cap_prm;
}

void caps_setuid(struct tcb *t, uint32_t old)
{
    uint32_t root = (uint32_t)uid_kernel(t, 0);
    if (old == root && t->uid != root) t->cap_prm = t->cap_eff = 0;    /* leaving root */
    else if (old != root && t->uid == root) t->cap_eff = t->cap_prm;
}

int64_t caps_get(int pid, uint32_t out[6])
{
    struct tcb *t = pid ? task_find(pid) : current_task();
    if (!t) return -ESRCH;
    uint64_t e = t->cap_eff, p = t->cap_prm, i = t->cap_inh;
    out[0] = (uint32_t)e; out[1] = (uint32_t)p; out[2] = (uint32_t)i;
    out[3] = (uint32_t)(e >> 32); out[4] = (uint32_t)(p >> 32); out[5] = (uint32_t)(i >> 32);
    return 0;
}

/* capset(): permitted may only shrink, effective stays inside it, and the
 * inheritable set inside both the old inheritable and permitted ones. */
int64_t caps_set(const uint32_t in[6], int words)
{
    struct tcb *t = current_task();
    uint64_t e = in[0], p = in[1], i = in[2];
    if (words > 1) { e |= (uint64_t)in[3] << 32; p |= (uint64_t)in[4] << 32; i |= (uint64_t)in[5] << 32; }
    e &= CAP_FULL_SET; p &= CAP_FULL_SET; i &= CAP_FULL_SET;
    if (p & ~t->cap_prm) return -EPERM;
    if (e & ~p) return -EPERM;
    if (i & ~(t->cap_inh | t->cap_prm)) return -EPERM;
    t->cap_eff = e; t->cap_prm = p; t->cap_inh = i;
    return 0;
}

/* ---- id maps ------------------------------------------------------------------------ */
static int64_t map_down(const struct idmap *m, uint32_t id)    /* ns id -> parent's id */
{
    for (int i = 0; i < m->n; i++)
        if (id >= m->e[i].first && id - m->e[i].first < m->e[i].count) return m->e[i].lower + (id - m->e[i].first);
    return -1;
}
static int64_t map_up(const struct idmap *m, uint32_t id)      /* parent's id -> ns id */
{
    for (int i = 0; i < m->n; i++)
        if (id >= m->e[i].lower && id - m->e[i].lower < m->e[i].count) return m->e[i].first + (id - m->e[i].lower);
    return -1;
}

static int64_t from_kernel(const struct user_ns *ns, bool gid, uint32_t k)
{
    if (ns == &init_user) return k;
    int64_t up = from_kernel(ns->parent, gid, k);
    return up < 0 ? -1 : map_up(gid ? &ns->gid : &ns->uid, (uint32_t)up);
}
static int64_t to_kernel(const struct user_ns *ns, bool gid, uint32_t id)
{
    for (; ns != &init_user; ns = ns->parent) {
        int64_t d = map_down(gid ? &ns->gid : &ns->uid, id);
        if (d < 0) return -1;
        id = (uint32_t)d;
    }
    return id;
}

uint32_t uid_view(const struct tcb *t, uint32_t k) { int64_t v = from_kernel(task_userns(t), false, k); return v < 0 ? OVERFLOW_UID : (uint32_t)v; }
uint32_t gid_view(const struct tcb *t, uint32_t k) { int64_t v = from_kernel(task_userns(t), true, k); return v < 0 ? OVERFLOW_UID : (uint32_t)v; }
int64_t uid_kernel(const struct tcb *t, uint32_t id) { return to_kernel(task_userns(t), false, id); }
int64_t gid_kernel(const struct tcb *t, uint32_t id) { return to_kernel(task_userns(t), true, id); }

bool setgroups_allowed(const struct tcb *t)
{
    for (const struct user_ns *ns = task_userns(t); ns != &init_user; ns = ns->parent)
        if (ns->setgroups_deny || !ns->gid_written) return false;
    return true;
}

/* ---- making namespaces ------------------------------------------------------------- */
static struct user_ns *user_new(const struct tcb *p, int *err)
{
    struct user_ns *parent = task_userns(p);
    if (parent->level + 1 > MAX_USERNS_LEVEL) { *err = -EUSERS; return NULL; }
    /* Linux: the creator's ids must have a mapping in its own namespace,
     * and a chrooted process may not (it could escape through the new
     * namespace's capabilities) */
    if (from_kernel(parent, false, p->uid) < 0 || from_kernel(parent, true, p->gid) < 0) { *err = -EPERM; return NULL; }
    if (p->fs && p->fs->root) { *err = -EPERM; return NULL; }
    struct user_ns *u = kzalloc(sizeof *u);
    if (!u) { *err = -ENOMEM; return NULL; }
    u->refs = 1;
    u->parent = parent;
    user_get(parent);
    u->level = parent->level + 1;
    u->owner = p->uid;
    u->group = p->gid;
    u->ino = new_ino();
    return u;
}

/* Namespaces per flags for a task whose user namespace is `user` (the new
 * one, if it is being made too), into s; s->user already set. */
static int make_others(const struct tcb *p, uint64_t flags, struct ns_set *s)
{
    /* the others need CAP_SYS_ADMIN in the user namespace that will own them */
    if ((flags & (CLONE_NEWNET | CLONE_NEWIPC | CLONE_NEWUTS)) && !s->new_user && !ns_capable(s->user, CAP_SYS_ADMIN))
        return -EPERM;
    if (flags & CLONE_NEWNET) {
        struct net_ns *n = kzalloc(sizeof *n);
        if (!n) return -ENOMEM;
        n->refs = 1; n->ino = new_ino(); n->owner = s->user; user_get(s->user);
        s->net = n;
    } else { s->net = p->netns; net_ns_get(s->net); }
    if (flags & CLONE_NEWIPC) {
        struct ipc_ns *n = kzalloc(sizeof *n);
        if (!n) return -ENOMEM;
        n->refs = 1; n->ino = new_ino(); n->owner = s->user; user_get(s->user);
        s->ipc = n;
    } else { s->ipc = p->ipcns; ipc_ns_get(s->ipc); }
    if (flags & CLONE_NEWUTS) {
        struct uts_ns *n = kzalloc(sizeof *n);
        if (!n) return -ENOMEM;
        struct uts_ns *from = task_utsns(p);
        uint64_t fl = spin_lock_irqsave(&ns_lock);
        memcpy(n->host, from->host, sizeof n->host);
        memcpy(n->domain, from->domain, sizeof n->domain);
        spin_unlock_irqrestore(&ns_lock, fl);
        n->refs = 1; n->ino = new_ino(); n->owner = s->user; user_get(s->user);
        s->uts = n;
    } else { s->uts = p->utsns; uts_ns_get(s->uts); }
    return 0;
}

void ns_abort(struct ns_set *s)
{
    user_put(s->new_user ? s->user : NULL);
    net_ns_put(s->net); ipc_ns_put(s->ipc); uts_ns_put(s->uts);
    memset(s, 0, sizeof *s);
}

int ns_prepare(const struct tcb *p, uint64_t flags, struct ns_set *s)
{
    memset(s, 0, sizeof *s);
    if (flags & (CLONE_NS_ALL & ~CLONE_NS_SUPPORTED)) return -EINVAL;
    int err = 0;
    if (flags & CLONE_NEWUSER) {
        if (!(s->user = user_new(p, &err))) return err;
        s->new_user = true;
    } else {
        s->user = p->userns;                            /* NULL: the initial one */
    }
    if (!s->user) s->user = &init_user;
    if ((err = make_others(p, flags, s))) { ns_abort(s); return err; }
    return 0;
}

void ns_install(struct tcb *c, struct ns_set *s)
{
    struct user_ns *u = s->user == &init_user ? NULL : s->user;
    if (!s->new_user) user_get(u);                      /* the child's reference */
    c->userns = u;
    c->netns = s->net; c->ipcns = s->ipc; c->utsns = s->uts == &init_uts ? NULL : s->uts;
    if (s->new_user) c->cap_eff = c->cap_prm = CAP_FULL_SET;   /* all of them, inside it */
    memset(s, 0, sizeof *s);
}

void ns_release(struct tcb *t)
{
    uint64_t fl = spin_lock_irqsave(&proc_lock);        /* /proc readers look at them */
    struct user_ns *u = t->userns; struct net_ns *n = t->netns;
    struct ipc_ns *i = t->ipcns; struct uts_ns *s = t->utsns;
    t->userns = NULL; t->netns = NULL; t->ipcns = NULL; t->utsns = NULL;
    spin_unlock_irqrestore(&proc_lock, fl);
    user_put(u); net_ns_put(n); ipc_ns_put(i); uts_ns_put(s);
}

int64_t ns_unshare(uint64_t flags)
{
    struct tcb *t = current_task();
    flags &= CLONE_NS_ALL;
    if (!flags) return 0;
    struct ns_set s;
    int r = ns_prepare(t, flags, &s);
    if (r) return r;
    uint64_t fl = spin_lock_irqsave(&proc_lock);
    struct user_ns *ou = t->userns; struct net_ns *on = t->netns;
    struct ipc_ns *oi = t->ipcns; struct uts_ns *os = t->utsns;
    bool new_user = s.new_user;
    ns_install(t, &s);
    spin_unlock_irqrestore(&proc_lock, fl);
    user_put(ou); net_ns_put(on); ipc_ns_put(oi); uts_ns_put(os);
    (void)new_user;
    return 0;
}

/* ---- UTS ---------------------------------------------------------------------------- */
void uts_names(const struct tcb *t, char host[65], char domain[65])
{
    struct uts_ns *u = task_utsns(t);
    uint64_t fl = spin_lock_irqsave(&ns_lock);
    memcpy(host, u->host, 65);
    memcpy(domain, u->domain, 65);
    spin_unlock_irqrestore(&ns_lock, fl);
}

int64_t uts_set(bool host, const char *name, uint64_t len)
{
    struct tcb *t = current_task();
    struct uts_ns *u = task_utsns(t);
    if (!ns_capable(u == &init_uts ? &init_user : u->owner, CAP_SYS_ADMIN)) return -EPERM;
    if (len > 64) return -EINVAL;
    uint64_t fl = spin_lock_irqsave(&ns_lock);
    char *dst = host ? u->host : u->domain;
    memcpy(dst, name, len);
    dst[len] = 0;
    spin_unlock_irqrestore(&ns_lock, fl);
    return 0;
}

/* ---- /proc ---------------------------------------------------------------------------- */
uint64_t ns_ino(const struct tcb *t, const char *kind)
{
    if (!strcmp(kind, "user")) return task_userns(t)->ino;
    if (!strcmp(kind, "net")) return t && t->netns ? t->netns->ino : NS_INIT_INO_NET;
    if (!strcmp(kind, "ipc")) return t && t->ipcns ? t->ipcns->ino : NS_INIT_INO_IPC;
    if (!strcmp(kind, "uts")) return task_utsns(t)->ino;
    if (!strcmp(kind, "pid") || !strcmp(kind, "pid_for_children")) return NS_INIT_INO_PID;
    if (!strcmp(kind, "mnt")) return NS_INIT_INO_MNT;
    if (!strcmp(kind, "cgroup")) return NS_INIT_INO_CGRP;
    if (!strcmp(kind, "time") || !strcmp(kind, "time_for_children")) return NS_INIT_INO_TIME;
    return 0;
}

int ns_link_text(const struct tcb *t, const char *kind, char *buf, int size)
{
    const char *k = !strcmp(kind, "pid_for_children") ? "pid" : !strcmp(kind, "time_for_children") ? "time" : kind;
    int n = snprintf(buf, (size_t)size, "%s:[%lu]", k, (unsigned long)ns_ino(t, kind));
    return n < size ? n : size;
}

int ns_map_show(const struct tcb *t, int which, char *buf, int cap)
{
    struct user_ns *u = task_userns(t);
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&ns_lock);
    if (which == 2) {
        n = snprintf(buf, (size_t)cap, "%s\n", u->setgroups_deny ? "deny" : "allow");
    } else {
        const struct idmap *m = which ? &u->gid : &u->uid;
        for (int i = 0; i < m->n && n < cap; i++)
            n += snprintf(buf + n, (size_t)(cap - n), "%10u %10u %10u\n", m->e[i].first, m->e[i].lower, m->e[i].count);
    }
    spin_unlock_irqrestore(&ns_lock, fl);
    return n < cap ? n : cap;
}

static bool parse_u32(const char **p, uint32_t *out)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return false;
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s++ - '0'); if (v > 0xffffffffull) return false; }
    *out = (uint32_t)v;
    *p = s;
    return true;
}

/* Linux's map_write() and new_idmap_permitted(): once; by a process with
 * CAP_SYS_ADMIN in the namespace, in it or its parent; one line mapping the
 * writer's own id needs nothing more (for gids, after "deny" in
 * setgroups), anything else CAP_SETUID/CAP_SETGID in the parent. The lower
 * ids must exist in the parent namespace. */
int ns_map_write(struct tcb *target, int which, const char *buf, uint64_t len)
{
    struct tcb *w = current_task();
    struct user_ns *u = task_userns(target);
    if (u == &init_user) return -EPERM;
    struct user_ns *mine = task_userns(w);
    if (mine != u && mine != u->parent) return -EPERM;
    if (which == 2) {                                   /* setgroups */
        bool deny;
        if (len >= 4 && !memcmp(buf, "deny", 4) && (len == 4 || buf[4] == '\n')) deny = true;
        else if (len >= 5 && !memcmp(buf, "allow", 5) && (len == 5 || buf[5] == '\n')) deny = false;
        else return -EINVAL;
        if (!ns_capable(u, CAP_SYS_ADMIN)) return -EPERM;
        uint64_t fl = spin_lock_irqsave(&ns_lock);
        int r = 0;
        if (u->gid_written || (u->setgroups_deny && !deny)) r = -EPERM;
        else u->setgroups_deny = deny;
        spin_unlock_irqrestore(&ns_lock, fl);
        return r ? r : (int)len;
    }
    char text[256];
    if (len >= sizeof text) return -EINVAL;
    memcpy(text, buf, len);
    text[len] = 0;
    struct idmap m = { 0 };
    for (const char *p = text; *p;) {
        while (*p == '\n' || *p == ' ') p++;
        if (!*p) break;
        if (m.n == MAX_EXTENTS) return -EINVAL;
        struct extent *e = &m.e[m.n];
        if (!parse_u32(&p, &e->first) || !parse_u32(&p, &e->lower) || !parse_u32(&p, &e->count)) return -EINVAL;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '\n') return -EINVAL;
        if (!e->count || (uint64_t)e->first + e->count > 0xffffffffull || (uint64_t)e->lower + e->count > 0xffffffffull)
            return -EINVAL;
        for (int i = 0; i < m.n; i++) {                 /* no overlaps, either side */
            const struct extent *o = &m.e[i];
            if ((e->first < o->first + o->count && o->first < e->first + e->count) ||
                (e->lower < o->lower + o->count && o->lower < e->lower + e->count)) return -EINVAL;
        }
        m.n++;
    }
    if (!m.n) return -EINVAL;
    if (!ns_capable(u, CAP_SYS_ADMIN)) return -EPERM;
    bool gid = which == 1;
    for (int i = 0; i < m.n; i++)                       /* the lower ids, in the parent */
        for (uint32_t k = 0; k < m.e[i].count && k < 2; k++)
            if (to_kernel(u->parent, gid, m.e[i].lower + (k ? m.e[i].count - 1 : 0)) < 0) return -EPERM;
    bool self = m.n == 1 && m.e[0].count == 1 &&
                to_kernel(u->parent, gid, m.e[0].lower) == (gid ? w->gid : w->uid);
    uint64_t fl = spin_lock_irqsave(&ns_lock);
    bool deny = u->setgroups_deny;
    spin_unlock_irqrestore(&ns_lock, fl);
    bool ok = (self && (!gid || deny)) || has_cap(w, u->parent, gid ? CAP_SETGID : CAP_SETUID);
    if (!ok) return -EPERM;
    fl = spin_lock_irqsave(&ns_lock);
    struct idmap *dst = gid ? &u->gid : &u->uid;
    int r = 0;
    if (dst->n) r = -EPERM;                             /* written once */
    else { *dst = m; if (gid) u->gid_written = true; }
    spin_unlock_irqrestore(&ns_lock, fl);
    return r ? r : (int)len;
}
