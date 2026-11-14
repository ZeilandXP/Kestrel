/* user/install/install.c -- kestrel-install, the Kestrel installer (beta)
 *
 * The steps follow Ubuntu's installer: Welcome, Keyboard, Installation
 * type, Disk, Time zone, Who are you, then a last look at the changes and
 * the installation with its progress. Nothing is written before that last
 * confirmation; Esc goes back a step at any time.
 *
 * Installation types:
 *   Erase disk and install Kestrel   a new MBR; the root file system takes
 *                                    the rest of the disk
 *   Install alongside ...            free space of a disk with Windows or
 *                                    Linux on it (an MBR disk)
 *   Something else                   the disk, the space (free space, a
 *                                    partition to replace, or the whole
 *                                    disk) and the root's size
 * Kestrel gets three partitions: an EFI System Partition (FAT32: GRUB for
 * UEFI, the kernel, GRUB's local.cfg), the boot volume (two slots, for the
 * updater) and the ext2 root, made with mke2fs and filled with the running
 * system's files. GRUB goes into the MBR and the sectors after it, as on
 * the stick; its menu lists the other systems it finds.
 *
 * What it installs comes from the running system, so it works the same
 * from a stick, from the ISO (also through Ventoy) or from an installed
 * Kestrel: GRUB's images and the kernel from /boot/share/kestrel/boot, the
 * boot volume from its device (/proc/bootvol), and the root file system's
 * files (or the boot volume's, when the root is in memory).
 *
 *   kestrel-install               the steps, full screen
 *   kestrel-install --list        the disks and what each offers
 *   kestrel-install --auto FILE   no questions: the answers in FILE
 *                                 (key=value lines; see auto_load()) */
#define _GNU_SOURCE
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/reboot.h>

#define VERSION_FILE    "/proc/sys/kernel/osrelease"
#define IMAGES          "/boot/share/kestrel/boot"
#define TARGET          "/mnt/kestrel-target"
#define LOG             "/var/log/kestrel-install.log"

static char version[32] = "?";
static struct disk disks[MAX_DISKS];
static int ndisks;
static struct settings S = { .autologin = true };
static struct plan P;
static uint64_t boot_vol_bytes, root_need;      /* what the system needs on the disk */
static char boot_src[32];                       /* the running boot volume's device */
static uint64_t boot_src_block;
static const char *root_src = "/";
static bool auto_mode, ui_on;
static char kind_choice = 'e';                  /* e(rase) a(longside) s(omething else) */

static const char *const steps[] = { "Welcome", "Keyboard", "Installation type", "Disk", "Time zone", "Who are you", "Install" };
enum { ST_WELCOME, ST_KEYBOARD, ST_TYPE, ST_DISK, ST_TZ, ST_USER, ST_INSTALL, ST_DONE };

/* ---- what is there to install ------------------------------------------------ */
static void trim(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0; }

static int sources(char *err, size_t cap)
{
    char *v = read_file(VERSION_FILE, NULL);
    if (v) { snprintf(version, sizeof version, "%s", v); trim(version); free(v); }
    char *bv = read_file("/proc/bootvol", NULL);
    char *d = bv ? strstr(bv, "device      /dev/") : NULL;
    unsigned long long blk = 0, bytes = 0;
    if (!d || sscanf(d + 17, "%31[^,], header block %llu, %llu payload bytes", boot_src, &blk, &bytes) != 3) {
        free(bv);
        snprintf(err, cap, "Kestrel was not started from a boot volume (see /proc/bootvol), so there is nothing to install.");
        return -1;
    }
    free(bv);
    boot_src_block = blk;
    boot_vol_bytes = 4096 + bytes;
    static const char *const need[] = { IMAGES "/boot.img", IMAGES "/core.img", IMAGES "/kernel.elf", NULL };
    for (int i = 0; need[i]; i++)
        if (access(need[i], R_OK) < 0) {
            snprintf(err, cap, "%s is missing: this Kestrel was built without the installer's boot files.", need[i]);
            return -1;
        }
    /* the root's files: the root file system if it is on a disk, else the boot volume's */
    char *m = read_file("/proc/mounts", NULL);
    bool disk_root = false;
    for (char *l = m ? strtok(m, "\n") : NULL; l; l = strtok(NULL, "\n")) {
        char src[64], mnt[64], type[32];
        if (sscanf(l, "%63s %63s %31s", src, mnt, type) == 3 && !strcmp(mnt, "/") && !strcmp(type, "ext2")) disk_root = true;
    }
    free(m);
    root_src = disk_root ? "/" : "/boot";
    return 0;
}

static const char *const base_skip[] = { "/proc", "/sys", "/dev", "/run", "/tmp", "/boot", "/mnt", "/media",
                                          "/lost+found", "/var/tmp", NULL };
static const char **skip_list(void)
{
    static const char *list[64];
    int n = 0;
    if (!strcmp(root_src, "/")) {
        for (int i = 0; base_skip[i]; i++) list[n++] = base_skip[i];
        char *m = read_file("/proc/mounts", NULL);         /* and every other mount point */
        for (char *l = m ? strtok(m, "\n") : NULL; l && n < 62; l = strtok(NULL, "\n")) {
            char src[64], mnt[200];
            if (sscanf(l, "%63s %199s", src, mnt) == 2 && strcmp(mnt, "/")) list[n++] = strdup(mnt);
        }
        free(m);
    }
    list[n] = NULL;
    return list;
}

static void sizes(void)
{
    uint64_t used = tree_size(root_src, skip_list(), NULL);
    root_need = used + used / 5 + 512 * MIB;            /* room to grow */
}

static uint64_t boot_part_sectors(void)
{
    uint64_t slot = 3 * boot_vol_bytes;
    if (slot < 512 * MIB) slot = 512 * MIB;
    slot = (slot + MIB - 1) / MIB * MIB;
    return 2 * slot / SECTOR;
}

static bool usable(const struct disk *d) { return !d->in_use && d->block_size == 512 && d->sectors * SECTOR >= 2 * root_need; }

static bool any_alongside(void)
{
    for (int i = 0; i < ndisks; i++) {
        if (disks[i].in_use || !disks[i].systems[0]) continue;
        for (int f = 0; f < disks[i].nfree; f++) {
            struct plan p = { .disk = &disks[i], .where = disks[i].free[f] };
            if (plan_layout(&p, boot_part_sectors()) && p.np[2].sectors * SECTOR >= root_need) return true;
        }
    }
    return false;
}

static void disk_line(const struct disk *d, char *out, size_t cap)
{
    char sz[24];
    size_str(d->sectors * SECTOR, sz, sizeof sz);
    snprintf(out, cap, "%-7s %9s  %s%s%s", d->name, sz, d->model[0] ? d->model : d->kind,
             d->systems[0] ? "  -  " : "", d->systems);
}

/* ---- the steps ------------------------------------------------------------- */
static int step_welcome(void)
{
    char text[1024];
    snprintf(text, sizeof text,
             "This puts Kestrel %s on a disk of this computer, so that it starts without the stick.\n\n"
             "This installer is a beta: back up anything you care about first. Nothing is written to any "
             "disk until the last step, where you see every change and confirm it.", version);
    struct menu_item it[] = {
        { "Install Kestrel", "Choose a keyboard, a disk, a time zone and your account, then install", false },
        { "Try Kestrel", "Leave the installer and keep using Kestrel from the stick", false },
    };
    int c = ui_menu("Welcome", text, it, 2, 0);
    return c == 0 ? 1 : -2;                     /* -2: quit */
}

static void apply_layout(int l)
{
    int fd = open("/proc/sys/kernel/keymap", O_WRONLY);
    if (fd >= 0) { ssize_t w = write(fd, layouts[l].kernel, strlen(layouts[l].kernel)); (void)w; close(fd); }
    if (getenv("DISPLAY")) {
        const char *argv[] = { "setxkbmap", layouts[l].xkb, NULL };
        run(argv, NULL, 0);
    }
}

static int step_keyboard(void)
{
    struct menu_item it[16];
    for (int i = 0; i < layout_count; i++) it[i] = (struct menu_item){ layouts[i].name, NULL, false };
    int c = ui_menu("Keyboard layout", "Choose your keyboard layout. It applies at once, here and on the installed system.",
                    it, layout_count, S.layout);
    if (c < 0) return -1;
    S.layout = c;
    apply_layout(c);
    char test[128] = "";
    struct field f[] = { { "Type here to test your keyboard:", test, sizeof test, false, "Enter: continue" } };
    if (ui_form("Keyboard layout", layouts[c].name, f, 1, NULL) < 0) return 0;     /* back to the list */
    return 1;
}

static int step_type(void)
{
    int candidates = 0;
    for (int i = 0; i < ndisks; i++) if (usable(&disks[i])) candidates++;
    if (!candidates) {
        char text[2048], sz[24];
        size_str(2 * root_need, sz, sizeof sz);
        int n = snprintf(text, sizeof text, "There is no disk Kestrel can be installed on: it needs a SATA, NVMe or virtio disk "
                         "with 512-byte sectors and %s at least, that the running system is not using.\n\nDisks Kestrel sees:\n", sz);
        for (int i = 0; i < ndisks && n < (int)sizeof text - 200; i++) {
            char l[128];
            disk_line(&disks[i], l, sizeof l);
            n += snprintf(text + n, sizeof text - (size_t)n, "  %s%s%s\n", l, disks[i].in_use ? ": " : "", disks[i].in_use ? disks[i].in_use_why : "");
        }
        ui_message("Installation type", text);
        return -1;
    }
    char systems[160] = "";
    for (int i = 0; i < ndisks; i++)
        if (!disks[i].in_use && disks[i].systems[0] && !strstr(systems, disks[i].systems))
            snprintf(systems + strlen(systems), sizeof systems - strlen(systems), "%s%s", systems[0] ? ", " : "", disks[i].systems);
    char text[512], along[200];
    if (systems[0]) snprintf(text, sizeof text, "This computer has %s on its disks. What would you like to do?", systems);
    else snprintf(text, sizeof text, "This computer has no other system Kestrel recognizes. What would you like to do?");
    bool can_along = any_alongside();
    snprintf(along, sizeof along, "Install Kestrel alongside %s", systems[0] ? systems : "the other systems");
    struct menu_item it[] = {
        { "Erase disk and install Kestrel", "Everything on the disk you choose is deleted: its systems, programs and files.", false },
        { along, can_along ? "Kestrel goes into free space on the disk. You choose a system each time the computer starts."
                           : "Needs free space on a disk with another system (an MBR disk; see the disk step for why).", !can_along },
        { "Something else", "Choose the disk, the space Kestrel takes and the size of its file system yourself.", false },
    };
    int sel = kind_choice == 'a' ? 1 : kind_choice == 's' ? 2 : 0;
    int c = ui_menu("Installation type", text, it, 3, sel);
    if (c < 0) return -1;
    kind_choice = "eas"[c];
    return 1;
}

static void plan_text(char *t, size_t cap)
{
    size_t n = 0;
    char sz[24];
    struct disk *d = P.disk;
    if (P.erase) {
        n += (size_t)snprintf(t + n, cap - n, "The disk %s (%s) gets a new partition table. EVERYTHING ON IT IS DELETED%s%s.\n\n",
                              d->name, d->model, d->systems[0] ? ", including " : "", d->systems);
    } else {
        if (P.delete_part) n += (size_t)snprintf(t + n, cap - n, "Partition %sp%d of %s is deleted, with all its files.\n",
                                                 d->name, P.delete_part, d->name);
        n += (size_t)snprintf(t + n, cap - n, "The partition table of %s gets these new partitions%s; its other partitions stay as they are.\n\n",
                              d->name, P.logical ? " (in a new extended partition)" : "");
    }
    for (int i = 0; i < P.nnp && n < cap; i++) {
        size_str(P.np[i].sectors * SECTOR, sz, sizeof sz);
        n += (size_t)snprintf(t + n, cap - n, "  %-9s %9s  %s\n", P.np[i].dev, sz, P.np[i].role);
    }
    if (n < cap) n += (size_t)snprintf(t + n, cap - n, "\nThe boot loader (GRUB) is written to the start of %s%s.", d->name,
                                       P.erase ? "" : "; its menu also starts the systems already there");
}

/* A partition's space, joined with the free space right around it */
static struct region replace_region(const struct disk *d, const struct part *pt)
{
    uint64_t s = pt->start, e = pt->start + pt->sectors;
    for (int f = 0; f < d->nfree; f++) {
        const struct region *r = &d->free[f];
        if (r->in_extended) continue;
        if (r->start + r->sectors + ALIGN_SECTORS >= s && r->start < s) s = r->start;
        if (r->start <= e + ALIGN_SECTORS && r->start + r->sectors > e) e = r->start + r->sectors;
    }
    s = (s + ALIGN_SECTORS - 1) / ALIGN_SECTORS * ALIGN_SECTORS;
    return (struct region){ s, e / ALIGN_SECTORS * ALIGN_SECTORS - s, false };
}

static int pick_root_size(void)
{
    uint64_t lo = root_need, hi = P.np[2].sectors * SECTOR;
    char buf[32];
    snprintf(buf, sizeof buf, "%llu", (unsigned long long)(hi / MIB / 1024 ? hi / MIB : hi / MIB));
    char hint[96], a[24], b[24];
    size_str(lo, a, sizeof a);
    size_str(hi, b, sizeof b);
    snprintf(hint, sizeof hint, "in MiB, from %llu (%s) to %llu (%s)", (unsigned long long)(lo / MIB), a,
             (unsigned long long)(hi / MIB), b);
    for (;;) {
        struct field f[] = { { "Size of Kestrel's file system:", buf, sizeof buf, false, hint } };
        if (ui_form("Something else", "How much of this space should Kestrel's root file system take? What is left stays free.",
                    f, 1, NULL) < 0) return -1;
        uint64_t mib = strtoull(buf, NULL, 10);
        if (mib * MIB >= lo && mib * MIB <= hi) {
            P.root_sectors = mib * MIB / SECTOR;
            return plan_layout(&P, boot_part_sectors()) ? 1 : -1;
        }
    }
}

static int step_disk(void)
{
    memset(&P, 0, sizeof P);
    struct menu_item it[48];
    char lines[48][160], details[48][160];
    struct { int disk; int region; int part; bool erase; } map[48];
    int n = 0;
    if (kind_choice == 'e') {
        for (int i = 0; i < ndisks && n < 16; i++) {
            disk_line(&disks[i], lines[n], sizeof lines[n]);
            bool ok = usable(&disks[i]);
            snprintf(details[n], sizeof details[n], "%s", disks[i].in_use ? disks[i].in_use_why :
                     !ok ? "too small, or not 512-byte sectors" : disks[i].removable ? "a USB disk" : "");
            it[n] = (struct menu_item){ lines[n], details[n][0] ? details[n] : NULL, !ok };
            map[n].disk = i; map[n].erase = true; map[n].region = map[n].part = -1;
            n++;
        }
        int c = ui_menu("Erase disk and install Kestrel", "Choose the disk to erase and install Kestrel on.", it, n, 0);
        if (c < 0) return -1;
        P.disk = &disks[map[c].disk];
        P.erase = true;
        if (!plan_layout(&P, boot_part_sectors())) { ui_message("Erase disk and install Kestrel", P.why_not); return 0; }
    } else {
        for (int i = 0; i < ndisks && n < 40; i++) {
            struct disk *d = &disks[i];
            if (!usable(d)) continue;
            if (kind_choice == 's' && n < 40) {
                snprintf(lines[n], sizeof lines[n], "%s: the whole disk (erase it)", d->name);
                disk_line(d, details[n], sizeof details[n]);
                it[n] = (struct menu_item){ lines[n], details[n], false };
                map[n].disk = i; map[n].erase = true; map[n].region = map[n].part = -1;
                n++;
            }
            for (int f = 0; f < d->nfree && n < 40; f++) {
                struct plan p = { .disk = d, .where = d->free[f] };
                bool ok = plan_layout(&p, boot_part_sectors()) && p.np[2].sectors * SECTOR >= root_need;
                if (kind_choice == 'a' && (!d->systems[0])) continue;
                char sz[24];
                size_str(d->free[f].sectors * SECTOR, sz, sizeof sz);
                snprintf(lines[n], sizeof lines[n], "%s: free space, %s%s%s", d->name, sz, d->systems[0] ? " (next to " : "",
                         d->systems[0] ? d->systems : "");
                if (d->systems[0]) strncat(lines[n], ")", sizeof lines[n] - strlen(lines[n]) - 1);
                snprintf(details[n], sizeof details[n], "%s", ok ? (d->model[0] ? d->model : d->kind) : p.why_not[0] ? p.why_not : "too small");
                it[n] = (struct menu_item){ lines[n], details[n], !ok };
                map[n].disk = i; map[n].erase = false; map[n].region = f; map[n].part = -1;
                n++;
            }
            if (kind_choice == 's' && d->mbr)
                for (int k = 0; k < d->nparts && n < 40; k++) {
                    struct part *pt = &d->part[k];
                    if (pt->fs == FS_EXTENDED || pt->no > 4) continue;
                    char sz[24];
                    size_str(pt->sectors * SECTOR, sz, sizeof sz);
                    snprintf(lines[n], sizeof lines[n], "%s: replace %s (%s, %s%s%s)", d->name, pt->dev, fs_name(pt->fs), sz,
                             pt->label[0] ? ", " : "", pt->label);
                    snprintf(details[n], sizeof details[n], "Everything on %s is deleted%s", pt->dev,
                             pt->windows ? ": this looks like a Windows partition" : "");
                    it[n] = (struct menu_item){ lines[n], details[n], false };
                    map[n].disk = i; map[n].erase = false; map[n].region = -1; map[n].part = k;
                    n++;
                }
        }
        if (!n) { ui_message(steps[ST_DISK], "No disk has room for Kestrel."); return -1; }
        char systems[160] = "", along[200];
        for (int i = 0; i < ndisks; i++)
            if (!disks[i].in_use && disks[i].systems[0] && !strstr(systems, disks[i].systems))
                snprintf(systems + strlen(systems), sizeof systems - strlen(systems), "%s%s", systems[0] ? ", " : "", disks[i].systems);
        snprintf(along, sizeof along, "Install Kestrel alongside %s", systems[0] ? systems : "the other systems");
        const char *h = kind_choice == 'a' ? along : "Something else";
        int c = ui_menu(h, kind_choice == 'a' ? "Choose the free space for Kestrel." :
                        "Choose where Kestrel goes: a whole disk, free space, or a partition it replaces.", it, n, 0);
        if (c < 0) return -1;
        P.disk = &disks[map[c].disk];
        P.erase = map[c].erase;
        if (map[c].region >= 0) P.where = P.disk->free[map[c].region];
        if (map[c].part >= 0) {
            P.delete_part = P.disk->part[map[c].part].no;
            P.where = replace_region(P.disk, &P.disk->part[map[c].part]);
        }
        bool ok = plan_layout(&P, boot_part_sectors());
        if (!ok) { ui_message(h, P.why_not); return 0; }
        if (P.np[2].sectors * SECTOR < root_need) { ui_message(h, "This space is too small for Kestrel's files."); return 0; }
        if (kind_choice == 's' && pick_root_size() < 0) return 0;
    }
    return 1;
}

static int step_tz(void)
{
    struct menu_item it[96];
    char names[96][48];
    for (int i = 0; i < tz_count && i < 96; i++) {
        snprintf(names[i], sizeof names[i], "%s", tz_list[i].name);
        for (char *s = names[i]; *s; s++) if (*s == '_') *s = ' ';
        it[i] = (struct menu_item){ names[i], NULL, false };
    }
    int c = ui_menu("Where are you?", "Choose your time zone (a city in it). The computer's clock is taken as UTC.", it, tz_count, S.tz);
    if (c < 0) return -1;
    S.tz = c;
    return 1;
}

static const char *check_user(struct field *f, int n)
{
    (void)n;
    const char *u = f[2].buf, *h = f[1].buf;
    if (!f[0].buf[0]) return "Please enter your name.";
    if (!u[0]) {                                /* as Ubuntu's: from the name's first word */
        size_t k = 0;
        for (const char *s = f[0].buf; *s && *s != ' ' && k + 1 < f[2].cap && k < 32; s++)
            if (isalnum((unsigned char)*s)) f[2].buf[k++] = (char)tolower((unsigned char)*s);
        f[2].buf[k] = 0;
        if (k && !islower((unsigned char)f[2].buf[0])) f[2].buf[0] = 0;
    }
    if (!h[0] && u[0]) snprintf(f[1].buf, f[1].cap, "%s-kestrel", u);
    if (strchr(f[0].buf, ':') || strchr(f[0].buf, ',')) return "Your name cannot contain : or ,";
    if (!h[0]) return "Please enter a name for this computer.";
    for (const char *s = h; *s; s++) if (!isalnum((unsigned char)*s) && *s != '-') return "The computer's name may have letters, digits and - only.";
    if (h[0] == '-' || strlen(h) > 63) return "The computer's name cannot start with - or be longer than 63 characters.";
    if (!u[0]) return "Please pick a username.";
    if (!islower((unsigned char)u[0]) && u[0] != '_') return "The username must start with a lower-case letter.";
    for (const char *s = u; *s; s++) if (!islower((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '_' && *s != '-')
        return "The username may have lower-case letters, digits, - and _ only.";
    static const char *const taken[] = { "root", "daemon", "bin", "sys", "nobody", "messagebus", "sshd", "www-data", NULL };
    for (int i = 0; taken[i]; i++) if (!strcmp(u, taken[i])) return "That username is used by the system: pick another one.";
    if (!f[3].buf[0]) return "Please choose a password.";
    if (strcmp(f[3].buf, f[4].buf)) return "The passwords do not match.";
    return NULL;
}

static int step_user(void)
{
    if (!S.hostname[0] && S.user[0]) snprintf(S.hostname, sizeof S.hostname, "%s-kestrel", S.user);
    char confirm[128];
    snprintf(confirm, sizeof confirm, "%s", S.password);
    struct field f[] = {
        { "Your name:", S.fullname, sizeof S.fullname, false, NULL },
        { "Your computer's name:", S.hostname, sizeof S.hostname, false, "the name it uses when it talks to other computers" },
        { "Pick a username:", S.user, sizeof S.user, false, NULL },
        { "Choose a password:", S.password, sizeof S.password, true, "root gets the same password" },
        { "Confirm your password:", confirm, sizeof confirm, true, NULL },
    };
    for (;;) {
        if (ui_form("Who are you?", "Kestrel makes an account for you.", f, 5, check_user) < 0) return -1;
        break;
    }
    struct menu_item it[] = {
        { "Log in automatically", "The desktop and the text terminals start with your account, without asking.", false },
        { "Require my password to log in", "The desktop and each text terminal ask for your username and password.", false },
    };
    int c = ui_menu("Who are you?", "How should Kestrel start?", it, 2, S.autologin ? 0 : 1);
    if (c < 0) return 0;
    S.autologin = c == 0;
    return 1;
}

/* ---- installing -------------------------------------------------------------- */
static const char *phase;
static double phase_lo, phase_hi;
static void progress(double f, const char *detail)
{
    double all = phase_lo + (phase_hi - phase_lo) * f;
    if (ui_on) ui_progress("Installing Kestrel", phase, all, detail);
    else { static int last = -1; int pct = (int)(all * 100); if (pct != last) { printf("[%3d%%] %s%s%s\n", pct, phase, detail ? ": " : "", detail ? detail : ""); fflush(stdout); last = pct; } }
}
static void begin(const char *what, double lo, double hi) { phase = what; phase_lo = lo; phase_hi = hi; ilog("== %s", what); progress(0, NULL); }

static void copy_progress(const struct copy_progress *p, void *ctx)
{
    (void)ctx;
    char d[256];
    snprintf(d, sizeof d, "%llu of %llu MB, %llu files", (unsigned long long)(p->bytes / 1000000),
             (unsigned long long)(p->total / 1000000), (unsigned long long)p->files);
    progress(p->total ? (double)p->bytes / (double)p->total : 0, d);
}

static uint32_t crc32z(uint32_t crc, const uint8_t *b, size_t n)
{
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    crc = ~crc;
    while (n--) crc = t[(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void uuid_str(const uint8_t u[16], char *out)
{
    sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static void *slurp(const char *path, size_t *n)
{
    return read_file(path, n);
}

/* The boot volume: copied from its device into the first slot, with a new
 * UUID (so that the stick's and this one are told apart). */
static int install_bootvol(const char *dev, uint8_t uuid[16], char *err, size_t cap)
{
    int in = dev_open(boot_src, O_RDONLY), outfd = dev_open(dev, O_RDWR);
    if (in < 0 || outfd < 0) { snprintf(err, cap, "opening the boot volume devices: %s", strerror(errno)); if (in >= 0) close(in); if (outfd >= 0) close(outfd); return -1; }
    uint8_t hdr[512];
    uint64_t base = boot_src_block * 512;
    int r = dev_pread(in, hdr, 512, base);
    uint64_t poff, plen;
    memcpy(&poff, hdr + 68, 8);         /* struct kestrel_volhdr */
    memcpy(&plen, hdr + 76, 8);
    if (r < 0 || memcmp(hdr, "KESTREL_BOOT", 12) || poff < 512 || poff > 1 << 20) { snprintf(err, cap, "the running boot volume's header is unreadable"); close(in); close(outfd); return -1; }
    random_bytes(uuid, 16);
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
    memcpy(hdr + 20, uuid, 16);
    memset(hdr + 16, 0, 4);
    uint32_t c = crc32z(0, hdr, 512);
    memcpy(hdr + 16, &c, 4);
    uint64_t total = poff + plen;
    char *buf = malloc(MIB);
    if (!buf) { close(in); close(outfd); snprintf(err, cap, "out of memory"); return -1; }
    /* the second slot's header goes first: no stale volume there */
    static uint8_t zero[512];
    struct stat st;
    uint64_t psize = 0;
    if (fstat(outfd, &st) == 0) psize = (uint64_t)lseek(outfd, 0, SEEK_END);
    if (psize >= 2 * 2048 * 512) r = dev_pwrite(outfd, zero, 512, psize / 2 / 512 * 512);
    for (uint64_t o = 512; r == 0 && o < total; o += MIB) {
        size_t n = total - o < MIB ? (size_t)(total - o) : MIB;
        if ((r = dev_pread(in, buf, n, base + o)) == 0) r = dev_pwrite(outfd, buf, n, o);
        progress((double)o / (double)total, NULL);
    }
    if (r == 0) r = dev_pwrite(outfd, hdr, 512, 0);       /* the header last: valid only when complete */
    if (r == 0 && fsync(outfd) < 0) r = -errno;
    free(buf);
    close(in);
    close(outfd);
    if (r < 0) { snprintf(err, cap, "copying the boot volume: %s", strerror(-r)); return -1; }
    char u[40];
    uuid_str(uuid, u);
    ilog("boot volume copied to %s: %llu bytes, uuid %s", dev, (unsigned long long)total, u);
    return 0;
}

static int install_esp(const char *dev, const uint8_t uuid[16], char *err, size_t cap)
{
    size_t kn = 0, en = 0;
    void *kernel = slurp(IMAGES "/kernel.elf", &kn);
    void *efi = slurp(IMAGES "/bootx64.efi", &en);
    if (!kernel) { snprintf(err, cap, "cannot read %s", IMAGES "/kernel.elf"); free(efi); return -1; }
    char u[40], cfg[1024];
    uuid_str(uuid, u);
    const struct layout *l = &layouts[S.layout];
    int n = snprintf(cfg, sizeof cfg,
             "# Kestrel %s: this system's boot settings, read by GRUB (written by\n"
             "# kestrel-install; kestrel-update writes it again for each release)\n"
             "set kestrel_installed=1\n"
             "set kestrel_root=any\n"
             "set kestrel_slot=a\n"
             "set kestrel_kernel=/boot/kernel-a.elf\n"
             "set kestrel_cmdline=\"kestrel.uuid=%s kestrel.keymap=%s\"\n"
             "set kestrel_desktop=\"%s\"\n"
             "set timeout=%d\n",
             version, u, l->kernel, "kestrel.exec=/bin/startx",
             P.erase ? 3 : 10);
    for (; n < 511; n++) cfg[n] = n % 64 == 63 ? '\n' : '#';   /* 512 bytes: one sector, rewritten at once */
    cfg[511] = '\n';
    struct fat_file f[3];
    int nf = 0;
    if (efi) f[nf++] = (struct fat_file){ "EFI/BOOT/BOOTX64.EFI", efi, en };
    f[nf++] = (struct fat_file){ "boot/kernel-a.elf", kernel, kn };
    f[nf++] = (struct fat_file){ "boot/grub/local.cfg", cfg, 512 };
    int fd = dev_open(dev, O_RDWR);
    if (fd < 0) { snprintf(err, cap, "opening /dev/%s: %s", dev, strerror(errno)); free(kernel); free(efi); return -1; }
    uint64_t size = (uint64_t)lseek(fd, 0, SEEK_END);
    uint32_t serial;
    random_bytes(&serial, 4);
    int r = fat_format(fd, 0, size / SECTOR, serial, f, nf, err, cap);
    if (r == 0 && fsync(fd) < 0) r = -1;
    close(fd);
    free(kernel);
    free(efi);
    if (!efi) ilog("no bootx64.efi: the installed system starts by BIOS only");
    return r;
}

static int install_grub(uint64_t boot_start, char *err, size_t cap)
{
    size_t bn = 0, cn = 0;
    uint8_t *boot = slurp(IMAGES "/boot.img", &bn), *core = slurp(IMAGES "/core.img", &cn);
    if (!boot || !core || bn < 440) { free(boot); free(core); snprintf(err, cap, "cannot read GRUB's images"); return -1; }
    uint64_t first = ~0ull;
    for (int i = 0; i < P.disk->nparts; i++) if (P.disk->part[i].start < first) first = P.disk->part[i].start;
    for (int i = 0; i < P.nnp; i++) if (P.np[i].start < first) first = P.np[i].start;
    if (P.erase) first = P.np[0].start;
    uint64_t room = (first - 1) * SECTOR;
    if (cn > room) { free(boot); free(core); snprintf(err, cap, "GRUB (%zu bytes) does not fit before the first partition", cn); return -1; }
    int fd = dev_open(P.disk->name, O_RDWR);
    if (fd < 0) { free(boot); free(core); snprintf(err, cap, "opening /dev/%s: %s", P.disk->name, strerror(errno)); return -1; }
    uint8_t s0[512];
    int r = dev_pread(fd, s0, 512, 0);
    if (r == 0) r = dev_pwrite(fd, core, cn, SECTOR);
    if (r == 0) {
        memcpy(s0, boot, 440);
        memcpy(s0 + 3, "KESTREL_BOOT", 12);             /* option B: where the boot volume starts */
        memcpy(s0 + 15, &boot_start, 8);
        r = dev_pwrite(fd, s0, 512, 0);
    }
    if (r == 0 && fsync(fd) < 0) r = -errno;
    close(fd);
    free(boot);
    free(core);
    if (r < 0) { snprintf(err, cap, "writing GRUB to /dev/%s: %s", P.disk->name, strerror(-r)); return -1; }
    ilog("GRUB written to %s: core.img %zu bytes", P.disk->name, cn);
    return 0;
}

static int do_install(char *err, size_t cap)
{
    ilog("installing Kestrel %s on %s (%s), %s", version, P.disk->name, P.disk->model,
         P.erase ? "erasing it" : P.delete_part ? "replacing a partition" : "in free space");
    char cmd_err[2048];
    /* 1. the partition table */
    begin("Writing the partition table", 0, 0.02);
    if (P.delete_part) {
        /* the replaced partition's entry goes; plan_layout counted it free */
        int fd = dev_open(P.disk->name, O_RDWR);
        uint8_t s0[512];
        if (fd < 0 || dev_pread(fd, s0, 512, 0) < 0) { snprintf(err, cap, "reading the partition table"); if (fd >= 0) close(fd); return -1; }
        memset(s0 + 446 + 16 * (P.delete_part - 1), 0, 16);
        int r = dev_pwrite(fd, s0, 512, 0);
        close(fd);
        if (r < 0) { snprintf(err, cap, "writing the partition table"); return -1; }
    }
    uint32_t sig = P.disk->signature;
    if (P.erase || !sig) random_bytes(&sig, 4);
    if (write_table(&P, sig, err, cap) < 0) return -1;
    const char *esp = P.np[0].dev, *bootv = P.np[1].dev, *root = P.np[2].dev;

    /* 2. the root file system */
    begin("Making the file system", 0.02, 0.08);
    char rdev[48], ratio[16];
    snprintf(rdev, sizeof rdev, "/dev/%s", root);
    snprintf(ratio, sizeof ratio, "%u", P.np[2].sectors * SECTOR > 64ull * 1024 * MIB ? 65536u : 16384u);
    const char *mk[] = { "mke2fs", "-F", "-b", "4096", "-i", ratio, "-L", "kestrel-root", rdev, NULL };
    if (run(mk, cmd_err, sizeof cmd_err) != 0) { snprintf(err, cap, "mke2fs %s failed: %.200s", rdev, cmd_err); return -1; }
    mkdir("/mnt", 0755);
    mkdir(TARGET, 0755);
    if (mount(rdev, TARGET, "ext2", 0, NULL) < 0) { snprintf(err, cap, "mounting %s: %s", rdev, strerror(errno)); return -1; }

    /* 3. the files */
    begin("Copying files", 0.08, 0.85);
    int r = copy_tree(root_src, TARGET, skip_list(), copy_progress, NULL, err, cap);
    if (r == 0) {
        static const char *const dirs[] = { "boot", "dev", "etc", "home", "mnt", "proc", "root", "run", "sys", "tmp",
                                            "usr", "var", "var/log", "var/tmp", NULL };
        for (int i = 0; dirs[i]; i++) {
            char p[256];
            snprintf(p, sizeof p, TARGET "/%s", dirs[i]);
            mkdir(p, !strcmp(dirs[i], "tmp") || !strcmp(dirs[i], "var/tmp") ? 01777 : !strcmp(dirs[i], "root") ? 0700 : 0755);
            if (!strcmp(dirs[i], "tmp") || !strcmp(dirs[i], "var/tmp")) chmod(p, 01777);
        }
        begin("Setting up the system", 0.85, 0.88);
        r = setup_system(TARGET, &S, err, cap);
    }
    if (r == 0) {
        char t[512];
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        snprintf(t, sizeof t, "# Written by kestrel-install; kestrel-update reads it\nversion=%s\ninstalled=%04d-%02d-%02d\n"
                 "disk=%s\nmodel=%s\nlayout=%s\n", version, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, P.disk->name,
                 P.disk->model, P.erase ? "erase" : P.logical ? "alongside-logical" : "alongside");
        mkdir(TARGET "/etc/kestrel", 0755);
        write_file(TARGET "/etc/kestrel/installed", t, 0644);
        char *log = read_file(LOG, NULL);
        if (log) { write_file(TARGET "/var/log/kestrel-install.log", log, 0640); free(log); }
    }
    sync();
    if (umount(TARGET) < 0) ilog("umount %s: %s", TARGET, strerror(errno));
    if (r < 0) return -1;

    /* 4. the boot volume, the EFI partition, GRUB */
    uint8_t uuid[16];
    begin("Installing the boot volume", 0.88, 0.95);
    if (install_bootvol(bootv, uuid, err, cap) < 0) return -1;
    begin("Installing the boot loader", 0.95, 1.0);
    if (install_esp(esp, uuid, err, cap) < 0) return -1;
    if (install_grub(P.np[1].start, err, cap) < 0) return -1;
    sync();
    progress(1, "done");
    ilog("installation complete");
    return 0;
}

static int step_install(void)
{
    char text[2048];
    plan_text(text, sizeof text);
    size_t n = strlen(text);
    snprintf(text + n, sizeof text - n, "\n\nKeyboard: %s. Time zone: %s. Account: %s (%s)%s.",
             layouts[S.layout].name, tz_list[S.tz].name, S.user, S.fullname, S.autologin ? ", logging in automatically" : "");
    if (!ui_confirm("Write the changes to disks?", text, "Install now", "Go back", false)) return -1;
    char err[512] = "";
    if (do_install(err, sizeof err) < 0) {
        ilog("FAILED: %s", err);
        char t[1024];
        snprintf(t, sizeof t, "The installation failed:\n\n%s\n\nThe log is in " LOG ". The disk may now hold part of an "
                 "installation: run the installer again, or erase the disk.", err);
        ui_message("Installation failed", t);
        return -2;
    }
    struct menu_item it[] = {
        { "Restart now", "Remove the stick when the screen goes dark, then the computer starts Kestrel from the disk.", false },
        { "Continue testing", "Stay on the stick for now.", false },
    };
    char done[512];
    snprintf(done, sizeof done, "Kestrel %s is installed on %s. Restart the computer and remove the installation stick or disc to use it."
             "%s", version, P.disk->name, P.erase ? "" : "\n\nGRUB's menu lists Kestrel first, then the other systems it found.");
    int c = ui_menu("Installation complete", done, it, 2, 0);
    if (c == 0) {
        ui_end();
        ui_on = false;
        sync();
        const char *argv[] = { "reboot", NULL };
        execvp(argv[0], (char *const *)argv);
        reboot(RB_AUTOBOOT);
    }
    return -2;
}

/* ---- without questions ------------------------------------------------------ */
static int find_disk(const char *name) { for (int i = 0; i < ndisks; i++) if (!strcmp(disks[i].name, name)) return i; return -1; }

static int auto_run(const char *file)
{
    char *t = read_file(file, NULL);
    if (!t) { fprintf(stderr, "kestrel-install: cannot read %s\n", file); return 1; }
    char mode[16] = "erase", disk[16] = "", reboot_after[8] = "no";
    int region = -1, part = 0;
    unsigned long long root_mib = 0;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) {
        char *eq = strchr(l, '=');
        if (!eq || l[0] == '#') continue;
        *eq = 0;
        const char *k = l, *v = eq + 1;
        if (!strcmp(k, "mode")) snprintf(mode, sizeof mode, "%s", v);
        else if (!strcmp(k, "disk")) snprintf(disk, sizeof disk, "%s", v);
        else if (!strcmp(k, "region")) region = atoi(v);
        else if (!strcmp(k, "replace")) part = atoi(v);
        else if (!strcmp(k, "root_mib")) root_mib = strtoull(v, NULL, 10);
        else if (!strcmp(k, "fullname")) snprintf(S.fullname, sizeof S.fullname, "%s", v);
        else if (!strcmp(k, "hostname")) snprintf(S.hostname, sizeof S.hostname, "%s", v);
        else if (!strcmp(k, "user")) snprintf(S.user, sizeof S.user, "%s", v);
        else if (!strcmp(k, "password")) snprintf(S.password, sizeof S.password, "%s", v);
        else if (!strcmp(k, "autologin")) S.autologin = !strcmp(v, "yes");
        else if (!strcmp(k, "reboot")) snprintf(reboot_after, sizeof reboot_after, "%s", v);
        else if (!strcmp(k, "keyboard")) { for (int i = 0; i < layout_count; i++) if (!strcmp(layouts[i].kernel, v)) S.layout = i; }
        else if (!strcmp(k, "timezone")) { for (int i = 0; i < tz_count; i++) if (!strcmp(tz_list[i].name, v)) S.tz = i; }
    }
    free(t);
    int d = find_disk(disk);
    if (d < 0) { fprintf(stderr, "kestrel-install: no disk %s\n", disk); return 1; }
    if (!S.user[0] || !S.password[0]) { fprintf(stderr, "kestrel-install: user= and password= are needed\n"); return 1; }
    if (!S.hostname[0]) snprintf(S.hostname, sizeof S.hostname, "%s-kestrel", S.user);
    if (!S.fullname[0]) snprintf(S.fullname, sizeof S.fullname, "%s", S.user);
    memset(&P, 0, sizeof P);
    P.disk = &disks[d];
    P.erase = !strcmp(mode, "erase");
    P.root_sectors = root_mib * MIB / SECTOR;
    if (!P.erase) {
        if (part > 0) {
            struct part *pt = NULL;
            for (int i = 0; i < P.disk->nparts; i++) if (P.disk->part[i].no == part) pt = &P.disk->part[i];
            if (!pt || part > 4 || pt->fs == FS_EXTENDED) { fprintf(stderr, "kestrel-install: %s has no primary partition %d to replace\n", disk, part); return 1; }
            P.delete_part = part;
            P.where = replace_region(P.disk, pt);
        } else {
            if (region < 0) region = 0;
            if (region >= P.disk->nfree) { fprintf(stderr, "kestrel-install: %s has no free region %d\n", disk, region); return 1; }
            P.where = P.disk->free[region];
        }
    }
    if (!plan_layout(&P, boot_part_sectors())) { fprintf(stderr, "kestrel-install: %s\n", P.why_not); return 1; }
    char text[2048];
    plan_text(text, sizeof text);
    printf("%s\n\n", text);
    char err[512] = "";
    if (do_install(err, sizeof err) < 0) { fprintf(stderr, "kestrel-install: FAILED: %s\n", err); ilog("FAILED: %s", err); return 1; }
    printf("kestrel-install: Kestrel %s is installed on %s\n", version, P.disk->name);
    if (!strcmp(reboot_after, "yes")) { sync(); reboot(RB_AUTOBOOT); }
    return 0;
}

static void list_disks(void)
{
    char sz[24];
    size_str(root_need, sz, sizeof sz);
    printf("Kestrel %s: the root file system needs %s; the boot volume partition %llu MiB (from %s, the files from %s)\n\n",
           version, sz, (unsigned long long)(boot_part_sectors() * SECTOR / MIB), boot_src, root_src);
    for (int i = 0; i < ndisks; i++) {
        struct disk *d = &disks[i];
        char l[160];
        disk_line(d, l, sizeof l);
        printf("%s\n  table: %s%s%s\n", l, d->gpt ? "GPT" : d->mbr ? "MBR" : "none",
               d->in_use ? ", in use: " : "", d->in_use ? d->in_use_why : "");
        for (int k = 0; k < d->nparts; k++) {
            struct part *p = &d->part[k];
            size_str(p->sectors * SECTOR, sz, sizeof sz);
            printf("  %-10s %9s  start %10llu  type 0x%02x  %s%s%s%s\n", p->dev, sz, (unsigned long long)p->start, p->mbr_type,
                   fs_name(p->fs), p->label[0] ? " \"" : "", p->label, p->label[0] ? "\"" : "");
        }
        for (int f = 0; f < d->nfree; f++) {
            size_str(d->free[f].sectors * SECTOR, sz, sizeof sz);
            struct plan p = { .disk = d, .where = d->free[f] };
            bool ok = plan_layout(&p, boot_part_sectors());
            printf("  free %d: %s at %llu%s: %s\n", f, sz, (unsigned long long)d->free[f].start,
                   d->free[f].in_extended ? " (in the extended partition)" : "", ok ? (p.logical ? "fits (logical partitions)" : "fits") : p.why_not);
        }
        struct plan e = { .disk = d, .erase = true };
        printf("  erase: %s\n\n", plan_layout(&e, boot_part_sectors()) ? "possible" : e.why_not);
    }
}

int main(int argc, char **argv)
{
    if (getuid() != 0) { fprintf(stderr, "kestrel-install: run it as root\n"); return 1; }
    log_open(LOG);
    ilog("kestrel-install started");
    char err[512];
    if (sources(err, sizeof err) < 0) { fprintf(stderr, "kestrel-install: %s\n", err); return 1; }
    ndisks = disk_scan(disks, MAX_DISKS);
    sizes();
    if (argc > 1 && !strcmp(argv[1], "--list")) { list_disks(); return 0; }
    if (argc > 2 && !strcmp(argv[1], "--auto")) { auto_mode = true; return auto_run(argv[2]); }
    if (argc > 1) { fprintf(stderr, "usage: kestrel-install [--list | --auto FILE]\n"); return 2; }
    char *km = read_file("/proc/sys/kernel/keymap", NULL);
    if (km) { trim(km); for (int i = 0; i < layout_count; i++) if (!strcmp(layouts[i].kernel, km)) S.layout = i; free(km); }

    char title[96];
    snprintf(title, sizeof title, "Install Kestrel %s (beta)", version);
    ui_init(title);
    ui_on = true;
    int st = ST_WELCOME;
    while (st >= 0 && st < ST_DONE) {
        ui_steps(steps, 7, st);
        int r;
        switch (st) {
        case ST_WELCOME:  r = step_welcome(); break;
        case ST_KEYBOARD: r = step_keyboard(); break;
        case ST_TYPE:     r = step_type(); break;
        case ST_DISK:     r = step_disk(); break;
        case ST_TZ:       r = step_tz(); break;
        case ST_USER:     r = step_user(); break;
        default:          r = step_install(); break;
        }
        if (r == -2) break;
        st += r;
        if (st < 0) st = 0;
    }
    ui_end();
    return 0;
}
