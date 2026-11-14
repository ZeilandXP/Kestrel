/* user/nstest.c -- namespaces, capabilities and chroot on Kestrel (musl)
 *
 * What Firefox's sandbox asks of the kernel before seccomp: user, network,
 * IPC and UTS namespaces from clone() and unshare(), /proc/PID/uid_map,
 * gid_map and setgroups with Linux's rules, ids seen through the maps,
 * capabilities that count only inside the namespace (chroot yes, mount and
 * the machine's host name no), capset and execve dropping them, the
 * /proc/PID/ns links, a network namespace with no network and abstract
 * AF_UNIX names of its own, System V shared memory of its own, and
 * Firefox's chroot helper, which chroots itself and its sandboxed sibling
 * into /proc/self/fdinfo. Run as root; the unprivileged cases switch to
 * uid 1000 in a child. Exit status 0 = every check passed. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <grp.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); \
                            if (!ok_) { failures++; printf("        (errno %d)\n", errno); } } while (0)

#define USER_UID 1000
#define USER_GID 1000
static char self_path[256];
static char tmpdir[64];

/* Run fn in a child; its exit status (or 128 + the signal that ended it). */
static int in_child(int (*fn)(void))
{
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) { failures = 0; int r = fn(); fflush(stdout); _exit(r); }
    int st = 0;
    if (waitpid(p, &st, 0) != p) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* clone() without a new stack, the way Firefox starts a sandboxed child */
static pid_t clone_flags(unsigned long flags) { return (pid_t)syscall(SYS_clone, flags | SIGCHLD, 0, 0, 0, 0); }

static int wait_status(pid_t p)
{
    int st = 0;
    if (p <= 0 || waitpid(p, &st, 0) != p) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int put(const char *path, const char *text)      /* 0 or errno */
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return errno;
    ssize_t n = write(fd, text, strlen(text));
    int e = n == (ssize_t)strlen(text) ? 0 : n < 0 ? errno : EIO;
    close(fd);
    return e;
}

static int get(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

/* Firefox's ConfigureUserNamespace: "uid uid 1", setgroups "deny", "gid gid 1" */
static int map_self(uid_t uid, gid_t gid)
{
    char b[64];
    snprintf(b, sizeof b, "%u %u 1", (unsigned)uid, (unsigned)uid);
    if (put("/proc/self/uid_map", b)) return -1;
    if (put("/proc/self/setgroups", "deny")) return -2;
    snprintf(b, sizeof b, "%u %u 1", (unsigned)gid, (unsigned)gid);
    if (put("/proc/self/gid_map", b)) return -3;
    return 0;
}

/* capget/capset, version 3 */
struct cap_hdr { uint32_t version; int pid; };
struct cap_data { uint32_t eff, prm, inh; };
static int caps_get(uint64_t *eff, uint64_t *prm)
{
    struct cap_hdr h = { 0x20080522, 0 };
    struct cap_data d[2];
    if (syscall(SYS_capget, &h, d)) return -1;
    if (eff) *eff = d[0].eff | (uint64_t)d[1].eff << 32;
    if (prm) *prm = d[0].prm | (uint64_t)d[1].prm << 32;
    return 0;
}
static int caps_set(uint64_t eff, uint64_t prm)
{
    struct cap_hdr h = { 0x20080522, 0 };
    struct cap_data d[2] = { { (uint32_t)eff, (uint32_t)prm, 0 }, { (uint32_t)(eff >> 32), (uint32_t)(prm >> 32), 0 } };
    return (int)syscall(SYS_capset, &h, d);
}
#define CAP_SYS_CHROOT 18
#define CAP_SYS_ADMIN  21

static int become_user(void)
{
    if (setgroups(0, NULL) || setgid(USER_GID) || setuid(USER_UID)) return -1;
    prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);         /* a uid change made /proc/self root's */
    return 0;
}

static int ns_ino(const char *kind, char *text, size_t cap)    /* the link's text; the inode in it */
{
    char path[64], b[64];
    snprintf(path, sizeof path, "/proc/self/ns/%s", kind);
    ssize_t n = readlink(path, b, sizeof b - 1);
    if (n <= 0) return -1;
    b[n] = 0;
    if (text) snprintf(text, cap, "%s", b);
    char *o = strchr(b, '[');
    return o ? (int)(strtoul(o + 1, NULL, 10) & 0x7fffffff) : -1;
}

/* ---- user namespaces --------------------------------------------------------------- */
static int t_root_maps(void)
{
    char path[96];
    snprintf(path, sizeof path, "%s/owned-by-1000", tmpdir);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) { if (fchown(fd, USER_UID, USER_GID)) {} close(fd); }
    CHECK(unshare(CLONE_NEWUSER) == 0, "root: unshare(CLONE_NEWUSER)");
    CHECK(getuid() == 65534 && getgid() == 65534, "before uid_map is written: uid and gid 65534");
    uint64_t eff = 0;
    CHECK(caps_get(&eff, NULL) == 0 && (eff & 0xffffffffu) == 0xffffffffu, "every capability, inside the new namespace");
    CHECK(put("/proc/self/uid_map", "0 1000 1\n") == EPERM, "from inside, only its own uid (no CAP_SETUID outside)");
    CHECK(put("/proc/self/uid_map", "0 0 1\n") == 0, "uid 0 maps to itself");
    CHECK(put("/proc/self/uid_map", "0 0 1\n") == EPERM, "uid_map is written once");
    CHECK(put("/proc/self/gid_map", "0 0 1\n") == EPERM, "gid_map before setgroups \"deny\": EPERM");
    CHECK(put("/proc/self/setgroups", "deny") == 0 && put("/proc/self/gid_map", "0 0 1\n") == 0, "after it: gid 0");
    CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0, "now uid and gid 0");
    struct stat st;
    CHECK(stat(path, &st) == 0 && st.st_uid == 65534 && st.st_gid == 65534, "a file of uid 1000 shows as 65534 (unmapped)");
    CHECK(stat("/", &st) == 0 && st.st_uid == 0, "a file of root shows as 0");
    CHECK(setuid(0) == 0, "setuid(0): mapped");
    errno = 0;
    CHECK(setuid(5) == -1 && errno == EINVAL, "setuid(5): EINVAL, not mapped");
    char b[256];
    CHECK(get("/proc/self/uid_map", b, sizeof b) > 0 && strtoul(b, NULL, 10) == 0, "uid_map reads back");
    CHECK(get("/proc/self/status", b, sizeof b) > 0, "/proc/self/status");
    unlink(path);
    return failures;
}

static int t_user_maps(void)
{
    if (become_user()) { printf("  FAIL  setuid(%d)\n", USER_UID); return 1; }
    uint64_t eff = 1;
    CHECK(caps_get(&eff, NULL) == 0 && eff == 0, "uid 1000: no capabilities");
    errno = 0;
    CHECK(chroot("/") == -1 && errno == EPERM, "uid 1000: chroot refused");
    errno = 0;
    CHECK(sethostname("x", 1) == -1 && errno == EPERM, "uid 1000: sethostname refused");
    CHECK(unshare(CLONE_NEWUSER) == 0, "uid 1000: unshare(CLONE_NEWUSER)");
    CHECK(caps_get(&eff, NULL) == 0 && (eff & 0xffffffffu) == 0xffffffffu, "every capability, inside the new namespace");
    CHECK(put("/proc/self/uid_map", "0 0 1") == EPERM, "may not map uid 0");
    CHECK(put("/proc/self/uid_map", "0 1000 2") == EPERM, "may not map more than itself");
    CHECK(put("/proc/self/gid_map", "1000 1000 1") == EPERM, "gid_map before setgroups \"deny\": EPERM");
    CHECK(map_self(USER_UID, USER_GID) == 0, "Firefox's maps: uid 1000, setgroups deny, gid 1000");
    char b[32];
    CHECK(get("/proc/self/setgroups", b, sizeof b) > 0 && !strncmp(b, "deny", 4), "setgroups reads \"deny\"");
    errno = 0;
    CHECK(setgroups(0, NULL) == -1 && errno == EPERM, "setgroups() then refused");
    CHECK(getuid() == USER_UID && getgid() == USER_GID, "uid and gid 1000 inside");
    errno = 0;
    CHECK(setuid(0) == -1 && errno == EINVAL, "setuid(0): EINVAL, not mapped");
    errno = 0;
    CHECK(mount("none", tmpdir, "tmpfs", 0, NULL) == -1 && errno == EPERM, "mount refused: no CAP_SYS_ADMIN outside");
    errno = 0;
    CHECK(sethostname("x", 1) == -1 && errno == EPERM, "the machine's host name: still refused");
    CHECK(chroot("/") == 0, "chroot allowed: CAP_SYS_CHROOT in its own namespace");
    CHECK(caps_set(0, 0) == 0 && caps_get(&eff, NULL) == 0 && eff == 0, "capset drops them all");
    errno = 0;
    CHECK(chroot("/") == -1 && errno == EPERM, "then chroot refused");
    errno = 0;
    CHECK(caps_set(1ull << CAP_SYS_CHROOT, 1ull << CAP_SYS_CHROOT) == -1 && errno == EPERM, "and they can't come back");
    return failures;
}

/* A user namespace's capabilities over its own children's: the parent
 * (outside) writes the child's map. */
static int t_nested(void)
{
    if (become_user()) return 1;
    CHECK(unshare(CLONE_NEWUSER) == 0 && map_self(USER_UID, USER_GID) == 0, "uid 1000 in a namespace of its own");
    int ino1 = ns_ino("user", NULL, 0);
    int pfd[2], qfd[2];
    if (pipe(pfd) || pipe(qfd)) return 1;
    pid_t c = fork();
    if (c == 0) {
        close(pfd[1]); close(qfd[0]);
        int ok = unshare(CLONE_NEWUSER) == 0;
        char x = ok ? 'y' : 'n';
        if (write(qfd[1], &x, 1)) {}
        if (read(pfd[0], &x, 1) != 1) _exit(2);
        _exit(ok && getuid() == 0 && ns_ino("user", NULL, 0) != ino1 ? 0 : 3);
    }
    close(pfd[0]); close(qfd[1]);
    char x = 0;
    CHECK(read(qfd[0], &x, 1) == 1 && x == 'y', "a namespace inside it");
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/uid_map", c);
    CHECK(put(path, "0 1000 1") == 0, "its parent maps the child's uid 0 to its own 1000");
    snprintf(path, sizeof path, "/proc/%d/gid_map", c);
    CHECK(put(path, "0 1000 1") == 0, "and gid 0 (the parent has CAP_SETGID there)");
    if (write(pfd[1], "g", 1)) {}
    CHECK(wait_status(c) == 0, "the child is root in it");
    return failures;
}

/* ---- Firefox's launch: clone(NEWUSER|NEWNET|NEWIPC), the maps, a chroot
 * helper sharing the fs (CLONE_FS), capabilities dropped, execve, then the
 * chroot into the helper's /proc/self/fdinfo --------------------------------- */
static int content_process(int fd)              /* after execve */
{
    uint64_t eff = 1, prm = 1;
    CHECK(caps_get(&eff, &prm) == 0 && eff == 0 && prm == 0, "content: no capabilities after execve");
    CHECK(getuid() == USER_UID && getgid() == USER_GID, "content: uid and gid 1000");
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(80), .sin_addr.s_addr = htonl(0x7f000001) };
    errno = 0;
    CHECK(s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == -1 && errno == ENETUNREACH, "content: no network");
    close(s);
    CHECK(access("/etc", F_OK) == 0, "content: the file system, before the chroot");
    char m = 'C';
    CHECK(write(fd, &m, 1) == 1 && read(fd, &m, 1) == 1 && m == 'O', "content: the helper chrooted us");
    errno = 0;
    CHECK(access("/etc", F_OK) == -1, "content: no /etc any more");
    errno = 0;
    CHECK(access("/../../etc", F_OK) == -1, "content: \"..\" doesn't climb out");
    errno = 0;
    CHECK(open("/x", O_CREAT | O_WRONLY, 0600) == -1, "content: can't create files in the new root");
    errno = 0;
    CHECK(chroot("/") == -1 && errno == EPERM, "content: can't chroot again");
    return failures;
}

static int t_firefox(void)
{
    if (become_user()) return 1;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return 1;
    pid_t c = clone_flags(CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWIPC);
    if (c == 0) {
        if (map_self(USER_UID, USER_GID)) _exit(10);
        pid_t h = clone_flags(CLONE_FS);
        if (h == 0) {                                           /* the chroot helper */
            close(sv[1]);
            if (caps_set(1ull << CAP_SYS_CHROOT, 1ull << CAP_SYS_CHROOT)) _exit(11);
            char m;
            if (read(sv[0], &m, 1) != 1 || m != 'C') _exit(12);
            if (chroot("/proc/self/fdinfo")) _exit(13);
            caps_set(0, 0);
            if (chdir("/")) _exit(14);
            m = 'O';
            if (write(sv[0], &m, 1) != 1) _exit(15);
            _exit(0);
        }
        if (h < 0) _exit(16);
        close(sv[0]);
        if (caps_set(0, 0)) _exit(17);
        char fdtext[16];
        snprintf(fdtext, sizeof fdtext, "%d", sv[1]);
        execl(self_path, self_path, "content", fdtext, (char *)NULL);
        _exit(18);
    }
    close(sv[1]);
    close(sv[0]);
    CHECK(c > 0, "clone(CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWIPC) as uid 1000");
    int st = wait_status(c);
    CHECK(st == 0, "the sandboxed child passed");
    if (st) printf("        (status %d)\n", st);
    return failures;
}

/* ---- network namespaces ---------------------------------------------------------- */
static int abstract_sock(const char *name, int listen_too, int conn)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    a.sun_path[0] = 0;
    strcpy(a.sun_path + 1, name);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name));
    if (conn) return connect(s, (struct sockaddr *)&a, len) ? (close(s), -1) : s;
    if (bind(s, (struct sockaddr *)&a, len) || (listen_too && listen(s, 4))) { close(s); return -1; }
    return s;
}

static char abs_name[64], unix_path[96];
static int t_netns(void)
{
    char before[64], after[64];
    int ino = ns_ino("net", before, sizeof before);
    CHECK(unshare(CLONE_NEWUSER | CLONE_NEWNET) == 0, "unshare(CLONE_NEWUSER | CLONE_NEWNET)");
    CHECK(ns_ino("net", after, sizeof after) != ino && !strncmp(after, "net:[", 5), "/proc/self/ns/net changed");
    errno = 0;
    CHECK(abstract_sock(abs_name, 0, 1) == -1 && errno == ECONNREFUSED, "the outside's abstract AF_UNIX name: not here");
    int l = abstract_sock(abs_name, 1, 0);
    CHECK(l >= 0, "the same abstract name can be bound here");
    int c = abstract_sock(abs_name, 0, 1);
    CHECK(c >= 0, "and connected to here");
    close(c); close(l);
    int u = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ua = { .sun_family = AF_UNIX };
    strcpy(ua.sun_path, unix_path);
    CHECK(connect(u, (struct sockaddr *)&ua, sizeof ua) == 0, "a named AF_UNIX socket (like the X server's) still works");
    close(u);

    int t = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(t >= 0, "socket(AF_INET, SOCK_STREAM)");
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x0a000001) };
    int t3 = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(bind(t3, (struct sockaddr *)&a, sizeof a) == 0, "bind to 10.0.0.1 (every interface is down; Linux allows it)");
    close(t3);
    a.sin_addr.s_addr = htonl(0x7f000001);
    CHECK(bind(t, (struct sockaddr *)&a, sizeof a) == 0 && listen(t, 4) == 0, "bind to 127.0.0.1 and listen (as on Linux)");
    socklen_t al = sizeof a;
    CHECK(getsockname(t, (struct sockaddr *)&a, &al) == 0 && a.sin_port != 0, "a port was given");
    fcntl(t, F_SETFL, O_NONBLOCK);
    errno = 0;
    CHECK(accept(t, NULL, NULL) == -1 && errno == EAGAIN, "nothing to accept");
    int t2 = socket(AF_INET, SOCK_STREAM, 0);
    a.sin_addr.s_addr = htonl(0x7f000001);
    errno = 0;
    CHECK(connect(t2, (struct sockaddr *)&a, sizeof a) == -1 && errno == ENETUNREACH, "connect to 127.0.0.1: ENETUNREACH");
    a.sin_addr.s_addr = htonl(0x01010101);
    errno = 0;
    CHECK(connect(t2, (struct sockaddr *)&a, sizeof a) == -1 && errno == ENETUNREACH, "connect to 1.1.1.1: ENETUNREACH");
    close(t2); close(t);
    int d = socket(AF_INET, SOCK_DGRAM, 0);
    a.sin_port = htons(53);
    errno = 0;
    CHECK(sendto(d, "x", 1, 0, (struct sockaddr *)&a, sizeof a) == -1 && errno == ENETUNREACH, "UDP sendto: ENETUNREACH");
    close(d);
    struct ifconf ifc;
    struct ifreq rq[8];
    ifc.ifc_len = sizeof rq;
    ifc.ifc_req = rq;
    d = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(ioctl(d, SIOCGIFCONF, &ifc) == 0 && ifc.ifc_len == 0, "SIOCGIFCONF: no addresses");
    close(d);
    return failures;
}

/* ---- IPC and UTS namespaces --------------------------------------------------------- */
static key_t shm_key;
static int shm_id;
static int t_ipcns(void)
{
    CHECK(shmget(shm_key, 0, 0) == shm_id, "before: the segment is there");
    CHECK(unshare(CLONE_NEWUSER | CLONE_NEWIPC) == 0, "unshare(CLONE_NEWUSER | CLONE_NEWIPC)");
    errno = 0;
    CHECK(shmget(shm_key, 0, 0) == -1 && errno == ENOENT, "the outside's segment: not here (by key)");
    struct shmid_ds ds;
    errno = 0;
    CHECK(shmctl(shm_id, IPC_STAT, &ds) == -1 && errno == EINVAL, "nor by id");
    int id = shmget(shm_key, 4096, IPC_CREAT | IPC_EXCL | 0600);
    CHECK(id >= 0, "a segment with the same key, here");
    void *p = id >= 0 ? shmat(id, NULL, 0) : (void *)-1;
    CHECK(p != (void *)-1, "attached");
    if (p != (void *)-1) { strcpy(p, "inner"); shmdt(p); }
    /* left behind on purpose: it goes when the namespace does */
    return failures;
}

static int t_utsns(void)
{
    CHECK(unshare(CLONE_NEWUSER | CLONE_NEWUTS) == 0, "unshare(CLONE_NEWUSER | CLONE_NEWUTS)");
    CHECK(sethostname("nstest-inner", 12) == 0, "sethostname in its own UTS namespace");
    struct utsname u;
    CHECK(uname(&u) == 0 && !strcmp(u.nodename, "nstest-inner"), "uname() shows it");
    char b[80];
    CHECK(get("/proc/sys/kernel/hostname", b, sizeof b) > 0 && !strncmp(b, "nstest-inner", 12), "and /proc/sys/kernel/hostname");
    return failures;
}

/* ---- /proc and refusals ------------------------------------------------------------- */
static void *idle_thread(void *a) { (void)a; pause(); return NULL; }
static int t_refusals(void)
{
    static const char *kinds[] = { "user", "net", "ipc", "uts", "pid", "mnt" };
    int good = 1;
    for (int i = 0; i < 6; i++) {
        char text[64], path[64];
        int ino = ns_ino(kinds[i], text, sizeof text);
        snprintf(path, sizeof path, "/proc/self/ns/%s", kinds[i]);
        struct stat st;
        if (ino <= 0 || strncmp(text, kinds[i], strlen(kinds[i])) || stat(path, &st) || (int)(st.st_ino & 0x7fffffff) != ino) {
            printf("        %s: \"%s\"\n", kinds[i], text);
            good = 0;
        }
    }
    CHECK(good, "/proc/self/ns links: \"kind:[inode]\", and stat() gives that inode");
    CHECK(access("/proc/self/ns/user", F_OK) == 0 && access("/proc/self/ns/pid", F_OK) == 0, "Firefox's probe: ns/user, ns/pid exist");
    errno = 0;
    CHECK(clone_flags(CLONE_NEWUSER | CLONE_FS) == -1 && errno == EINVAL, "clone(CLONE_NEWUSER | CLONE_FS): EINVAL");
    pthread_t th;
    pthread_create(&th, NULL, idle_thread, NULL);
    errno = 0;
    CHECK(unshare(CLONE_NEWUSER) == -1 && errno == EINVAL, "unshare(CLONE_NEWUSER) with two threads: EINVAL");
    return failures;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "content")) { setvbuf(stdout, NULL, _IOLBF, 0); return content_process(atoi(argv[2])); }
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof self_path - 1);
    if (n <= 0) strcpy(self_path, argv[0]); else self_path[n] = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("nstest: namespaces\n");
    if (getuid() != 0) { printf("nstest: run as root\n"); return 1; }
    snprintf(tmpdir, sizeof tmpdir, "/tmp/nstest.%d", (int)getpid());
    if (mkdir(tmpdir, 0777) && errno != EEXIST) { printf("nstest: mkdir %s: %s\n", tmpdir, strerror(errno)); return 1; }
    chmod(tmpdir, 0777);

    CHECK(in_child(t_refusals) == 0, "/proc/PID/ns and the refusals");
    CHECK(in_child(t_root_maps) == 0, "root in a user namespace: maps, ids seen through them");
    CHECK(in_child(t_user_maps) == 0, "uid 1000 in a user namespace: Linux's map rules, capabilities");
    CHECK(in_child(t_nested) == 0, "a user namespace inside one");

    snprintf(abs_name, sizeof abs_name, "nstest-%d", (int)getpid());
    snprintf(unix_path, sizeof unix_path, "%s/sock", tmpdir);
    int al = abstract_sock(abs_name, 1, 0);
    int ul = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ua = { .sun_family = AF_UNIX };
    strcpy(ua.sun_path, unix_path);
    if (bind(ul, (struct sockaddr *)&ua, sizeof ua) || listen(ul, 4)) printf("  (bind %s: %s)\n", unix_path, strerror(errno));
    CHECK(in_child(t_netns) == 0, "a network namespace: no network, abstract names of its own");
    int c = abstract_sock(abs_name, 0, 1);
    CHECK(al >= 0 && c >= 0, "outside, the abstract name still answers");
    if (c >= 0) close(c);
    close(al); close(ul); unlink(unix_path);

    shm_key = (key_t)(0x4e530000 | (getpid() & 0xffff));
    shm_id = shmget(shm_key, 4096, IPC_CREAT | 0600);
    CHECK(in_child(t_ipcns) == 0, "an IPC namespace: System V shared memory of its own");
    CHECK(shmget(shm_key, 0, 0) == shm_id, "outside, the segment is the same one");
    shmctl(shm_id, IPC_RMID, NULL);

    struct utsname u0, u1;
    uname(&u0);
    CHECK(in_child(t_utsns) == 0, "a UTS namespace: a host name of its own");
    CHECK(uname(&u1) == 0 && !strcmp(u0.nodename, u1.nodename), "outside, the host name is unchanged");

    CHECK(in_child(t_firefox) == 0, "Firefox's launch: namespaces, maps, the chroot helper, execve");

    rmdir(tmpdir);
    printf("nstest: %s (%d failed)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
