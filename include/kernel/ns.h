/* =============================================================================
 *  ns.h -- namespaces and capabilities (kernel/ns.c)
 *
 *  What Firefox's and Chromium's sandboxes build on, after seccomp: a child
 *  started with clone(CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWIPC) (or that
 *  calls unshare()) gets namespaces of its own --
 *
 *  - a user namespace: its own uid and gid numbers (/proc/PID/uid_map,
 *    gid_map, setgroups, written once) and a full set of capabilities that
 *    count only inside it -- enough to chroot(), never to reboot the machine,
 *    mount, or touch other users' files;
 *  - a network namespace: no network at all (its loopback is down, as on
 *    Linux) and its own abstract AF_UNIX names; named AF_UNIX sockets, such
 *    as the X server's, still work;
 *  - an IPC namespace: its own System V shared memory;
 *  - a UTS namespace: its own host name.
 *
 *  /proc/PID/ns names each namespace ("net:[4026531840]"). Not here: PID,
 *  mount, cgroup and time namespaces (clone and unshare refuse them with
 *  EINVAL) and setns().
 *
 *  Capabilities are Linux's: an effective, a permitted and an inheritable
 *  set per thread. Root starts with all of them, other users with none;
 *  capset() drops them, execve() gives them back to root (in its user
 *  namespace) and takes them from everyone else, and a uid change away from
 *  root clears them. Where the kernel used to ask "is this uid 0?" it now
 *  asks capable(CAP_...): the capability, in the initial user namespace.
 * ============================================================================= */
#ifndef KESTREL_NS_H
#define KESTREL_NS_H

#include <stdint.h>
#include <stdbool.h>

struct tcb;

#define CAP_CHOWN            0
#define CAP_DAC_OVERRIDE     1
#define CAP_FOWNER           3
#define CAP_KILL             5
#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_ADMIN        12
#define CAP_NET_RAW          13
#define CAP_IPC_OWNER        15
#define CAP_SYS_CHROOT       18
#define CAP_SYS_ADMIN        21
#define CAP_SYS_BOOT         22
#define CAP_SYS_NICE         23
#define CAP_LAST_CAP         40
#define CAP_FULL_SET         ((1ull << (CAP_LAST_CAP + 1)) - 1)

#define CLONE_NEWTIME        0x00000080
#define CLONE_NEWNS          0x00020000
#define CLONE_NEWCGROUP      0x02000000
#define CLONE_NEWUTS         0x04000000
#define CLONE_NEWIPC         0x08000000
#define CLONE_NEWUSER        0x10000000
#define CLONE_NEWPID         0x20000000
#define CLONE_NEWNET         0x40000000
#define CLONE_NS_SUPPORTED   (CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWNET)
#define CLONE_NS_ALL         (CLONE_NEWTIME | CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | \
                              CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)

#define OVERFLOW_UID         65534      /* an id with no mapping in a namespace */

struct user_ns;
struct net_ns;
struct ipc_ns;
struct uts_ns;

/* ---- capabilities ------------------------------------------------------- */
bool capable(int cap);                          /* current thread, initial user namespace */
bool ns_capable(struct user_ns *ns, int cap);   /* ... in ns (its own, or one it owns)    */
void caps_exec(struct tcb *t);                  /* execve: root gets them all, others none */
void caps_setuid(struct tcb *t, uint32_t old_kuid);    /* after a uid change */
int64_t caps_get(int pid, uint32_t out[6]);     /* capget: 2 words of {eff, prm, inh} */
int64_t caps_set(const uint32_t in[6], int words);

/* ---- ids ---------------------------------------------------------------------- */
/* Kernel ids are those of the initial namespace (the ones files carry). */
struct user_ns *task_userns(const struct tcb *t);
uint32_t uid_view(const struct tcb *t, uint32_t kuid);      /* as t sees it (65534: unmapped) */
uint32_t gid_view(const struct tcb *t, uint32_t kgid);
int64_t  uid_kernel(const struct tcb *t, uint32_t uid);     /* t's uid to a kernel one; -1: unmapped */
int64_t  gid_kernel(const struct tcb *t, uint32_t gid);
bool     setgroups_allowed(const struct tcb *t);

/* ---- namespaces of a task -------------------------------------------------------- */
/* clone(): namespaces for the child, made before it exists (so failure
 * leaves nothing behind), then given to it. */
struct ns_set {
    struct user_ns *user;
    struct net_ns  *net;
    struct ipc_ns  *ipc;
    struct uts_ns  *uts;
    bool new_user;
};
int  ns_prepare(const struct tcb *p, uint64_t flags, struct ns_set *out);   /* 0 or -errno */
void ns_install(struct tcb *c, struct ns_set *s);           /* proc_lock held */
void ns_abort(struct ns_set *s);
void ns_release(struct tcb *t);                             /* reap */
int64_t ns_unshare(uint64_t flags);                         /* the namespace part of unshare() */

struct net_ns *task_netns(const struct tcb *t);
bool netns_initial(const struct tcb *t);                    /* the machine's network */
uint64_t netns_id(const struct tcb *t);                     /* 0: the initial one */
struct ipc_ns *task_ipcns(const struct tcb *t);
uint64_t ipcns_id(const struct tcb *t);                     /* 0: the initial one */

/* UTS: the host and domain names (uname, sethostname, /proc/sys/kernel) */
void uts_names(const struct tcb *t, char host[65], char domain[65]);
int64_t uts_set(bool host, const char *name, uint64_t len);

/* /proc/PID: ns/<kind> links, uid_map, gid_map, setgroups */
int  ns_link_text(const struct tcb *t, const char *kind, char *buf, int size);
uint64_t ns_ino(const struct tcb *t, const char *kind);
int  ns_map_show(const struct tcb *t, int which, char *buf, int cap);  /* 0 uid, 1 gid, 2 setgroups */
int  ns_map_write(struct tcb *t, int which, const char *buf, uint64_t len);

#endif
