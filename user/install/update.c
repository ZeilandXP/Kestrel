/* user/install/update.c -- updating an installed Kestrel to this release
 *
 * Started from the new release's stick or ISO (kestrel-install offers it on
 * its Welcome screen; kestrel-install --update DISK without questions), it
 * finds installed systems by their EFI System Partition's boot/grub/local.cfg
 * and brings one to the running release, keeping the user's files,
 * accounts and settings:
 *
 *   1. the boot volume into the partition's other slot (a new UUID; the
 *      header last), and the kernel into that slot's kernel-<slot>.elf;
 *   2. the root's files: the release's files replace the installed ones
 *      (each unlinked first), except below /home, /root, /var/log and the
 *      like. In /etc a file the user changed stays: if this release changes
 *      it too, its new version goes beside it as <file>.kestrel-new. The
 *      accounts files (/etc/passwd, /etc/group, /etc/shadow) keep the
 *      user's lines and get the release's new ones. Files the old release
 *      had and this one has not go, unless the user changed them. What the
 *      installed release shipped is /var/lib/kestrel/files (crc, size, time);
 *   3. GRUB (when its images differ) and BOOTX64.EFI;
 *   4. local.cfg, one sector, switched to the new slot last: until then
 *      the old kernel and boot volume start. The old ones stay in the
 *      other slot, as GRUB's "previous version" entries, until the next
 *      update. */
#define _GNU_SOURCE
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/reboot.h>

/* ---- finding installed systems --------------------------------------------- */
static void cfg_value(const char *cfg, const char *key, char *out, size_t cap)
{
    const char *p = strstr(cfg, key);
    out[0] = 0;
    if (!p) return;
    p += strlen(key);
    size_t n = strcspn(p, " \"\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static bool parse_cfg(const char *cfg, struct boot_cfg *b)
{
    memset(b, 0, sizeof *b);
    if (strncmp(cfg, "# Kestrel ", 10)) return false;
    size_t n = strcspn(cfg + 10, ": \n");
    if (!n || n >= sizeof b->version) return false;
    memcpy(b->version, cfg + 10, n);
    char v[40];
    cfg_value(cfg, "set kestrel_slot=", v, sizeof v);
    b->slot = v[0] == 'b' ? 'b' : 'a';
    const char *cl = strstr(cfg, "set kestrel_cmdline=");       /* before the previous version's */
    if (!cl) return false;
    cfg_value(cl, "kestrel.uuid=", b->uuid, sizeof b->uuid);
    cfg_value(cl, "kestrel.keymap=", b->keymap, sizeof b->keymap);
    if (!b->keymap[0]) snprintf(b->keymap, sizeof b->keymap, "us");
    cfg_value(cfg, "set timeout=", v, sizeof v);
    b->timeout = v[0] ? atoi(v) : 3;
    cfg_value(cfg, "set kestrel_prev_version=", b->prev_version, sizeof b->prev_version);
    return b->uuid[0] != 0;
}

int find_installed(struct disk *d, int nd, struct installed *out, int max)
{
    int n = 0;
    for (int i = 0; i < nd && n < max; i++) {
        struct disk *dk = &d[i];
        if (dk->in_use) continue;
        int fd = dev_open(dk->name, O_RDONLY);
        if (fd < 0) continue;
        for (int k = 0; k < dk->nparts && n < max; k++) {
            struct part *esp = &dk->part[k];
            if (esp->fs != FS_FAT) continue;
            char cfg[513];
            size_t sz = 0;
            if (fat_read(fd, esp->start * SECTOR, "boot/grub/local.cfg", cfg, 512, &sz) != 0) continue;
            cfg[sz < 512 ? sz : 512] = 0;
            struct installed in = { .disk = dk, .esp = esp };
            if (!parse_cfg(cfg, &in.cfg)) continue;
            snprintf(in.version, sizeof in.version, "%s", in.cfg.version);
            /* its boot volume and root: the next ones after it on the disk */
            for (int j = 0; j < dk->nparts; j++) {
                struct part *p = &dk->part[j];
                if (p->fs == FS_KESTREL && p->start > esp->start && (!in.bootv || p->start < in.bootv->start)) in.bootv = p;
            }
            for (int j = 0; in.bootv && j < dk->nparts; j++) {
                struct part *p = &dk->part[j];
                if ((p->fs == FS_EXT2 || p->fs == FS_EXT3 || p->fs == FS_EXT4) && !strcmp(p->label, "kestrel-root") &&
                    p->start > in.bootv->start && (!in.root || p->start < in.root->start)) in.root = p;
            }
            if (!in.bootv || !in.root) { ilog("%s: local.cfg without its boot volume or root, left out", esp->dev); continue; }
            ilog("installed: Kestrel %s on %s (%s, %s, %s), slot %c", in.version, dk->name, esp->dev, in.bootv->dev, in.root->dev, in.cfg.slot);
            out[n++] = in;
        }
        close(fd);
    }
    return n;
}

int version_cmp(const char *a, const char *b)
{
    for (;;) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*ea != '.' || *eb != '.') return strcmp(ea, eb) < 0 ? -1 : strcmp(ea, eb) > 0;
        a = ea + 1;
        b = eb + 1;
    }
}

/* ---- the file list (manifest) -------------------------------------------------- */
struct ment { char *path; uint32_t crc; uint64_t size; int64_t mtime; char kind; };
struct mtab { struct ment *e; size_t cap, n; };

static size_t mhash(const char *s) { size_t h = 1469598103934665603ull; while (*s) h = (h ^ (unsigned char)*s++) * 1099511628211ull; return h; }

static struct ment *mfind(const struct mtab *t, const char *path)
{
    if (!t->cap) return NULL;
    for (size_t i = mhash(path) & (t->cap - 1);; i = (i + 1) & (t->cap - 1)) {
        if (!t->e[i].path) return NULL;
        if (!strcmp(t->e[i].path, path)) return &t->e[i];
    }
}

static void madd(struct mtab *t, const struct ment *m)
{
    if ((t->n + 1) * 2 > t->cap) {
        struct mtab nt = { .cap = t->cap ? t->cap * 2 : 4096 };
        nt.e = calloc(nt.cap, sizeof *nt.e);
        if (!nt.e) return;
        for (size_t i = 0; i < t->cap; i++) if (t->e[i].path) madd(&nt, &t->e[i]);
        free(t->e);
        *t = nt;
    }
    size_t i = mhash(m->path) & (t->cap - 1);
    while (t->e[i].path && strcmp(t->e[i].path, m->path)) i = (i + 1) & (t->cap - 1);
    if (!t->e[i].path) t->n++;
    else free(t->e[i].path);
    t->e[i] = *m;
}

static void mload(struct mtab *t, const char *file)
{
    char *s = read_file(file, NULL);
    if (!s) return;
    for (char *l = strtok(s, "\n"); l; l = strtok(NULL, "\n")) {
        struct ment m = { 0 };
        unsigned crc = 0;
        unsigned long long size = 0;
        long long mt = 0;
        int off = 0;
        if (l[0] == 'f' && sscanf(l, "f %x %llu %lld %n", &crc, &size, &mt, &off) == 3 && off) {
            m = (struct ment){ strdup(l + off), crc, size, mt, 'f' };
        } else if (l[0] == 'l' && l[1] == ' ') {
            m = (struct ment){ strdup(l + 2), 0, 0, 0, 'l' };
        } else continue;
        if (m.path) madd(t, &m);
    }
    free(s);
}

static void mfree(struct mtab *t)
{
    for (size_t i = 0; i < t->cap; i++) free(t->e[i].path);
    free(t->e);
    memset(t, 0, sizeof *t);
}

/* ---- the root's files ---------------------------------------------------------- */
static const char *const keep_dirs[] = { "/home", "/root", "/var/log", "/var/spool", "/var/mail", "/srv", "/usr/local", NULL };
static const char *const accounts[] = { "/etc/passwd", "/etc/group", "/etc/shadow", NULL };

struct upd {
    struct mtab old;
    bool have_old;
    char conflicts[1024];       /* the .kestrel-new files, for the last screen */
    int nconflicts, kept, removed;
};
static struct upd U;

static bool under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return !strncmp(path, dir, n) && (path[n] == '/' || !path[n]);
}

static enum copy_action decide(const char *rel, const char *src, const char *dst, const struct stat *st,
                               uint32_t *crc, bool *have, void *ctx)
{
    (void)ctx;
    struct ment *m = mfind(&U.old, rel);
    if (m && m->kind != 'f') m = NULL;
    bool etc = under(rel, "/etc");
    struct stat t;
    if (lstat(dst, &t) < 0) {
        if (etc && m) { ilog("%s: deleted on this system, stays deleted", rel); return CA_SKIP; }
        return CA_COPY;
    }
    if (!S_ISREG(t.st_mode)) return CA_COPY;
    if (!etc) {
        if (t.st_size == st->st_size && t.st_mtime == st->st_mtime) {
            if (m && m->size == (uint64_t)st->st_size && m->mtime == (int64_t)st->st_mtime) { *crc = m->crc; *have = true; }
            return CA_SKIP;
        }
        return CA_COPY;
    }
    for (int i = 0; accounts[i]; i++) if (!strcmp(rel, accounts[i])) return CA_SKIP;     /* merged afterwards */
    uint32_t tc = 0, sc = 0;
    if (file_crc(dst, &tc, NULL) < 0) return CA_COPY;
    if (m && tc == m->crc && (uint64_t)t.st_size == m->size) return CA_COPY;        /* not changed here */
    if (file_crc(src, &sc, NULL) == 0) { *crc = sc; *have = true; }
    if (*have && sc == tc) return CA_SKIP;                                          /* the same already */
    if (*have && m && sc == m->crc) { U.kept++; return CA_SKIP; }                   /* changed here only */
    if (!m && !U.have_old) { U.kept++; return CA_SKIP; }                            /* no list: keep it */
    ilog("%s: changed on this system and in this release: the new one is %s.kestrel-new", rel, rel);
    size_t n = strlen(U.conflicts);
    if (n + strlen(rel) + 16 < sizeof U.conflicts) snprintf(U.conflicts + n, sizeof U.conflicts - n, "%s%s.kestrel-new", n ? "\n" : "", rel);
    U.nconflicts++;
    return CA_NEW;
}

/* The third field of an account line: the uid or gid. */
static long id_of(const char *l)
{
    for (int k = 0; k < 2; k++) { l = strchr(l, ':'); if (!l) return -1; l++; }
    return strtol(l, NULL, 10);
}

static bool has_name(const char *text, const char *l, size_t nl)
{
    for (const char *q = text; q && *q; q = strchr(q, '\n'), q = q ? q + 1 : NULL)
        if (!strncmp(q, l, nl + 1)) return true;
    return false;
}

/* The release's system accounts and groups (below 1000, or nobody's 65534)
 * that the system lacks, by name, are added; its shadow lines for the
 * accounts that the system then has. The live system's own "user" (1000),
 * which the installer replaced by the user's account, stays out. */
static void merge_accounts(void)
{
    for (int i = 0; accounts[i]; i++) {
        char sp[256], tp[256];
        snprintf(sp, sizeof sp, "%s%s", strcmp(root_src, "/") ? root_src : "", accounts[i]);
        snprintf(tp, sizeof tp, TARGET "%s", accounts[i]);
        char *s = read_file(sp, NULL), *t = read_file(tp, NULL);
        if (!s || !t) { free(s); free(t); continue; }
        size_t tn = strlen(t), cap = tn + strlen(s) + 2;
        char *out = malloc(cap);
        if (!out) { free(s); free(t); continue; }
        memcpy(out, t, tn + 1);
        if (tn && out[tn - 1] != '\n') { out[tn++] = '\n'; out[tn] = 0; }
        int added = 0;
        for (char *l = strtok(s, "\n"); l; l = strtok(NULL, "\n")) {
            size_t nl = strcspn(l, ":");
            if (!nl || !l[nl]) continue;
            if (has_name(t, l, nl)) continue;
            if (i < 2) {                                    /* passwd, group */
                long id = id_of(l);
                if (id < 0 || (id >= 1000 && id != 65534)) continue;
            } else {                                        /* shadow: accounts the system has */
                char *pw = read_file(TARGET "/etc/passwd", NULL);
                bool known = pw && has_name(pw, l, nl);
                free(pw);
                if (!known) continue;
            }
            tn += (size_t)sprintf(out + tn, "%s\n", l);
            added++;
            ilog("%s: added %.*s", accounts[i], (int)nl, l);
        }
        struct stat st;
        if (added && stat(tp, &st) == 0) write_file(tp, out, st.st_mode & 07777);
        free(out);
        free(s);
        free(t);
    }
}

/* What the old release had and this one has not: gone, unless changed here. */
static void remove_gone(const char *const *skip)
{
    struct mtab now = { 0 };
    mload(&now, MANIFEST_TMP);
    if (!now.n) { mfree(&now); return; }
    for (size_t i = 0; i < U.old.cap; i++) {
        struct ment *m = &U.old.e[i];
        if (!m->path || mfind(&now, m->path)) continue;
        bool keep = false;
        for (int k = 0; skip[k] && !keep; k++) if (under(m->path, skip[k])) keep = true;
        if (keep) continue;
        char p[4096];
        snprintf(p, sizeof p, TARGET "%s", m->path);
        struct stat st;
        if (lstat(p, &st) < 0) continue;
        uint32_t c;
        if (m->kind == 'l' ? S_ISLNK(st.st_mode)
                           : S_ISREG(st.st_mode) && (uint64_t)st.st_size == m->size && file_crc(p, &c, NULL) == 0 && c == m->crc) {
            if (unlink(p) == 0) { U.removed++; ilog("removed %s (not in this release)", m->path); }
        } else ilog("%s: not in this release, but changed here: kept", m->path);
    }
    mfree(&now);
}

static int update_root(struct installed *in, char *err, size_t cap)
{
    char rdev[48];
    snprintf(rdev, sizeof rdev, "/dev/%s", in->root->dev);
    mkdir("/mnt", 0755);
    mkdir(TARGET, 0755);
    if (mount(rdev, TARGET, "ext2", 0, NULL) < 0) { snprintf(err, cap, "mounting %s: %s", rdev, strerror(errno)); return -1; }
    memset(&U, 0, sizeof U);
    mload(&U.old, TARGET MANIFEST);
    U.have_old = U.old.n > 0;
    if (!U.have_old) ilog("no %s on the installed system: changed files in /etc are all kept", MANIFEST);
    static const char *skip[96];
    int n = 0;
    const char **base = skip_list();
    for (int i = 0; base[i] && n < 80; i++) skip[n++] = base[i];
    for (int i = 0; keep_dirs[i]; i++) skip[n++] = keep_dirs[i];
    skip[n] = NULL;
    FILE *mf = fopen(MANIFEST_TMP, "w");
    struct copy_opts co = { .skip = skip, .cb = copy_progress, .replace = true, .decide = decide, .manifest = mf };
    int r = copy_tree_opts(root_src, TARGET, &co, err, cap);
    if (mf && fclose(mf) != 0) { ilog("writing %s: %s", MANIFEST_TMP, strerror(errno)); mf = NULL; }
    if (r == 0) {
        merge_accounts();
        if (mf && U.have_old) remove_gone(skip);
        if (mf) save_manifest(TARGET);
        char *t = read_file(TARGET "/etc/kestrel/installed", NULL);
        char out[2048], day[16];
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        snprintf(day, sizeof day, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        size_t o = 0;
        out[0] = 0;
        for (char *l = t ? strtok(t, "\n") : NULL; l; l = strtok(NULL, "\n"))
            if (strncmp(l, "version=", 8) && o + strlen(l) + 2 < sizeof out) o += (size_t)sprintf(out + o, "%s\n", l);
        snprintf(out + o, sizeof out - o, "version=%s\nupdated=%s from %s\n", version, day, in->version);
        free(t);
        mkdir(TARGET "/etc/kestrel", 0755);
        write_file(TARGET "/etc/kestrel/installed", out, 0644);
        ilog("files: %d settings files kept as changed here, %d new versions beside them, %d removed", U.kept, U.nconflicts, U.removed);
        char *log = read_file(LOG, NULL);
        if (log) { write_file(TARGET "/var/log/kestrel-update.log", log, 0640); free(log); }
    }
    mfree(&U.old);
    sync();
    if (umount(TARGET) < 0) ilog("umount %s: %s", TARGET, strerror(errno));
    return r;
}

/* ---- the update ------------------------------------------------------------------ */
static int esp_rewrite(struct installed *in, const char *path, const void *data, size_t n, char *err, size_t cap)
{
    int fd = dev_open(in->esp->dev, O_RDWR);
    if (fd < 0) { snprintf(err, cap, "opening /dev/%s: %s", in->esp->dev, strerror(errno)); return -1; }
    int r = fat_rewrite(fd, 0, path, data, n, err, cap);
    if (r == 0 && fsync(fd) < 0) { snprintf(err, cap, "writing /dev/%s: %s", in->esp->dev, strerror(errno)); r = -1; }
    close(fd);
    if (r == 0) ilog("%s on %s: %zu bytes", path, in->esp->dev, n);
    return r;
}

int do_update(struct installed *in, char *err, size_t cap)
{
    char slot = in->cfg.slot == 'a' ? 'b' : 'a';
    ilog("updating Kestrel %s on %s to %s: slot %c to %c", in->version, in->disk->name, version, in->cfg.slot, slot);
    progress_heading = "Updating Kestrel";

    begin("Installing the new boot volume", 0, 0.08);
    uint8_t uuid[16];
    if (copy_bootvol(in->bootv->dev, slot - 'a', uuid, err, cap) < 0) return -1;

    begin("Installing the new kernel", 0.08, 0.1);
    size_t kn = 0;
    void *kernel = read_file(IMAGES "/kernel.elf", &kn);
    if (!kernel) { snprintf(err, cap, "cannot read %s", IMAGES "/kernel.elf"); return -1; }
    char kpath[32];
    snprintf(kpath, sizeof kpath, "boot/kernel-%c.elf", slot);
    int r = esp_rewrite(in, kpath, kernel, kn, err, cap);
    free(kernel);
    if (r < 0) return -1;

    begin("Updating the files", 0.1, 0.92);
    if (update_root(in, err, cap) < 0) return -1;

    begin("Updating the boot loader", 0.92, 1.0);
    size_t en = 0;
    void *efi = read_file(IMAGES "/bootx64.efi", &en);
    if (efi) {
        r = esp_rewrite(in, "EFI/BOOT/BOOTX64.EFI", efi, en, err, cap);
        free(efi);
        if (r < 0) return -1;
    }
    uint64_t first = ~0ull;
    for (int i = 0; i < in->disk->nparts; i++)
        if (in->disk->part[i].no <= 4 && in->disk->part[i].start < first) first = in->disk->part[i].start;
    if (write_grub(in->disk->name, first, in->bootv->start, err, cap) < 0) return -1;

    struct boot_cfg b = { .slot = slot, .timeout = in->cfg.timeout, .prev_slot = in->cfg.slot };
    snprintf(b.keymap, sizeof b.keymap, "%s", in->cfg.keymap);
    snprintf(b.version, sizeof b.version, "%s", version);
    uuid_str(uuid, b.uuid);
    snprintf(b.prev_uuid, sizeof b.prev_uuid, "%s", in->cfg.uuid);
    snprintf(b.prev_version, sizeof b.prev_version, "%s", in->version);
    char cfg[512];
    if (esp_cfg(cfg, &b) < 0) { snprintf(err, cap, "local.cfg is too long"); return -1; }
    if (esp_rewrite(in, "boot/grub/local.cfg", cfg, 512, err, cap) < 0) return -1;
    sync();
    progress(1, "done");
    ilog("update complete: Kestrel %s on %s starts from slot %c", version, in->disk->name, slot);
    return 0;
}

/* ---- full screen and command line ----------------------------------------------- */
bool step_update(struct installed *in)
{
    char text[1024];
    int c = version_cmp(version, in->version);
    int n = c ? snprintf(text, sizeof text, "This updates Kestrel %s on %s (%s) to Kestrel %s.", in->version, in->disk->name,
                         in->disk->model[0] ? in->disk->model : in->disk->kind, version)
              : snprintf(text, sizeof text, "This writes Kestrel %s's system files onto %s (%s) again.", version, in->disk->name,
                         in->disk->model[0] ? in->disk->model : in->disk->kind);
    snprintf(text + n, sizeof text - (size_t)n,
             "\n\n"
             "Your files (/home, /root), accounts and settings stay. Settings files in /etc that you changed stay as they are; "
             "where this release changes one too, its version goes beside yours as <file>.kestrel-new.\n\n"
             "Kestrel %s stays on the disk as GRUB's \"previous version\" entries until the next update. Back up anything you care about first.",
             in->version);
    if (!ui_confirm(c ? "Update Kestrel?" : "Reinstall Kestrel's system files?", text, c ? "Update now" : "Reinstall now", "Go back", false)) return false;
    char err[512] = "";
    if (do_update(in, err, sizeof err) < 0) {
        ilog("FAILED: %s", err);
        char t[1024];
        snprintf(t, sizeof t, "The update failed:\n\n%s\n\nThe log is in " LOG ". GRUB still starts Kestrel %s: the switch to the "
                 "new version is the update's last step. Its files may be partly updated; running the update again finishes them.",
                 err, in->version);
        ui_message("Update failed", t);
        return true;
    }
    char done[1536];
    n = c ? snprintf(done, sizeof done, "Kestrel %s on %s is updated to %s.", in->version, in->disk->name, version)
          : snprintf(done, sizeof done, "Kestrel %s's system files are written onto %s again.", version, in->disk->name);
    n += snprintf(done + n, sizeof done - (size_t)n, " Restart the computer and remove the stick or disc to use it.");
    if (U.nconflicts)
        snprintf(done + n, sizeof done - (size_t)n, "\n\nYou changed these settings files and this release changes them too; "
                 "its versions are beside yours:\n%s", U.conflicts);
    struct menu_item it[] = {
        { "Restart now", "Remove the stick when the screen goes dark.", false },
        { "Continue testing", "Stay on the stick for now.", false },
    };
    if (ui_menu(c ? "Update complete" : "Reinstall complete", done, it, 2, 0) == 0) {
        ui_end();
        ui_on = false;
        sync();
        const char *argv[] = { "reboot", NULL };
        execvp(argv[0], (char *const *)argv);
        reboot(RB_AUTOBOOT);
    }
    return true;
}

/* kestrel-install --update [DISK]: no questions */
int update_main(struct disk *d, int nd, int argc, char **argv)
{
    struct installed in[8];
    int n = find_installed(d, nd, in, 8);
    if (!n) {
        fprintf(stderr, "kestrel-install: no installed Kestrel found. An installed system is updated from the new release's stick or ISO.\n");
        return 1;
    }
    const char *want = argc > 0 ? argv[0] : NULL;
    struct installed *t = NULL;
    for (int i = 0; i < n; i++) if (!want || !strcmp(want, in[i].disk->name)) { if (t) { t = NULL; want = "?"; break; } t = &in[i]; }
    if (!t) {
        fprintf(stderr, "kestrel-install: %s; installed systems:\n", want ? "name the disk" : "no installed Kestrel on that disk");
        for (int i = 0; i < n; i++) fprintf(stderr, "  %s: Kestrel %s\n", in[i].disk->name, in[i].version);
        return 1;
    }
    if (version_cmp(version, t->version) < 0 && !(argc > 1 && !strcmp(argv[1], "--force"))) {
        fprintf(stderr, "kestrel-install: %s has Kestrel %s, newer than this %s (--update %s --force to go back)\n",
                t->disk->name, t->version, version, t->disk->name);
        return 1;
    }
    printf("Updating Kestrel %s on %s to %s\n", t->version, t->disk->name, version);
    char err[512] = "";
    if (do_update(t, err, sizeof err) < 0) { fprintf(stderr, "kestrel-install: FAILED: %s\n", err); ilog("FAILED: %s", err); return 1; }
    printf("kestrel-install: Kestrel %s on %s is updated to %s\n", t->version, t->disk->name, version);
    if (U.nconflicts) printf("New versions of settings files you changed:\n%s\n", U.conflicts);
    return 0;
}
