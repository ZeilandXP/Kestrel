/* user/install/setup.c -- the installed system's settings
 *
 * Written into the new root before its first boot: the computer's name,
 * the account (in /etc/passwd, /etc/group and /etc/shadow, with a SHA-512
 * crypt hash; root gets the same password), the time zone (/etc/localtime
 * as a TZif file holding only the zone's POSIX rule, so musl and glibc
 * programs both follow it), the keyboard layout (/etc/default/keyboard,
 * read by /etc/init.d/rcS for the text terminals and by startx for the
 * desktop), and whether the terminals log in by themselves
 * (/etc/default/kestrel). */
#define _GNU_SOURCE
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

const struct tz tz_list[] = {
    { "UTC", "UTC0" },
    { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Dublin", "IST-1GMT0,M10.5.0,M3.5.0/1" },
    { "Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Copenhagen", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Prague", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Budapest", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Istanbul", "<+03>-3" },
    { "Europe/Moscow", "MSK-3" },
    { "Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    { "Africa/Lagos", "WAT-1" },
    { "Africa/Johannesburg", "SAST-2" },
    { "Africa/Nairobi", "EAT-3" },
    { "Asia/Dubai", "<+04>-4" },
    { "Asia/Karachi", "PKT-5" },
    { "Asia/Kolkata", "IST-5:30" },
    { "Asia/Dhaka", "<+06>-6" },
    { "Asia/Bangkok", "<+07>-7" },
    { "Asia/Jakarta", "WIB-7" },
    { "Asia/Shanghai", "CST-8" },
    { "Asia/Hong_Kong", "HKT-8" },
    { "Asia/Singapore", "<+08>-8" },
    { "Asia/Manila", "PST-8" },
    { "Asia/Seoul", "KST-9" },
    { "Asia/Tokyo", "JST-9" },
    { "Australia/Perth", "AWST-8" },
    { "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane", "AEST-10" },
    { "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Pacific/Honolulu", "HST10" },
    { "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Phoenix", "MST7" },
    { "America/Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Mexico_City", "CST6" },
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Toronto", "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Halifax", "AST4ADT,M3.2.0,M11.1.0" },
    { "America/Bogota", "<-05>5" },
    { "America/Lima", "<-05>5" },
    { "America/Caracas", "<-04>4" },
    { "America/Santiago", "<-04>4<-03>,M9.1.6/24,M4.1.6/24" },
    { "America/Sao_Paulo", "<-03>3" },
    { "America/Argentina/Buenos_Aires", "<-03>3" },
};
const int tz_count = sizeof tz_list / sizeof *tz_list;

const struct layout layouts[] = {
    { "us", "us", "English (US)" },
    { "gb", "gb", "English (UK)" },
    { "de", "de", "German" },
    { "fr", "fr", "French (AZERTY)" },
    { "es", "es", "Spanish" },
    { "it", "it", "Italian" },
};
const int layout_count = sizeof layouts / sizeof *layouts;

char *read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    char *b = malloc(cap + 1);
    for (;;) {
        if (!b) { close(fd); return NULL; }
        if (n == cap) { cap *= 2; char *nb = realloc(b, cap + 1); if (!nb) { free(b); close(fd); return NULL; } b = nb; }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    b[n] = 0;
    if (len) *len = n;
    return b;
}

int write_file(const char *path, const char *text, mode_t mode)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.kestrel-new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return -errno;
    size_t n = strlen(text);
    for (size_t o = 0; o < n;) {
        ssize_t w = write(fd, text + o, n - o);
        if (w <= 0) { close(fd); unlink(tmp); return -EIO; }
        o += (size_t)w;
    }
    fchmod(fd, mode);
    if (close(fd) < 0 || rename(tmp, path) < 0) { unlink(tmp); return -errno; }
    return 0;
}

static void path_in(char *out, size_t cap, const char *root, const char *p) { snprintf(out, cap, "%s%s", root, p); }

/* Lines of an /etc file, without those for which drop() says so. */
static char *filter_lines(const char *text, bool (*drop)(const char *line, const void *ctx), const void *ctx)
{
    size_t cap = (text ? strlen(text) : 0) + 1;
    char *out = calloc(1, cap + 1), *o = out;
    if (!out) return NULL;
    for (const char *s = text; s && *s;) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)len, s);
        if (len && !drop(line, ctx)) { memcpy(o, s, len); o += len; *o++ = '\n'; }
        s = nl ? nl + 1 : s + len;
    }
    *o = 0;
    return out;
}

static int field_num(const char *line, int idx)          /* the idx-th ':'-separated field, as a number */
{
    for (int i = 0; i < idx && line; i++) { line = strchr(line, ':'); if (line) line++; }
    return line ? atoi(line) : -1;
}

static bool starts_name(const char *line, const char *name)
{
    size_t n = strlen(name);
    return !strncmp(line, name, n) && line[n] == ':';
}

struct who { const char *user; const char *old; };
static bool drop_passwd(const char *line, const void *ctx)
{
    const struct who *w = ctx;
    return starts_name(line, w->user) || field_num(line, 2) == 1000 || (w->old && starts_name(line, w->old));
}
static bool drop_shadow(const char *line, const void *ctx)
{
    const struct who *w = ctx;
    return starts_name(line, w->user) || starts_name(line, "root") || (w->old && starts_name(line, w->old));
}

/* The name of the account with uid 1000 the root came with ("user"), if any. */
static char *uid1000_name(const char *passwd)
{
    for (const char *s = passwd; s && *s;) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        char line[512];
        snprintf(line, sizeof line, "%.*s", (int)len, s);
        if (field_num(line, 2) == 1000) { char *c = strchr(line, ':'); if (c) *c = 0; return strdup(line); }
        s = nl ? nl + 1 : s + len;
    }
    return NULL;
}

/* Add `user` to the member list of each of these groups that exists. */
static char *add_to_groups(const char *text, const char *user, const char *const *groups)
{
    size_t cap = strlen(text) + 2048;
    char *out = malloc(cap), *o = out;
    if (!out) return NULL;
    for (const char *s = text; *s;) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        memcpy(o, s, len);
        o += len;
        for (int i = 0; groups[i]; i++)
            if (len > strlen(groups[i]) && starts_name(s, groups[i])) {
                bool empty = s[len - 1] == ':';
                o += sprintf(o, "%s%s", empty ? "" : ",", user);
            }
        *o++ = '\n';
        s = nl ? nl + 1 : s + len;
    }
    *o = 0;
    return out;
}

static void salt(char out[20])
{
    static const char abc[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    uint8_t r[16];
    random_bytes(r, sizeof r);
    memcpy(out, "$6$", 3);
    for (int i = 0; i < 16; i++) out[3 + i] = abc[r[i] % 64];
    out[19] = 0;
}

/* /etc/localtime: TZif version 2 with no transitions and the zone's rule in
 * the footer, which musl and glibc both apply to every time. */
static int write_tzif(const char *path, const char *posix)
{
    uint8_t b[512];
    size_t n = 0;
    int std_off = 0;
    {                                           /* the standard offset, for the one time type */
        const char *p = posix;
        if (*p == '<') { while (*p && *p != '>') p++; if (*p) p++; }
        else while (*p && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) p++;
        int sign = *p == '-' ? -1 : 1;
        if (*p == '-' || *p == '+') p++;
        int h = atoi(p), m = 0;
        const char *c = strchr(p, ':');
        if (c && c - p <= 2) m = atoi(c + 1);
        std_off = -sign * (h * 3600 + m * 60);
    }
    for (int v = 0; v < 2; v++) {
        memcpy(b + n, "TZif2", 5);
        memset(b + n + 5, 0, 15);
        /* one transition, at the earliest time: from then on the footer's
         * rule applies (glibc ignores the footer in a file with none) */
        uint32_t counts[6] = { 0, 0, 0, 1, 1, 4 };          /* isut isstd leap time type char */
        for (int i = 0; i < 6; i++) {
            uint32_t be = __builtin_bswap32(counts[i]);
            memcpy(b + n + 20 + 4 * i, &be, 4);
        }
        n += 44;
        if (v == 0) { memcpy(b + n, "\x80\0\0\0", 4); n += 4; }
        else { memcpy(b + n, "\xff\xff\xff\xff\x80\0\0\0", 8); n += 8; }
        b[n++] = 0;                                         /* its time type */
        uint32_t off = __builtin_bswap32((uint32_t)std_off);
        memcpy(b + n, &off, 4);
        b[n + 4] = 0;
        b[n + 5] = 0;
        n += 6;
        memcpy(b + n, "LMT", 4);
        n += 4;
    }
    n += (size_t)snprintf((char *)b + n, sizeof b - n, "\n%s\n", posix);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -errno;
    ssize_t w = write(fd, b, n);
    close(fd);
    return w == (ssize_t)n ? 0 : -EIO;
}

static void append_block(const char *path, const char *marker, const char *block)
{
    char *old = read_file(path, NULL);
    if (old && strstr(old, marker)) { free(old); return; }
    size_t n = (old ? strlen(old) : 0) + strlen(block) + 4;
    char *t = malloc(n);
    if (!t) { free(old); return; }
    snprintf(t, n, "%s%s%s", old ? old : "", old && *old && old[strlen(old) - 1] != '\n' ? "\n" : "", block);
    write_file(path, t, 0644);
    free(t);
    free(old);
}

int setup_system(const char *root, const struct settings *s, char *err, size_t errcap)
{
    char p[512], q[512];
    path_in(p, sizeof p, root, "/etc");
    mkdir(p, 0755);

    /* the computer's name */
    char t[1024];
    snprintf(t, sizeof t, "%s\n", s->hostname);
    path_in(p, sizeof p, root, "/etc/hostname");
    if (write_file(p, t, 0644) < 0) { snprintf(err, errcap, "writing %s: %s", p, strerror(errno)); return -1; }
    path_in(p, sizeof p, root, "/etc/hosts");
    char *hosts = read_file(p, NULL);
    snprintf(t, sizeof t, "127.0.1.1\t%s\n", s->hostname);
    if (!hosts) write_file(p, "127.0.0.1\tlocalhost\n::1\t\tlocalhost\n", 0644);
    free(hosts);
    append_block(p, t, t);

    /* the account: uid 1000 in place of the one the root came with */
    path_in(p, sizeof p, root, "/etc/passwd");
    char *pw = read_file(p, NULL);
    char *old = uid1000_name(pw);
    struct who w = { s->user, old };
    char *npw = filter_lines(pw ? pw : "", drop_passwd, &w);
    if (!npw) { snprintf(err, errcap, "out of memory"); return -1; }
    char *grown = malloc(strlen(npw) + 512);
    sprintf(grown, "%s%s%s:x:1000:1000:%s,,,:/home/%s:/bin/sh\n",
            strstr(npw, "root:") == npw || strstr(npw, "\nroot:") ? "" : "root:x:0:0:root:/root:/bin/sh\n",
            npw, s->user, s->fullname, s->user);
    if (write_file(p, grown, 0644) < 0) { snprintf(err, errcap, "writing %s", p); return -1; }
    free(grown); free(npw); free(pw);

    path_in(p, sizeof p, root, "/etc/group");
    char *gr = read_file(p, NULL);
    struct who wg = { s->user, old };
    char *ngr = filter_lines(gr ? gr : "root:x:0:\n", drop_passwd, &wg);
    static const char *const extra[] = { "wheel", "sudo", "adm", "audio", "video", "input", "users", "plugdev", "disk", NULL };
    char *ngr2 = ngr ? add_to_groups(ngr, s->user, extra) : NULL;
    if (!ngr2) { snprintf(err, errcap, "out of memory"); return -1; }
    char *g3 = malloc(strlen(ngr2) + 128);
    sprintf(g3, "%s%s:x:1000:\n", ngr2, s->user);
    if (write_file(p, g3, 0644) < 0) { snprintf(err, errcap, "writing %s", p); return -1; }
    free(g3); free(ngr2); free(ngr); free(gr);

    char sl[20];
    salt(sl);
    const char *h = crypt(s->password, sl);
    if (!h || h[0] != '$') { snprintf(err, errcap, "crypt() has no SHA-512 hashes"); return -1; }
    char *hash = strdup(h);
    path_in(p, sizeof p, root, "/etc/shadow");
    char *sh = read_file(p, NULL);
    char *nsh = filter_lines(sh ? sh : "", drop_shadow, &w);
    long days = (long)(time(NULL) / 86400);
    char *s3 = malloc((nsh ? strlen(nsh) : 0) + 512);
    sprintf(s3, "root:%s:%ld:0:99999:7:::\n%s%s:%s:%ld:0:99999:7:::\n", hash, days, nsh ? nsh : "", s->user, hash, days);
    if (write_file(p, s3, 0640) < 0) { snprintf(err, errcap, "writing %s", p); return -1; }
    free(s3); free(nsh); free(sh); free(hash);

    /* the home directory: the old account's, renamed */
    snprintf(q, sizeof q, "%s/home/%s", root, s->user);
    path_in(p, sizeof p, root, "/home");
    mkdir(p, 0755);
    if (old && strcmp(old, s->user)) {
        snprintf(p, sizeof p, "%s/home/%s", root, old);
        if (rename(p, q) < 0 && errno != ENOENT) ilog("rename %s: %s", p, strerror(errno));
    }
    mkdir(q, 0755);
    if (chown(q, 1000, 1000) < 0) ilog("chown %s: %s", q, strerror(errno));
    free(old);

    /* the time zone */
    const struct tz *z = &tz_list[s->tz];
    snprintf(t, sizeof t, "%s\n", z->name);
    path_in(p, sizeof p, root, "/etc/timezone");
    write_file(p, t, 0644);
    path_in(p, sizeof p, root, "/etc/localtime");
    unlink(p);
    snprintf(q, sizeof q, "%s/usr/share/zoneinfo/%s", root, z->name);
    char target[256];
    snprintf(target, sizeof target, "/usr/share/zoneinfo/%s", z->name);
    if (access(q, R_OK) == 0 ? symlink(target, p) < 0 : write_tzif(p, z->posix) < 0)
        ilog("/etc/localtime: %s", strerror(errno));

    /* the keyboard */
    const struct layout *l = &layouts[s->layout];
    path_in(p, sizeof p, root, "/etc/default");
    mkdir(p, 0755);
    snprintf(t, sizeof t, "# The keyboard layout (kestrel-install). /etc/init.d/rcS sets KESTREL_KEYMAP\n"
             "# for the text terminals, startx sets XKBLAYOUT on the desktop.\n"
             "XKBMODEL=\"pc105\"\nXKBLAYOUT=\"%s\"\nXKBVARIANT=\"\"\nXKBOPTIONS=\"\"\nKESTREL_KEYMAP=\"%s\"\n", l->xkb, l->kernel);
    path_in(p, sizeof p, root, "/etc/default/keyboard");
    write_file(p, t, 0644);

    /* logging in */
    snprintf(t, sizeof t, "# Kestrel's settings (kestrel-install)\n"
             "# AUTOLOGIN=yes: the desktop and the text terminals log AUTOLOGIN_USER in by\n"
             "# themselves; no: a login window on the desktop (/bin/startx) and a login\n"
             "# prompt on each terminal (/etc/init.d/rcS)\n"
             "AUTOLOGIN=%s\nAUTOLOGIN_USER=%s\n", s->autologin ? "yes" : "no", s->user);
    path_in(p, sizeof p, root, "/etc/default/kestrel");
    write_file(p, t, 0644);
    snprintf(t, sizeof t,
             "\n# kestrel-install: the time zone, and the desktop after a login in\n"
             "# startx's login window (KESTREL_SESSION, kept by login -p)\n"
             "[ -n \"$TZ\" ] || export TZ='%s'\n"
             "if [ \"$KESTREL_SESSION\" = 1 ] && [ -n \"$DISPLAY\" ]; then\n"
             "    unset KESTREL_SESSION\n"
             "    exec /bin/startx --session\n"
             "fi\n", z->posix);
    path_in(p, sizeof p, root, "/etc/profile");
    append_block(p, "# kestrel-install:", t);

    /* the installer's own menu entry: not on an installed system */
    path_in(p, sizeof p, root, "/usr/share/applications/kestrel-install.desktop");
    unlink(p);
    return 0;
}
