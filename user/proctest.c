/* user/proctest.c -- processes and signals on Kestrel (musl)
 *
 * fork, execve, posix_spawn (clone CLONE_VM|CLONE_VFORK), vfork, wait4,
 * sessions, and signals: handlers, masks, sigsuspend, kill, SIGCHLD,
 * SIGPIPE, EINTR/SA_RESTART, default actions, FPU state across handlers,
 * and job control: stopping and continuing, WUNTRACED/WCONTINUED, stopped
 * threads, orphaned groups, SIGTTIN/SIGTTOU from a terminal.
 * Exit status 0 = every check passed. `proctest exec-child N` exits N (the
 * image that execve and posix_spawn start). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <pthread.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

extern char **environ;
static int failures;
#define CHECK(c, what) do { int ok_ = (c); printf("  %s  %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static volatile sig_atomic_t got_usr1, got_usr2, got_chld;
static void on_usr1(int s) { (void)s; got_usr1++; }
static void on_usr2(int s) { (void)s; got_usr2++; }
static void on_chld(int s) { (void)s; got_chld++; }
static volatile sig_atomic_t got_alrm;
static void on_alrm(int s) { (void)s; got_alrm++; }
static volatile double fp_in_handler;
static void on_fp(int s) { volatile double x = s; for (int i = 0; i < 10; i++) x = x * 1.5 + 0.25; fp_in_handler = x; }

static void handler(int sig, void (*fn)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

static int wait_status(pid_t pid)
{
    int st = -1;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    return st;
}


/* ---- job control ---- */
static char proc_state(pid_t pid)
{
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return '?';
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return '?';
    buf[n] = 0;
    char *p = strrchr(buf, ')');
    return p && p[1] == ' ' ? p[2] : '?';
}

static void msleep(int ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; while (nanosleep(&ts, &ts) && errno == EINTR) ; }

static volatile long spin_count;
static void *spinner(void *a) { (void)a; for (;;) __atomic_add_fetch(&spin_count, 1, __ATOMIC_RELAXED); return NULL; }

static volatile sig_atomic_t chld_code, chld_status, got_cont, got_hup;
static void on_chld_info(int s, siginfo_t *si, void *u) { (void)s; (void)u; chld_code = si->si_code; chld_status = si->si_status; }
static void on_cont(int s) { (void)s; got_cont++; }
static void on_hup(int s) { (void)s; got_hup++; }

static void job_control_tests(void)
{
    int st;
    /* SIGSTOP, WUNTRACED, the T state, SIGCONT, WCONTINUED */
    int pfd[2];
    pipe(pfd);
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        for (int i = 0; ; i++) { if (write(pfd[1], "x", 1) != 1) _exit(1); msleep(5); if (i > 4000) _exit(2); }
    }
    close(pfd[1]);
    char c;
    read(pfd[0], &c, 1);
    kill(pid, SIGSTOP);
    pid_t w = waitpid(pid, &st, WUNTRACED);
    CHECK(w == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP, "SIGSTOP: waitpid(WUNTRACED) reports it stopped");
    CHECK(proc_state(pid) == 'T', "a stopped process shows T in /proc/PID/stat");
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    while (read(pfd[0], &c, 1) == 1) ;                   /* what it wrote before it stopped */
    msleep(100);
    CHECK(read(pfd[0], &c, 1) == -1 && errno == EAGAIN, "a stopped process does not run");
    CHECK(waitpid(pid, &st, WUNTRACED | WNOHANG) == 0, "a stop is reported once");
    kill(pid, SIGCONT);
    w = waitpid(pid, &st, WCONTINUED);
    CHECK(w == pid && WIFCONTINUED(st), "SIGCONT: waitpid(WCONTINUED) reports it continued");
    fcntl(pfd[0], F_SETFL, 0);
    CHECK(read(pfd[0], &c, 1) == 1, "a continued process runs again");
    kill(pid, SIGKILL);
    st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "its exit is still collected");
    close(pfd[0]);

    /* SIGTSTP's default stops; SIGKILL ends a stopped process; SIGCHLD says CLD_STOPPED */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_chld_info;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    chld_code = 0;
    pid = fork();
    if (pid == 0) { setpgid(0, 0); raise(SIGTSTP); _exit(5); }
    w = waitpid(pid, &st, WUNTRACED);
    CHECK(w == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP, "raise(SIGTSTP) with the default action stops the process");
    for (int i = 0; i < 50 && chld_code != CLD_STOPPED; i++) msleep(10);
    CHECK(chld_code == CLD_STOPPED && chld_status == SIGTSTP, "SIGCHLD with CLD_STOPPED and the stop signal");
    kill(pid, SIGCONT);
    st = wait_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 5, "continued, it carries on to exit(5)");
    sa.sa_flags |= SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    chld_code = 0;
    pid = fork();
    if (pid == 0) { raise(SIGSTOP); _exit(0); }
    w = waitpid(pid, &st, WUNTRACED);
    msleep(50);
    CHECK(w == pid && WIFSTOPPED(st) && chld_code == 0, "SA_NOCLDSTOP: no SIGCHLD for a stop, wait still sees it");
    kill(pid, SIGKILL);
    st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "SIGKILL ends a stopped process");
    signal(SIGCHLD, SIG_DFL);

    /* a handled SIGCONT; SIGTSTP with a handler does not stop */
    pipe(pfd);
    pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        got_usr1 = 0; got_cont = 0;
        handler(SIGCONT, on_cont, 0);
        handler(SIGTSTP, on_usr1, 0);
        raise(SIGTSTP);                                 /* caught: no stop */
        raise(SIGSTOP);
        char r = got_cont == 1 && got_usr1 == 1 ? 'k' : 'n';
        write(pfd[1], &r, 1);
        _exit(0);
    }
    close(pfd[1]);
    w = waitpid(pid, &st, WUNTRACED);
    kill(pid, SIGCONT);
    c = 0;
    read(pfd[0], &c, 1);
    close(pfd[0]);
    wait_status(pid);
    CHECK(w == pid && WIFSTOPPED(st) && c == 'k', "a caught SIGTSTP does not stop; SIGCONT's handler runs on continuing");

    /* every thread stops */
    pipe(pfd);
    pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        pthread_t th;
        pthread_create(&th, NULL, spinner, NULL);
        pthread_create(&th, NULL, spinner, NULL);
        for (;;) {
            long v = spin_count;
            if (write(pfd[1], &v, sizeof v) != sizeof v) _exit(1);
            msleep(20);
        }
    }
    close(pfd[1]);
    long v1 = 0, v2 = 0, v3 = 0;
    read(pfd[0], &v1, sizeof v1);
    msleep(100);
    kill(pid, SIGSTOP);
    waitpid(pid, &st, WUNTRACED);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);
    while (read(pfd[0], &v2, sizeof v2) == sizeof v2) ;
    msleep(200);
    long after = -1;
    int quiet = read(pfd[0], &after, sizeof after) == -1;
    kill(pid, SIGCONT);
    fcntl(pfd[0], F_SETFL, 0);
    read(pfd[0], &v3, sizeof v3);
    msleep(100);
    long v4 = 0;
    read(pfd[0], &v4, sizeof v4);
    kill(pid, SIGKILL);
    wait_status(pid);
    close(pfd[0]);
    CHECK(WIFSTOPPED(st) && quiet && v4 > v3, "a process with three threads stops and continues as a whole");

    /* an orphaned group with a stopped member gets SIGHUP and SIGCONT */
    pipe(pfd);
    pid = fork();
    if (pid == 0) {
        pid_t g = fork();
        if (g == 0) {
            close(pfd[0]);
            setpgid(0, 0);
            handler(SIGHUP, on_hup, 0);
            raise(SIGSTOP);
            char r = got_hup ? 'h' : 'n';
            write(pfd[1], &r, 1);
            _exit(0);
        }
        int s2;
        waitpid(g, &s2, WUNTRACED);
        _exit(WIFSTOPPED(s2) ? 0 : 1);                  /* leaves its stopped child orphaned */
    }
    close(pfd[1]);
    st = wait_status(pid);
    c = 0;
    read(pfd[0], &c, 1);
    close(pfd[0]);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0 && c == 'h', "an orphaned stopped group is sent SIGHUP and SIGCONT");

    /* a terminal: background reads stop with SIGTTIN, TOSTOP writes with SIGTTOU */
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) { CHECK(0, "a pty for the terminal checks"); return; }
    char *sname = ptsname(m);
    pid = fork();
    if (pid == 0) {
        setsid();
        int s = open(sname, O_RDWR);                    /* the session's controlling terminal */
        if (s < 0) _exit(10);
        pid_t bg = fork();
        if (bg == 0) {
            setpgid(0, 0);
            char b[8] = { 0 };
            ssize_t n = read(s, b, sizeof b);            /* SIGTTIN until it is the foreground */
            _exit(n == 2 && b[0] == 'q' ? 0 : 11);
        }
        setpgid(bg, bg);
        int s2;
        if (waitpid(bg, &s2, WUNTRACED) != bg || !WIFSTOPPED(s2) || WSTOPSIG(s2) != SIGTTIN) _exit(12);
        signal(SIGTTOU, SIG_IGN);
        if (tcsetpgrp(s, bg)) _exit(13);                /* hand it the terminal */
        kill(bg, SIGCONT);
        write(m, "q\n", 2);
        if (waitpid(bg, &s2, 0) != bg || !WIFEXITED(s2) || WEXITSTATUS(s2)) _exit(14);
        if (tcsetpgrp(s, getpgrp())) _exit(15);
        signal(SIGTTOU, SIG_DFL);
        struct termios tio;
        tcgetattr(s, &tio);
        tio.c_lflag |= TOSTOP;
        tcsetattr(s, TCSANOW, &tio);
        bg = fork();
        if (bg == 0) { setpgid(0, 0); _exit(write(s, "w", 1) == 1 ? 0 : 16); }
        setpgid(bg, bg);
        if (waitpid(bg, &s2, WUNTRACED) != bg || !WIFSTOPPED(s2) || WSTOPSIG(s2) != SIGTTOU) _exit(17);
        kill(bg, SIGKILL);
        waitpid(bg, &s2, 0);
        _exit(0);
    }
    st = wait_status(pid);
    close(m);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a background read gets SIGTTIN, and a TOSTOP write SIGTTOU");
    if (!WIFEXITED(st) || WEXITSTATUS(st)) printf("  (terminal check stopped at step %d)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 3 && !strcmp(argv[1], "exec-child")) {
        printf("proctest: exec-child running as pid %d, argv[2]=%s, env KESTREL_TEST=%s\n", getpid(), argv[2],
               getenv("KESTREL_TEST") ? getenv("KESTREL_TEST") : "(unset)");
        return atoi(argv[2]);
    }
    printf("proctest: pid %d\n", getpid());

    /* ---- fork + wait4 ---- */
    static int shared = 1;
    pid_t pid = fork();
    if (pid == 0) { shared = 99; _exit(7); }
    int st = wait_status(pid);
    CHECK(pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 7, "fork: child exit status 7 through waitpid");
    CHECK(shared == 1, "fork: the child's writes do not reach the parent");
    CHECK(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD, "waitpid with no children: ECHILD");

    int pfd[2];
    pipe(pfd);
    pid = fork();
    if (pid == 0) { close(pfd[0]); write(pfd[1], "from child", 10); _exit(0); }
    close(pfd[1]);
    char buf[32] = { 0 };
    ssize_t n = read(pfd[0], buf, sizeof buf - 1);
    close(pfd[0]);
    wait_status(pid);
    CHECK(n == 10 && !strcmp(buf, "from child"), "fork: inherited pipe carries data");

    pid = fork();
    if (pid == 0) { struct timespec ts = { 0, 100 * 1000000L }; nanosleep(&ts, NULL); _exit(0); }
    CHECK(waitpid(pid, &st, WNOHANG) == 0, "WNOHANG returns 0 while the child runs");
    wait_status(pid);

    pid_t kids[16];
    for (int i = 0; i < 16; i++) if ((kids[i] = fork()) == 0) _exit(i);
    int okk = 1;
    for (int i = 0; i < 16; i++) { st = wait_status(kids[i]); if (!WIFEXITED(st) || WEXITSTATUS(st) != i) okk = 0; }
    CHECK(okk, "16 children at once, each status collected");

    /* ---- execve ---- */
    pid = fork();
    if (pid == 0) {
        char *av[] = { "proctest", "exec-child", "42", NULL };
        char *ev[] = { "KESTREL_TEST=execve", NULL };
        execve("/boot/bin/proctest", av, ev);
        _exit(100 + errno);
    }
    st = wait_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "execve in a forked child (exit 42 from the new image)");

    char *av0[] = { "nope", NULL };
    CHECK(execve("/boot/bin/does-not-exist", av0, environ) == -1 && errno == ENOENT,
          "execve of a missing file: ENOENT, caller unharmed");

    /* ---- posix_spawn: clone(CLONE_VM | CLONE_VFORK) + execve ---- */
    setenv("KESTREL_TEST", "posix_spawn", 1);
    char *sv[] = { "proctest", "exec-child", "33", NULL };
    int rc = posix_spawn(&pid, "/boot/bin/proctest", NULL, NULL, sv, environ);
    st = rc == 0 ? wait_status(pid) : -1;
    CHECK(rc == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 33, "posix_spawn + waitpid (exit 33)");
    rc = posix_spawn(&pid, "/boot/bin/does-not-exist", NULL, NULL, sv, environ);
    if (rc == 0) wait_status(pid);
    CHECK(rc == ENOENT, "posix_spawn of a missing file reports ENOENT");

    /* ---- vfork ---- */
    volatile int vf = 0;
    pid = vfork();
    if (pid == 0) { vf = 5; _exit(3); }
    st = wait_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3 && vf == 5, "vfork: child shares memory until _exit");

    /* ---- sessions ---- */
    pid = fork();
    if (pid == 0) {
        pid_t s = setsid();
        _exit(s == getpid() && getsid(0) == getpid() && getpgid(0) == getpid() ? 0 : 1);
    }
    st = wait_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "setsid in a child: new session and group");

    /* ---- signals ---- */
    handler(SIGUSR1, on_usr1, 0);
    raise(SIGUSR1);
    CHECK(got_usr1 == 1, "raise(SIGUSR1) runs the handler");
    kill(getpid(), SIGUSR1);
    CHECK(got_usr1 == 2, "kill(getpid(), SIGUSR1) runs the handler");

    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    raise(SIGUSR1);
    sigset_t pend;
    sigpending(&pend);
    CHECK(got_usr1 == 2 && sigismember(&pend, SIGUSR1), "a blocked signal stays pending");
    sigprocmask(SIG_SETMASK, &old, NULL);
    CHECK(got_usr1 == 3, "unblocking delivers it");

    signal(SIGUSR1, SIG_IGN);
    raise(SIGUSR1);
    CHECK(got_usr1 == 3, "SIG_IGN discards it");

    /* floats in flight across a signal, and floats inside the handler */
    double acc = 1.0;
    for (int i = 0; i < 10; i++) acc = acc * 1.25 + 0.5;
    handler(SIGUSR2, on_fp, 0);
    raise(SIGUSR2);
    for (int i = 0; i < 10; i++) acc = acc * 1.25 + 0.5;
    double expect = 1.0, inh = SIGUSR2;
    for (int i = 0; i < 20; i++) expect = expect * 1.25 + 0.5;
    for (int i = 0; i < 10; i++) inh = inh * 1.5 + 0.25;
    CHECK(acc == expect && fp_in_handler == inh, "FPU state survives a handler that uses floats");

    handler(SIGCHLD, on_chld, SA_RESTART);
    pid = fork();
    if (pid == 0) _exit(0);
    wait_status(pid);
    CHECK(got_chld >= 1, "SIGCHLD reaches the parent's handler");
    signal(SIGCHLD, SIG_DFL);

    pid = fork();
    if (pid == 0) { for (;;) pause(); }
    struct timespec ts = { 0, 50 * 1000000L };
    nanosleep(&ts, NULL);
    kill(pid, SIGTERM);
    st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "kill(child, SIGTERM): child ends, WTERMSIG = SIGTERM");

    pid = fork();
    if (pid == 0) { for (;;) ; }                       /* busy in ring 3 */
    nanosleep(&ts, NULL);
    kill(pid, SIGKILL);
    st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "SIGKILL ends a child spinning in user mode");

    /* EINTR vs SA_RESTART on a blocking read */
    pipe(pfd);
    pid_t me = getpid();
    handler(SIGUSR2, on_usr2, 0);
    pid = fork();
    if (pid == 0) { nanosleep(&ts, NULL); kill(me, SIGUSR2); nanosleep(&ts, NULL); write(pfd[1], "x", 1); _exit(0); }
    n = read(pfd[0], buf, 1);
    CHECK(n == -1 && errno == EINTR && got_usr2 == 1, "read interrupted by a handler without SA_RESTART: EINTR");
    read(pfd[0], buf, 1);
    wait_status(pid);
    handler(SIGUSR2, on_usr2, SA_RESTART);
    pid = fork();
    if (pid == 0) { nanosleep(&ts, NULL); kill(me, SIGUSR2); nanosleep(&ts, NULL); write(pfd[1], "y", 1); _exit(0); }
    n = read(pfd[0], buf, 1);
    CHECK(n == 1 && buf[0] == 'y' && got_usr2 == 2, "with SA_RESTART the read is restarted and gets the data");
    wait_status(pid);
    close(pfd[0]); close(pfd[1]);

    /* sigsuspend */
    sigprocmask(SIG_BLOCK, &set, &old);                 /* SIGUSR1 blocked */
    handler(SIGUSR1, on_usr1, 0);
    pid = fork();
    if (pid == 0) { nanosleep(&ts, NULL); kill(me, SIGUSR1); _exit(0); }
    sigset_t none;
    sigemptyset(&none);
    int r = sigsuspend(&none);
    sigset_t now;
    sigprocmask(SIG_SETMASK, NULL, &now);
    CHECK(r == -1 && errno == EINTR && got_usr1 == 4 && sigismember(&now, SIGUSR1),
          "sigsuspend waits for the signal and restores the mask");
    sigprocmask(SIG_SETMASK, &old, NULL);
    wait_status(pid);

    /* nanosleep interrupted */
    pid = fork();
    if (pid == 0) { nanosleep(&ts, NULL); kill(me, SIGUSR1); _exit(0); }
    struct timespec longer = { 5, 0 }, rem = { 0, 0 };
    r = nanosleep(&longer, &rem);
    CHECK(r == -1 && errno == EINTR && rem.tv_sec >= 3, "nanosleep interrupted: EINTR and the time left");
    wait_status(pid);

    /* SIGPIPE */
    pipe(pfd);
    close(pfd[0]);
    signal(SIGPIPE, SIG_IGN);
    CHECK(write(pfd[1], "z", 1) == -1 && errno == EPIPE, "write to a pipe without readers, SIGPIPE ignored: EPIPE");
    signal(SIGPIPE, SIG_DFL);
    pid = fork();
    if (pid == 0) { write(pfd[1], "z", 1); _exit(0); }
    st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE, "...with the default action the writer dies of SIGPIPE");
    close(pfd[1]);

    /* timers: alarm() and a periodic ITIMER_REAL */
    static volatile sig_atomic_t alarms;
    struct sigaction al;
    memset(&al, 0, sizeof al);
    al.sa_handler = on_alrm;
    sigaction(SIGALRM, &al, NULL);
    got_alrm = 0;
    alarm(1);
    pause();
    CHECK(got_alrm == 1, "alarm(1) + pause(): SIGALRM arrives");
    struct itimerval it = { { 0, 50000 }, { 0, 50000 } }, off = { { 0, 0 }, { 0, 0 } };
    got_alrm = 0;
    setitimer(ITIMER_REAL, &it, NULL);
    while (got_alrm < 3) pause();
    setitimer(ITIMER_REAL, &off, NULL);
    (void)alarms;
    CHECK(got_alrm == 3, "setitimer: a periodic 50 ms timer fires repeatedly");

    char exe[128] = { 0 };
    ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    CHECK(el > 0 && strstr(exe, "proctest"), "readlink(/proc/self/exe) names the program");

    pid = fork();
    if (pid == 0) abort();
    st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT, "abort(): WTERMSIG = SIGABRT");

    job_control_tests();

    printf(failures ? "proctest: FAILED (%d)\n" : "proctest: all checks passed\n", failures);
    return failures ? 1 : 0;
}
