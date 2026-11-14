/* user/install/disk.c -- disks, partition tables, file systems, other systems
 *
 * The disks come from /proc/partitions; each disk's table is read from the
 * disk itself (/dev/<name>): MBR with its extended partitions, or GPT. The
 * first sectors of each partition say which file system it holds, and that
 * says which systems the disk has (Windows on NTFS, Linux on ext2/3/4, an
 * earlier Kestrel). New partitions go into one region: the whole disk
 * (a new MBR), or free space of an MBR disk, as primary partitions when
 * three table entries are free, else in a new extended partition. */
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define BLKRRPART _IO(0x12, 95)

int dev_open(const char *name, int flags)
{
    char p[64];
    snprintf(p, sizeof p, "/dev/%s", name);
    return open(p, flags | O_CLOEXEC);
}

int dev_pread(int fd, void *buf, size_t len, uint64_t off)
{
    size_t done = 0;
    while (done < len) {
        ssize_t r = pread(fd, (char *)buf + done, len - done, (off_t)(off + done));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return r < 0 ? -errno : -EIO;
        done += (size_t)r;
    }
    return 0;
}

int dev_pwrite(int fd, const void *buf, size_t len, uint64_t off)
{
    size_t done = 0;
    while (done < len) {
        ssize_t r = pwrite(fd, (const char *)buf + done, len - done, (off_t)(off + done));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return r < 0 ? -errno : -EIO;
        done += (size_t)r;
    }
    return 0;
}

int dev_rescan(const char *disk)
{
    int fd = dev_open(disk, O_RDONLY);
    if (fd < 0) return -errno;
    fsync(fd);
    int r = ioctl(fd, BLKRRPART, 0) < 0 ? -errno : 0;
    close(fd);
    return r;
}

void size_str(uint64_t bytes, char *out, size_t cap)
{
    double v = (double)bytes;
    const char *u = "bytes";
    if (bytes >= 1000ull * 1000 * 1000 * 1000) { v /= 1e12; u = "TB"; }
    else if (bytes >= 1000ull * 1000 * 1000) { v /= 1e9; u = "GB"; }
    else if (bytes >= 1000ull * 1000) { v /= 1e6; u = "MB"; }
    else if (bytes >= 1000) { v /= 1e3; u = "kB"; }
    if (v >= 100 || !strcmp(u, "bytes")) snprintf(out, cap, "%.0f %s", v, u);
    else snprintf(out, cap, "%.1f %s", v, u);
}

const char *fs_name(enum fs_kind k)
{
    static const char *const n[] = { "empty", "unknown", "NTFS", "FAT", "exFAT", "ext2", "ext3", "ext4", "swap",
                                     "btrfs", "XFS", "Kestrel boot volume", "extended" };
    return k <= FS_EXTENDED ? n[k] : "?";
}

/* "sata0p3" -> "sata0"; a disk name stays as it is. */
static void disk_of(const char *dev, char *out, size_t cap)
{
    snprintf(out, cap, "%s", dev);
    size_t n = strlen(out);
    size_t e = n;
    while (e && isdigit((unsigned char)out[e - 1])) e--;
    if (e < n && e >= 2 && out[e - 1] == 'p' && isdigit((unsigned char)out[e - 2])) out[e - 1] = 0;
}

/* Disks holding the running system or anything mounted. */
static void mark_in_use(struct disk *d, int n)
{
    char *m = read_file("/proc/mounts", NULL);
    for (char *line = m ? strtok(m, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        char src[128], mnt[256];
        if (sscanf(line, "%127s %255s", src, mnt) != 2 || strncmp(src, "/dev/", 5)) continue;
        char dk[32];
        disk_of(src + 5, dk, sizeof dk);
        for (int i = 0; i < n; i++)
            if (!strcmp(d[i].name, dk) && !d[i].in_use) {
                d[i].in_use = true;
                snprintf(d[i].in_use_why, sizeof d[i].in_use_why, "%s is mounted on %s", src, mnt);
            }
    }
    free(m);
    char *bv = read_file("/proc/bootvol", NULL);
    char *p = bv ? strstr(bv, "device      /dev/") : NULL;
    if (p) {
        char dev[32], dk[32];
        if (sscanf(p + 17, "%31[^,]", dev) == 1) {
            disk_of(dev, dk, sizeof dk);
            for (int i = 0; i < n; i++)
                if (!strcmp(d[i].name, dk)) {
                    d[i].in_use = true;
                    snprintf(d[i].in_use_why, sizeof d[i].in_use_why, "Kestrel is running from it");
                }
        }
    }
    free(bv);
}

int disk_scan(struct disk *d, int max)
{
    char *t = read_file("/proc/partitions", NULL);
    if (!t) return 0;
    int n = 0;
    for (char *line = strtok(t, "\n"); line && n < max; line = strtok(NULL, "\n")) {
        char name[32], k1[16], k2[16], state[16];
        unsigned long long blocks, mib;
        unsigned bsize;
        int used = 0;
        if (sscanf(line, "%31s %llu %u %llu %15s %15s %15s %n", name, &blocks, &bsize, &mib, k1, k2, state, &used) < 7)
            continue;
        if (strcmp(k2, "disk") || !strcmp(k1, "ram") || (strcmp(state, "ok") && strcmp(state, "errors"))) continue;
        struct disk *x = &d[n];
        memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", name);
        snprintf(x->kind, sizeof x->kind, "%s", k1);
        snprintf(x->model, sizeof x->model, "%s", used ? line + used : "");
        size_t ml = strlen(x->model);
        while (ml && x->model[ml - 1] == ' ') x->model[--ml] = 0;
        x->block_size = bsize;
        x->sectors = (uint64_t)blocks * bsize / SECTOR;
        x->removable = !strcmp(k1, "usb");
        n++;
    }
    free(t);
    mark_in_use(d, n);
    for (int i = 0; i < n; i++) disk_read_table(&d[i]);
    return n;
}

/* ---- file systems ---------------------------------------------------------- */
static void probe_fs(int fd, struct part *p)
{
    static uint8_t b[72 * 1024];
    p->fs = FS_NONE;
    size_t want = p->sectors * SECTOR < sizeof b ? (size_t)(p->sectors * SECTOR) : sizeof b;
    if (dev_pread(fd, b, want, p->start * SECTOR) < 0) { p->fs = FS_UNKNOWN; return; }
    if (want < 4096) return;
    if (!memcmp(b, "KESTREL_BOOT", 12)) { p->fs = FS_KESTREL; snprintf(p->label, sizeof p->label, "%.32s", (char *)b + 36); return; }
    if (!memcmp(b + 3, "NTFS    ", 8)) { p->fs = FS_NTFS; return; }
    if (!memcmp(b + 3, "EXFAT   ", 8)) { p->fs = FS_EXFAT; return; }
    if (b[510] == 0x55 && b[511] == 0xAA && (!memcmp(b + 54, "FAT", 3) || !memcmp(b + 82, "FAT32", 5))) {
        p->fs = FS_FAT;
        const char *l = (const char *)(memcmp(b + 82, "FAT32", 5) ? b + 43 : b + 71);
        snprintf(p->label, sizeof p->label, "%.11s", l);
        for (int i = (int)strlen(p->label) - 1; i >= 0 && p->label[i] == ' '; i--) p->label[i] = 0;
        if (!strcmp(p->label, "NO NAME")) p->label[0] = 0;
        return;
    }
    if (want > 2048 && b[1080] == 0x53 && b[1081] == 0xEF) {
        uint32_t compat = b[1116] | b[1117] << 8 | b[1118] << 16 | (uint32_t)b[1119] << 24;
        uint32_t incompat = b[1120] | b[1121] << 8 | b[1122] << 16 | (uint32_t)b[1123] << 24;
        p->fs = (incompat & 0x2C0) ? FS_EXT4 : (compat & 4) ? FS_EXT3 : FS_EXT2;   /* extents/64bit/flex; journal */
        snprintf(p->label, sizeof p->label, "%.16s", (char *)b + 1144);
        return;
    }
    if (!memcmp(b + 4086, "SWAPSPACE2", 10) || !memcmp(b + 4086, "SWAP-SPACE", 10)) { p->fs = FS_SWAP; return; }
    if (want > 0x10048 && !memcmp(b + 0x10040, "_BHRfS_M", 8)) { p->fs = FS_BTRFS; return; }
    if (!memcmp(b, "XFSB", 4)) { p->fs = FS_XFS; return; }
    for (size_t i = 0; i < 4096; i++) if (b[i]) { p->fs = FS_UNKNOWN; return; }
}

/* ---- tables ---------------------------------------------------------------- */
struct mbr_ent { uint8_t status, chs1[3], type, chs2[3]; uint32_t lba, count; } __attribute__((packed));

static bool is_ext(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }

static void add_part(struct disk *d, int no, uint64_t start, uint64_t count, uint8_t type, bool active)
{
    if (d->nparts >= MAX_PARTS || !count || start >= d->sectors) return;
    struct part *p = &d->part[d->nparts++];
    memset(p, 0, sizeof *p);
    p->no = no;
    p->start = start;
    p->sectors = count;
    p->mbr_type = type;
    p->active = active;
    snprintf(p->dev, sizeof p->dev, "%sp%d", d->name, no);
}

static int cmp_part(const void *a, const void *b)
{
    const struct part *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

/* Free space between [lo, hi) not covered by the ranges, at 1 MiB borders. */
static void find_free(struct disk *d, uint64_t lo, uint64_t hi, bool in_ext, bool logicals_only)
{
    uint64_t pos = lo;
    for (int i = 0; i <= d->nparts; i++) {
        uint64_t s = hi, e = hi;
        if (i < d->nparts) {
            const struct part *p = &d->part[i];
            if (logicals_only ? p->no < 5 : (p->no >= 5)) continue;
            s = p->start;
            e = p->start + p->sectors;
            if (logicals_only) s = s > ALIGN_SECTORS ? s - ALIGN_SECTORS : s;   /* its EBR */
            if (e <= lo || s >= hi) continue;
        }
        uint64_t a = (pos + ALIGN_SECTORS - 1) / ALIGN_SECTORS * ALIGN_SECTORS;
        uint64_t b = s / ALIGN_SECTORS * ALIGN_SECTORS;
        if (b > a && b - a >= 16 * ALIGN_SECTORS && d->nfree < MAX_FREE)        /* 16 MiB at least */
            d->free[d->nfree++] = (struct region){ a, b - a, in_ext };
        if (e > pos) pos = e;
    }
}

int disk_read_table(struct disk *d)
{
    d->nparts = d->nfree = d->primaries_used = 0;
    d->gpt = d->mbr = d->blank = d->has_extended = false;
    d->systems[0] = 0;
    int fd = dev_open(d->name, O_RDONLY);
    if (fd < 0) return -errno;
    uint8_t s0[512], s1[512];
    if (dev_pread(fd, s0, 512, 0) < 0) { close(fd); return -EIO; }
    uint64_t end = d->sectors;
    if (s0[510] != 0x55 || s0[511] != 0xAA) {
        d->blank = true;
    } else {
        const struct mbr_ent *e = (const struct mbr_ent *)(s0 + 446);
        bool prot = false;
        for (int i = 0; i < 4; i++) if (e[i].type == 0xEE) prot = true;
        memcpy(&d->signature, s0 + 440, 4);
        if (prot && dev_pread(fd, s1, 512, 512) == 0 && !memcmp(s1, "EFI PART", 8)) {
            d->gpt = true;
            uint64_t ent_lba, first, last;
            uint32_t count, size;
            memcpy(&first, s1 + 40, 8);
            memcpy(&last, s1 + 48, 8);
            memcpy(&ent_lba, s1 + 72, 8);
            memcpy(&count, s1 + 80, 4);
            memcpy(&size, s1 + 84, 4);
            if (size < 128 || size > 1024 || count > 256) count = 0;
            uint8_t *arr = count ? malloc((size_t)count * size) : NULL;
            if (arr && dev_pread(fd, arr, (size_t)count * size, ent_lba * SECTOR) == 0)
                for (uint32_t i = 0; i < count; i++) {
                    const uint8_t *g = arr + (size_t)i * size;
                    uint64_t a, b;
                    memcpy(&a, g + 32, 8);
                    memcpy(&b, g + 40, 8);
                    bool used = false;
                    for (int k = 0; k < 16; k++) if (g[k]) used = true;
                    if (!used || b < a) continue;
                    add_part(d, (int)i + 1, a, b - a + 1, 0, false);
                    struct part *p = &d->part[d->nparts - 1];
                    p->gpt = true;
                    static const uint8_t esp[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                                     0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
                    if (!memcmp(g, esp, 16)) p->mbr_type = 0xEF;
                }
            free(arr);
            qsort(d->part, (size_t)d->nparts, sizeof *d->part, cmp_part);
            find_free(d, first, last + 1, false, false);
        } else {
            d->mbr = true;
            uint64_t ext_start = 0, ext_count = 0;
            for (int i = 0; i < 4; i++) {
                if (!e[i].type || !e[i].count) continue;
                d->primaries_used++;
                if (is_ext(e[i].type)) {
                    d->has_extended = true;
                    ext_start = e[i].lba;
                    ext_count = e[i].count;
                    add_part(d, i + 1, e[i].lba, e[i].count, e[i].type, false);
                    d->part[d->nparts - 1].fs = FS_EXTENDED;
                    uint64_t ebr = ext_start;
                    for (int n = 0; n < 64; n++) {
                        uint8_t eb[512];
                        if (ebr >= end || dev_pread(fd, eb, 512, ebr * SECTOR) < 0 || eb[510] != 0x55 || eb[511] != 0xAA) break;
                        const struct mbr_ent *l = (const struct mbr_ent *)(eb + 446);
                        if (l[0].type && l[0].count) add_part(d, 5 + n, ebr + l[0].lba, l[0].count, l[0].type, l[0].status == 0x80);
                        if (!is_ext(l[1].type) || !l[1].lba) break;
                        uint64_t next = ext_start + l[1].lba;
                        if (next <= ebr) break;
                        ebr = next;
                    }
                    continue;
                }
                add_part(d, i + 1, e[i].lba, e[i].count, e[i].type, e[i].status == 0x80);
            }
            qsort(d->part, (size_t)d->nparts, sizeof *d->part, cmp_part);
            uint64_t top = end > 0xFFFFFFFFull ? 0xFFFFFFFFull : end;
            find_free(d, 1, top, false, false);
            if (d->has_extended) find_free(d, ext_start, ext_start + ext_count, true, true);
        }
    }
    bool win = false, lin = false, kes = false;
    for (int i = 0; i < d->nparts; i++) {
        struct part *p = &d->part[i];
        if (p->fs == FS_EXTENDED) continue;
        probe_fs(fd, p);
        if (p->fs == FS_NTFS) p->windows = win = true;
        char tmp[16];
        size_t sz;
        if (p->fs == FS_FAT && fat_read(fd, p->start * SECTOR, "bootmgr", tmp, sizeof tmp, &sz) == 0) p->windows = win = true;
        if ((p->fs == FS_EXT2 || p->fs == FS_EXT3 || p->fs == FS_EXT4 || p->fs == FS_BTRFS || p->fs == FS_XFS) &&
            strcmp(p->label, "kestrel-root")) p->linux_boot = lin = true;
        if (p->fs == FS_KESTREL) kes = true;
        if (p->fs == FS_FAT && !strcmp(p->label, "VTOYEFI") && !d->in_use) {   /* the ISO may have come from it */
            d->in_use = true;
            snprintf(d->in_use_why, sizeof d->in_use_why, "it is a Ventoy stick");
        }
    }
    if (d->blank) {
        /* no table at all: a blank disk, or a file system on the whole disk */
        struct part whole = { .start = 0, .sectors = d->sectors };
        probe_fs(fd, &whole);
        if (whole.fs != FS_NONE) d->blank = false;
    }
    close(fd);
    snprintf(d->systems, sizeof d->systems, "%s%s%s%s%s", win ? "Windows" : "", win && (lin || kes) ? ", " : "",
             lin ? "Linux" : "", lin && kes ? ", " : "", kes ? "Kestrel" : "");
    return 0;
}

/* ---- the new layout ---------------------------------------------------------- */
static uint64_t align_up(uint64_t x) { return (x + ALIGN_SECTORS - 1) / ALIGN_SECTORS * ALIGN_SECTORS; }

bool plan_layout(struct plan *p, uint64_t boot_sectors)
{
    struct disk *d = p->disk;
    p->nnp = 0;
    p->logical = false;
    p->why_not[0] = 0;
    if (d->in_use) { snprintf(p->why_not, sizeof p->why_not, "This disk is in use: %s.", d->in_use_why); return false; }
    if (d->block_size != 512) { snprintf(p->why_not, sizeof p->why_not, "Kestrel installs only on disks with 512-byte sectors (this one has %u).", d->block_size); return false; }
    uint64_t lo, hi;
    if (p->erase) {
        lo = ALIGN_SECTORS;
        hi = d->sectors > 0xFFFFFFFFull ? 0xFFFFFFFFull : d->sectors;
        hi = hi / ALIGN_SECTORS * ALIGN_SECTORS;
    } else {
        if (d->gpt) {
            snprintf(p->why_not, sizeof p->why_not, "This disk has a GPT partition table (a system that starts with UEFI). "
                     "Installing next to it needs UEFI boot entries, which Kestrel cannot write yet.");
            return false;
        }
        if (p->where.in_extended) {
            snprintf(p->why_not, sizeof p->why_not, "This free space is inside an extended partition; Kestrel can only use free space outside one for now.");
            return false;
        }
        bool gap = true;
        for (int i = 0; i < d->nparts; i++) if (d->part[i].start < ALIGN_SECTORS) gap = false;
        if (!gap) { snprintf(p->why_not, sizeof p->why_not, "The first partition starts before 1 MiB: there is no room for the boot loader."); return false; }
        lo = p->where.start;
        hi = p->where.start + p->where.sectors;
    }
    /* a partition being replaced gives back its table entry */
    int free_slots = p->erase ? 4 : 4 - d->primaries_used + (p->delete_part >= 1 && p->delete_part <= 4);
    uint64_t pos = lo;
    if (!p->erase && free_slots < 3) {
        if (d->has_extended || free_slots < 1) {
            snprintf(p->why_not, sizeof p->why_not, "The partition table has no room: Kestrel needs three entries, or one for a new extended partition.");
            return false;
        }
        p->logical = true;
        p->ext_start = lo;
        p->ext_sectors = hi - lo;
        pos = lo + ALIGN_SECTORS;                       /* the first EBR's 1 MiB */
    }
    uint64_t gaps = p->logical ? 2 * ALIGN_SECTORS : 0;          /* the next two EBRs */
    uint64_t need_min = ESP_SECTORS + boot_sectors + gaps;
    if (pos + need_min + 64 * ALIGN_SECTORS > hi) {
        char s[24];
        size_str((need_min + 64 * ALIGN_SECTORS) * SECTOR, s, sizeof s);
        snprintf(p->why_not, sizeof p->why_not, "Not enough space: Kestrel needs %s at least here.", s);
        return false;
    }
    int no = 1, used_no[5] = { 0 };
    if (!p->erase)
        for (int i = 0; i < d->nparts; i++)
            if (d->part[i].no >= 1 && d->part[i].no <= 4 && d->part[i].no != p->delete_part) used_no[d->part[i].no] = 1;
    const char *roles[3] = { "EFI System Partition (the kernel and the boot loader)", "Kestrel boot volume (two slots, for updates)", "Kestrel root file system (ext2)" };
    uint8_t types[3] = { 0xEF, 0x4B, 0x83 };
    uint64_t sizes[3] = { ESP_SECTORS, boot_sectors, 0 };
    for (int i = 0; i < 3; i++) {
        struct newpart *n = &p->np[p->nnp++];
        n->start = pos;
        if (i < 2) n->sectors = sizes[i];
        else {
            uint64_t left = hi - pos;
            n->sectors = p->root_sectors && p->root_sectors < left ? align_up(p->root_sectors) : left;
            if (n->sectors > left) n->sectors = left;
        }
        n->type = types[i];
        n->role = roles[i];
        n->active = p->erase && i == 0;
        if (p->logical) n->no = 5 + i;
        else {
            while (no <= 4 && used_no[no]) no++;
            if (no > 4) { snprintf(p->why_not, sizeof p->why_not, "The partition table has no free entry."); return false; }
            n->no = no++;
        }
        snprintf(n->dev, sizeof n->dev, "%sp%d", d->name, n->no);
        pos = n->start + n->sectors;
        if (p->logical) pos += ALIGN_SECTORS;
    }
    if (p->logical) {
        p->ext_sectors = p->np[2].start + p->np[2].sectors - p->ext_start;
    }
    return true;
}

static void put_ent(uint8_t *at, uint8_t type, uint64_t start, uint64_t count, bool active)
{
    struct mbr_ent e = { active ? 0x80 : 0, { 0xFE, 0xFF, 0xFF }, type, { 0xFE, 0xFF, 0xFF },
                         (uint32_t)start, (uint32_t)count };
    memcpy(at, &e, sizeof e);
}

int write_table(const struct plan *p, uint32_t signature, char *err, size_t errcap)
{
    struct disk *d = p->disk;
    int fd = dev_open(d->name, O_RDWR);
    if (fd < 0) { snprintf(err, errcap, "cannot open /dev/%s for writing: %s", d->name, strerror(errno)); return -1; }
    uint8_t s0[512];
    static uint8_t zero[64 * 1024];
    int r = 0;
    if (p->erase) {
        memset(s0, 0, sizeof s0);
        memcpy(s0 + 440, &signature, 4);
        /* old GPT headers and boot code: the start and the last 33 sectors */
        if ((r = dev_pwrite(fd, zero, sizeof zero, 0)) == 0 && d->sectors > 128)
            r = dev_pwrite(fd, zero, 33 * SECTOR, (d->sectors - 33) * SECTOR);
        for (uint64_t o = sizeof zero; r == 0 && o < ALIGN_SECTORS * SECTOR; o += sizeof zero)
            r = dev_pwrite(fd, zero, sizeof zero, o);
    } else {
        r = dev_pread(fd, s0, sizeof s0, 0);
    }
    if (r < 0) { snprintf(err, errcap, "/dev/%s: %s", d->name, strerror(-r)); close(fd); return -1; }
    s0[510] = 0x55;
    s0[511] = 0xAA;
    if (!p->logical) {
        for (int i = 0; i < p->nnp; i++)
            put_ent(s0 + 446 + 16 * (p->np[i].no - 1), p->np[i].type, p->np[i].start, p->np[i].sectors, p->np[i].active);
    } else {
        int slot = -1;
        for (int i = 0; i < 4 && slot < 0; i++) if (!s0[446 + 16 * i + 4]) slot = i;
        if (slot < 0) { snprintf(err, errcap, "no free entry for the extended partition"); close(fd); return -1; }
        put_ent(s0 + 446 + 16 * slot, 0x0F, p->ext_start, p->ext_sectors, false);
        for (int i = 0; i < p->nnp; i++) {
            uint8_t eb[512] = { 0 };
            uint64_t ebr = p->np[i].start - ALIGN_SECTORS;
            put_ent(eb + 446, p->np[i].type, p->np[i].start - ebr, p->np[i].sectors, false);
            if (i + 1 < p->nnp) {
                uint64_t next = p->np[i + 1].start - ALIGN_SECTORS;
                put_ent(eb + 462, 0x05, next - p->ext_start, p->np[i + 1].start + p->np[i + 1].sectors - next, false);
            }
            eb[510] = 0x55;
            eb[511] = 0xAA;
            if ((r = dev_pwrite(fd, eb, 512, ebr * SECTOR)) < 0) break;
        }
    }
    if (r == 0) r = dev_pwrite(fd, s0, 512, 0);
    if (r == 0 && fsync(fd) < 0) r = -errno;
    close(fd);
    if (r < 0) { snprintf(err, errcap, "writing the partition table of /dev/%s: %s", d->name, strerror(-r)); return -1; }
    ilog("partition table of %s written (%s)", d->name, p->logical ? "extended partition" : p->erase ? "new MBR" : "primary entries");
    if ((r = dev_rescan(d->name)) < 0) {
        snprintf(err, errcap, "the kernel could not read the new partition table of /dev/%s: %s", d->name, strerror(-r));
        return -1;
    }
    for (int i = 0; i < p->nnp; i++) {
        struct stat st;
        char path[64];
        snprintf(path, sizeof path, "/dev/%s", p->np[i].dev);
        if (stat(path, &st) < 0) { snprintf(err, errcap, "%s did not appear after the new partition table", path); return -1; }
    }
    return 0;
}
