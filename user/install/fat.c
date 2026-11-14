/* user/install/fat.c -- the EFI System Partition's FAT32 file system
 *
 * fat_format() makes a new FAT32 volume ("KESTREL_EFI") on the partition and
 * writes its few files, each in one contiguous run: EFI/BOOT/BOOTX64.EFI for
 * UEFI, the kernel, and GRUB's boot/grub/local.cfg. fat_rewrite() replaces
 * one file's contents later (the updater: a new kernel, a new local.cfg),
 * in place when it keeps its number of clusters, else in newly allocated
 * clusters, with both copies of the FAT kept equal. fat_read() reads one.
 * Names are 8.3 (upper case on disk; GRUB and the firmware compare them
 * without case). */
#include "install.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define CLUSTER_SECTORS 2               /* 1 KiB clusters: >= 65525 of them from 64 MiB up */
#define RESERVED        32
#define EOC             0x0FFFFFFFu

struct vol {
    int fd;
    uint64_t off;                       /* byte offset of the partition on the disk */
    uint32_t spc, reserved, nfats, fat_sectors, root, clusters, cs;
    uint64_t data;                      /* byte offset of cluster 2, from the partition */
    uint32_t *fat;
};

static uint64_t cl_off(const struct vol *v, uint32_t c) { return v->off + v->data + (uint64_t)(c - 2) * v->cs; }

static void name83(const char *name, char out[11])
{
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name);
    for (size_t i = 0; i < base && i < 8; i++) out[i] = (char)toupper((unsigned char)name[i]);
    if (dot) for (size_t i = 0; dot[1 + i] && i < 3; i++) out[8 + i] = (char)toupper((unsigned char)dot[1 + i]);
}

static void dirent(uint8_t *e, const char n[11], uint8_t attr, uint32_t cl, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, n, 11);
    e[11] = attr;
    uint16_t t = 0x6000, d = (uint16_t)((2026 - 1980) << 9 | 11 << 5 | 14);    /* a fixed time stamp */
    memcpy(e + 14, &t, 2); memcpy(e + 16, &d, 2); memcpy(e + 18, &d, 2);
    memcpy(e + 22, &t, 2); memcpy(e + 24, &d, 2);
    uint16_t hi = (uint16_t)(cl >> 16), lo = (uint16_t)cl;
    memcpy(e + 20, &hi, 2);
    memcpy(e + 26, &lo, 2);
    memcpy(e + 28, &size, 4);
}

static int write_fats(struct vol *v)
{
    for (uint32_t i = 0; i < v->nfats; i++) {
        int r = dev_pwrite(v->fd, v->fat, (size_t)v->fat_sectors * SECTOR,
                           v->off + (uint64_t)(v->reserved + i * v->fat_sectors) * SECTOR);
        if (r < 0) return r;
    }
    return 0;
}

struct node { char path[64]; uint32_t cl; int parent; uint8_t ent[32 * 32]; int nent; };

int fat_format(int fd, uint64_t off, uint64_t sectors, uint32_t serial, const struct fat_file *f, int n,
               char *err, size_t errcap)
{
    struct vol v = { .fd = fd, .off = off, .spc = CLUSTER_SECTORS, .reserved = RESERVED, .nfats = 2 };
    v.cs = v.spc * SECTOR;
    if (sectors > 0xFFFFFFFFull) sectors = 0xFFFFFFFFull;
    uint32_t fs = 1;
    for (int it = 0; it < 16; it++) {
        uint64_t cl = (sectors - v.reserved - (uint64_t)v.nfats * fs) / v.spc;
        uint32_t need = (uint32_t)(((cl + 2) * 4 + SECTOR - 1) / SECTOR);
        if (need == fs) break;
        fs = need;
    }
    v.fat_sectors = fs;
    v.clusters = (uint32_t)((sectors - v.reserved - (uint64_t)v.nfats * fs) / v.spc);
    if (v.clusters < 65525) { snprintf(err, errcap, "the EFI partition is too small for FAT32"); return -1; }
    v.data = (uint64_t)(v.reserved + v.nfats * fs) * SECTOR;
    v.fat = calloc(v.fat_sectors, SECTOR);
    if (!v.fat) { snprintf(err, errcap, "out of memory"); return -1; }
    v.fat[0] = 0x0FFFFFF8;
    v.fat[1] = EOC;

    /* Directories: the root, then one cluster for each directory on a path. */
    struct node dirs[16];
    int nd = 1;
    memset(&dirs[0], 0, sizeof dirs[0]);
    dirs[0].cl = 2;
    v.fat[2] = EOC;
    uint32_t next = 3;
    for (int i = 0; i < n; i++) {
        char p[64];
        snprintf(p, sizeof p, "%s", f[i].path);
        int parent = 0;
        for (char *s = p, *slash; (slash = strchr(s, '/')); s = slash + 1) {
            *slash = 0;
            int found = -1;
            for (int k = 1; k < nd; k++) if (!strcmp(dirs[k].path, p)) found = k;
            if (found < 0 && nd < 16) {
                found = nd++;
                memset(&dirs[found], 0, sizeof dirs[found]);
                snprintf(dirs[found].path, sizeof dirs[found].path, "%s", p);
                dirs[found].cl = next;
                dirs[found].parent = parent;
                v.fat[next++] = EOC;
                char nm[11];
                name83(s, nm);
                dirent(dirs[parent].ent + 32 * dirs[parent].nent++, nm, 0x10, dirs[found].cl, 0);
                char dot[11], dotdot[11];
                memset(dot, ' ', 11); dot[0] = '.';
                memset(dotdot, ' ', 11); dotdot[0] = dotdot[1] = '.';
                dirent(dirs[found].ent, dot, 0x10, dirs[found].cl, 0);
                dirent(dirs[found].ent + 32, dotdot, 0x10, parent ? dirs[parent].cl : 0, 0);
                dirs[found].nent = 2;
            }
            *slash = '/';
            parent = found;
        }
    }
    /* Files, contiguous, after the directories. */
    uint32_t first[16];
    for (int i = 0; i < n && i < 16; i++) {
        const char *base = strrchr(f[i].path, '/');
        char dir[64];
        snprintf(dir, sizeof dir, "%.*s", base ? (int)(base - f[i].path) : 0, f[i].path);
        int parent = 0;
        for (int k = 1; k < nd; k++) if (!strcmp(dirs[k].path, dir)) parent = k;
        uint32_t ncl = (uint32_t)((f[i].size + v.cs - 1) / v.cs);
        first[i] = ncl ? next : 0;
        if (next + ncl > v.clusters + 2) { free(v.fat); snprintf(err, errcap, "the EFI partition is full"); return -1; }
        for (uint32_t c = 0; c < ncl; c++) v.fat[next + c] = c + 1 < ncl ? next + c + 1 : EOC;
        next += ncl;
        char nm[11];
        name83(base ? base + 1 : f[i].path, nm);
        if (dirs[parent].nent >= 32) { free(v.fat); snprintf(err, errcap, "too many files in one directory"); return -1; }
        dirent(dirs[parent].ent + 32 * dirs[parent].nent++, nm, 0x20, first[i], (uint32_t)f[i].size);
    }
    /* the volume label as the root's first entry */
    memmove(dirs[0].ent + 32, dirs[0].ent, (size_t)dirs[0].nent * 32);
    dirent(dirs[0].ent, "KESTREL_EFI", 0x08, 0, 0);
    dirs[0].nent++;

    uint8_t bs[512] = { 0xEB, 0x58, 0x90 };
    memcpy(bs + 3, "KESTREL1", 8);
    uint16_t w;
    uint32_t d;
    w = SECTOR; memcpy(bs + 11, &w, 2);
    bs[13] = (uint8_t)v.spc;
    w = (uint16_t)v.reserved; memcpy(bs + 14, &w, 2);
    bs[16] = (uint8_t)v.nfats;
    bs[21] = 0xF8;
    w = 63; memcpy(bs + 24, &w, 2);
    w = 255; memcpy(bs + 26, &w, 2);
    d = (uint32_t)(off / SECTOR); memcpy(bs + 28, &d, 4);
    d = (uint32_t)sectors; memcpy(bs + 32, &d, 4);
    memcpy(bs + 36, &v.fat_sectors, 4);
    d = 2; memcpy(bs + 44, &d, 4);
    w = 1; memcpy(bs + 48, &w, 2);
    w = 6; memcpy(bs + 50, &w, 2);
    bs[64] = 0x80;
    bs[66] = 0x29;
    memcpy(bs + 67, &serial, 4);
    memcpy(bs + 71, "KESTREL_EFI", 11);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[0x5A] = 0xF4; bs[0x5B] = 0xEB; bs[0x5C] = 0xFD;     /* hlt; jmp $-1: not bootable by BIOS */
    bs[510] = 0x55; bs[511] = 0xAA;
    uint8_t fsi[512] = { 0 };
    d = 0x41615252; memcpy(fsi, &d, 4);
    d = 0x61417272; memcpy(fsi + 484, &d, 4);
    d = v.clusters + 2 - next; memcpy(fsi + 488, &d, 4);
    memcpy(fsi + 492, &next, 4);
    d = 0xAA550000; memcpy(fsi + 508, &d, 4);

    static uint8_t zero[64 * 1024];
    int r = 0;
    for (uint64_t o = 0; r == 0 && o < v.data + (uint64_t)nd * v.cs; o += sizeof zero) {
        size_t chunk = v.data + (uint64_t)nd * v.cs - o < sizeof zero ? (size_t)(v.data + (uint64_t)nd * v.cs - o) : sizeof zero;
        r = dev_pwrite(fd, zero, chunk, off + o);
    }
    if (r == 0) r = dev_pwrite(fd, bs, 512, off);
    if (r == 0) r = dev_pwrite(fd, fsi, 512, off + SECTOR);
    if (r == 0) r = dev_pwrite(fd, bs, 512, off + 6 * SECTOR);
    if (r == 0) r = dev_pwrite(fd, fsi, 512, off + 7 * SECTOR);
    if (r == 0) r = write_fats(&v);
    for (int k = 0; r == 0 && k < nd; k++) {
        uint8_t *c = calloc(1, v.cs);
        if (!c) { r = -ENOMEM; break; }
        memcpy(c, dirs[k].ent, (size_t)dirs[k].nent * 32 > v.cs ? v.cs : (size_t)dirs[k].nent * 32);
        r = dev_pwrite(fd, c, v.cs, cl_off(&v, dirs[k].cl));
        free(c);
    }
    for (int i = 0; r == 0 && i < n; i++)
        if (f[i].size) r = dev_pwrite(fd, f[i].data, f[i].size, cl_off(&v, first[i]));
    free(v.fat);
    if (r < 0) { snprintf(err, errcap, "writing the EFI partition: %s", strerror(-r)); return -1; }
    ilog("EFI partition formatted: FAT32, %u clusters of %u bytes, %d files", v.clusters, v.cs, n);
    return 0;
}

/* ---- an existing volume ------------------------------------------------------ */
static int open_vol(int fd, uint64_t off, struct vol *v, char *err, size_t errcap)
{
    uint8_t bs[512];
    memset(v, 0, sizeof *v);
    v->fd = fd;
    v->off = off;
    if (dev_pread(fd, bs, 512, off) < 0 || bs[510] != 0x55 || bs[511] != 0xAA || memcmp(bs + 82, "FAT32", 5)) {
        snprintf(err, errcap, "the EFI partition has no FAT32 file system");
        return -1;
    }
    uint16_t bps, res;
    memcpy(&bps, bs + 11, 2);
    memcpy(&res, bs + 14, 2);
    uint32_t total;
    memcpy(&total, bs + 32, 4);
    memcpy(&v->fat_sectors, bs + 36, 4);
    memcpy(&v->root, bs + 44, 4);
    v->spc = bs[13];
    v->nfats = bs[16];
    v->reserved = res;
    if (bps != SECTOR || !v->spc || !v->nfats || !v->fat_sectors) { snprintf(err, errcap, "unsupported FAT32 geometry"); return -1; }
    v->cs = v->spc * SECTOR;
    v->data = (uint64_t)(v->reserved + v->nfats * v->fat_sectors) * SECTOR;
    v->clusters = (uint32_t)((total - v->reserved - (uint64_t)v->nfats * v->fat_sectors) / v->spc);
    v->fat = malloc((size_t)v->fat_sectors * SECTOR);
    if (!v->fat || dev_pread(fd, v->fat, (size_t)v->fat_sectors * SECTOR, off + (uint64_t)v->reserved * SECTOR) < 0) {
        free(v->fat);
        snprintf(err, errcap, "cannot read the FAT");
        return -1;
    }
    return 0;
}

/* The directory entry of `path`: the byte offset of the entry on the disk. */
static int lookup(struct vol *v, const char *path, uint8_t ent[32], uint64_t *ent_off)
{
    uint32_t dir = v->root;
    char p[96];
    snprintf(p, sizeof p, "%s", path);
    uint8_t *c = malloc(v->cs);
    if (!c) return -ENOMEM;
    for (char *s = p, *slash;; s = slash + 1) {
        slash = strchr(s, '/');
        if (slash) *slash = 0;
        char nm[11];
        name83(s, nm);
        bool found = false;
        for (uint32_t cl = dir, hops = 0; cl >= 2 && cl < 0x0FFFFFF8 && hops < 65536 && !found; cl = v->fat[cl] & 0x0FFFFFFF, hops++) {
            if (dev_pread(v->fd, c, v->cs, cl_off(v, cl)) < 0) { free(c); return -EIO; }
            for (uint32_t i = 0; i < v->cs; i += 32) {
                if (!c[i]) break;
                if (c[i] == 0xE5 || c[i + 11] == 0x0F || memcmp(c + i, nm, 11)) continue;
                memcpy(ent, c + i, 32);
                *ent_off = cl_off(v, cl) + i;
                found = true;
                break;
            }
        }
        if (!found) { free(c); return -ENOENT; }
        if (!slash) break;
        uint16_t hi, lo;
        memcpy(&hi, ent + 20, 2);
        memcpy(&lo, ent + 26, 2);
        dir = (uint32_t)hi << 16 | lo;
    }
    free(c);
    return 0;
}

int fat_read(int fd, uint64_t off, const char *path, void *buf, size_t cap, size_t *size)
{
    struct vol v;
    char err[96];
    if (open_vol(fd, off, &v, err, sizeof err) < 0) return -EINVAL;
    uint8_t e[32];
    uint64_t eo;
    int r = lookup(&v, path, e, &eo);
    if (r == 0) {
        uint32_t sz;
        uint16_t hi, lo;
        memcpy(&sz, e + 28, 4);
        memcpy(&hi, e + 20, 2);
        memcpy(&lo, e + 26, 2);
        uint32_t cl = (uint32_t)hi << 16 | lo;
        size_t done = 0, want = sz < cap ? sz : cap;
        while (r == 0 && done < want && cl >= 2 && cl < 0x0FFFFFF8) {
            size_t chunk = want - done < v.cs ? want - done : v.cs;
            r = dev_pread(fd, (char *)buf + done, chunk, cl_off(&v, cl));
            done += chunk;
            cl = v.fat[cl] & 0x0FFFFFFF;
        }
        if (size) *size = sz;
    }
    free(v.fat);
    return r;
}

int fat_rewrite(int fd, uint64_t off, const char *path, const void *data, size_t size, char *err, size_t errcap)
{
    struct vol v;
    if (open_vol(fd, off, &v, err, errcap) < 0) return -1;
    uint8_t e[32];
    uint64_t eo;
    int r = lookup(&v, path, e, &eo);
    if (r < 0) { free(v.fat); snprintf(err, errcap, "%s: not on the EFI partition", path); return -1; }
    uint16_t hi, lo;
    memcpy(&hi, e + 20, 2);
    memcpy(&lo, e + 26, 2);
    uint32_t cl = (uint32_t)hi << 16 | lo, old[1 << 16];
    uint32_t nold = 0;
    for (uint32_t c = cl; c >= 2 && c < 0x0FFFFFF8 && nold < (1u << 16); c = v.fat[c] & 0x0FFFFFFF) old[nold++] = c;
    uint32_t need = (uint32_t)((size + v.cs - 1) / v.cs);
    uint32_t *nc = malloc(sizeof *nc * (need ? need : 1));
    if (!nc) { free(v.fat); snprintf(err, errcap, "out of memory"); return -1; }
    if (need == nold) {
        memcpy(nc, old, sizeof *nc * need);         /* in place: the FAT stays as it is */
    } else {
        /* new clusters first, written, then the FAT and the entry point at them */
        uint32_t got = 0;
        for (uint32_t c = 2; c < v.clusters + 2 && got < need; c++) if (!(v.fat[c] & 0x0FFFFFFF)) nc[got++] = c;
        if (got < need) {
            /* not enough free: the old clusters can be reused after all */
            for (uint32_t i = 0; i < nold; i++) v.fat[old[i]] = 0;
            got = 0;
            for (uint32_t c = 2; c < v.clusters + 2 && got < need; c++) if (!(v.fat[c] & 0x0FFFFFFF)) nc[got++] = c;
            if (got < need) { free(nc); free(v.fat); snprintf(err, errcap, "the EFI partition is full"); return -1; }
        }
    }
    uint8_t *buf = calloc(1, v.cs);
    r = buf ? 0 : -ENOMEM;
    for (uint32_t i = 0; r == 0 && i < need; i++) {
        size_t chunk = size - (size_t)i * v.cs < v.cs ? size - (size_t)i * v.cs : v.cs;
        memset(buf, 0, v.cs);
        memcpy(buf, (const char *)data + (size_t)i * v.cs, chunk);
        r = dev_pwrite(fd, buf, v.cs, cl_off(&v, nc[i]));
    }
    free(buf);
    if (r == 0 && need != nold) {
        for (uint32_t i = 0; i < nold; i++) v.fat[old[i]] = 0;
        for (uint32_t i = 0; i < need; i++) v.fat[nc[i]] = i + 1 < need ? nc[i + 1] : EOC;
        r = write_fats(&v);
    }
    if (r == 0) {
        uint32_t first = need ? nc[0] : 0, sz = (uint32_t)size;
        hi = (uint16_t)(first >> 16);
        lo = (uint16_t)first;
        memcpy(e + 20, &hi, 2);
        memcpy(e + 26, &lo, 2);
        memcpy(e + 28, &sz, 4);
        r = dev_pwrite(fd, e, 32, eo);
    }
    free(nc);
    free(v.fat);
    if (r < 0) { snprintf(err, errcap, "writing %s on the EFI partition: %s", path, strerror(-r)); return -1; }
    return 0;
}
