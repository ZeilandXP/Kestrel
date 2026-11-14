/* user/install/util.c -- the installer's log, random numbers, programs */
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <sys/stat.h>

static FILE *logfp;

void log_open(const char *path)
{
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    char *s = strrchr(dir, '/');
    if (s) { *s = 0; mkdir(dir, 0755); }
    logfp = fopen(path, "a");
    if (logfp) setvbuf(logfp, NULL, _IOLBF, 0);
}

void ilog(const char *fmt, ...)
{
    if (!logfp) return;
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    fprintf(logfp, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logfp, fmt, ap);
    va_end(ap);
    fputc('\n', logfp);
}

void random_bytes(void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = getrandom((char *)buf + done, n - done, 0);
        if (r <= 0) break;
        done += (size_t)r;
    }
    if (done < n) {                             /* no getrandom: the clock, at least */
        uint64_t x = (uint64_t)time(NULL) ^ (uint64_t)getpid() << 32;
        for (; done < n; done++) { x = x * 6364136223846793005ull + 1442695040888963407ull; ((uint8_t *)buf)[done] = (uint8_t)(x >> 56); }
    }
}

int run(const char *const argv[], char *out, size_t cap)
{
    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, 0); close(nul); }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    char tmp[4096];
    for (;;) {
        ssize_t r = read(p[0], tmp, sizeof tmp);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        if (out && n + 1 < cap) {
            size_t k = (size_t)r < cap - 1 - n ? (size_t)r : cap - 1 - n;
            memcpy(out + n, tmp, k);
            n += k;
        }
    }
    if (out && cap) out[n] = 0;
    close(p[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    char line[256] = "";
    for (int i = 0; argv[i] && strlen(line) < sizeof line - 40; i++) {
        strncat(line, argv[i], sizeof line - strlen(line) - 2);
        strcat(line, " ");
    }
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    ilog("ran %s-> %d%s%s", line, code, out && *out ? ": " : "", out ? out : "");
    return code;
}
