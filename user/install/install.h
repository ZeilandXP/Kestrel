/* user/install/install.h -- Kestrel's installer and updater (shared parts)
 *
 *   ui.c      the full-screen text interface (terminal, keys, widgets)
 *   disk.c    disks, partition tables, file systems and other systems
 *   fat.c     the FAT32 EFI System Partition: format it, write its files
 *   copy.c    copying a directory tree with progress
 *   setup.c   the installed system's settings (users, time zone, keyboard)
 *   install.c kestrel-install: the steps, then the installation
 *   update.c  kestrel-update: the next release onto an installed system */
#ifndef KESTREL_INSTALL_H
#define KESTREL_INSTALL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define MIB             (1024ull * 1024)
#define SECTOR          512u
#define ALIGN_SECTORS   2048u                   /* 1 MiB */

/* ---- ui.c ---------------------------------------------------------------- */
enum { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_TAB, K_BTAB, K_BS, K_HOME, K_END,
       K_PGUP, K_PGDN, K_DEL, K_SPACE, K_CHAR };
struct key { int k; char ch[8]; };

struct menu_item {
    const char *label;          /* first line */
    const char *detail;         /* optional second line, dimmed */
    bool disabled;
};
struct field {
    const char *label;
    char *buf;
    size_t cap;
    bool secret;                /* password: shown as dots */
    const char *hint;           /* below the field, dimmed */
};

void ui_init(const char *title);
void ui_end(void);
void ui_steps(const char *const *names, int n, int current);   /* the step bar */
struct key ui_key(void);
/* A screen: heading, a paragraph (\n separates lines, word-wrapped). */
void ui_screen(const char *heading, const char *text);
/* Choose one item; -1 for Esc (back). `sel` is the initial choice. */
int  ui_menu(const char *heading, const char *text, const struct menu_item *items, int n, int sel);
/* Fill fields; returns 0 when done (Enter on the last field or Tab past it),
 * -1 for Esc. validate() returns an error message or NULL. */
int  ui_form(const char *heading, const char *text, struct field *f, int n,
             const char *(*validate)(struct field *f, int n));
/* Yes/no: true for the first button. */
bool ui_confirm(const char *heading, const char *text, const char *yes, const char *no, bool def_yes);
void ui_message(const char *heading, const char *text);       /* waits for Enter */
void ui_progress(const char *heading, const char *what, double fraction, const char *detail);
int  ui_cols(void);
int  ui_rows(void);

/* ---- disk.c -------------------------------------------------------------- */
enum fs_kind { FS_NONE, FS_UNKNOWN, FS_NTFS, FS_FAT, FS_EXFAT, FS_EXT2, FS_EXT3, FS_EXT4, FS_SWAP,
               FS_BTRFS, FS_XFS, FS_KESTREL, FS_EXTENDED };
struct part {
    int no;                     /* 1-4 primary, 5+ logical */
    uint64_t start, sectors;    /* in 512-byte sectors */
    uint8_t mbr_type;
    bool active;
    bool gpt;
    char dev[24];               /* "sata0p2" */
    enum fs_kind fs;
    char label[40];
    bool windows, linux_boot;   /* bootmgr / Windows directory; a Linux /boot */
};
struct region { uint64_t start, sectors; bool in_extended; };
#define MAX_PARTS 32
#define MAX_FREE  16
struct disk {
    char name[16];              /* "sata0" */
    char kind[8];               /* sata, nvme, usb, virtio */
    char model[48];
    uint64_t sectors;           /* 512-byte sectors */
    uint32_t block_size;
    bool gpt, mbr, blank;       /* partition table found; neither */
    uint32_t signature;         /* MBR disk signature */
    struct part part[MAX_PARTS];
    int nparts;
    int primaries_used;
    bool has_extended;
    struct region free[MAX_FREE];
    int nfree;
    bool in_use;                /* holds the running system or a mounted file system */
    char in_use_why[64];
    bool removable;             /* USB */
    char systems[128];          /* "Windows, Linux" found on it */
};
#define MAX_DISKS 16
int  disk_scan(struct disk *d, int max);        /* every disk Kestrel sees */
int  disk_read_table(struct disk *d);           /* partitions, free space, systems */
const char *fs_name(enum fs_kind k);
void size_str(uint64_t bytes, char *out, size_t cap);   /* "476.9 GB" */
int  dev_open(const char *name, int flags);     /* /dev/<name> */
int  dev_pread(int fd, void *buf, size_t len, uint64_t off);
int  dev_pwrite(int fd, const void *buf, size_t len, uint64_t off);
int  dev_rescan(const char *disk);              /* BLKRRPART, then wait for the nodes */

/* The new partition layout: a few partitions in one region of a disk. */
struct newpart { uint64_t start, sectors; uint8_t type; bool active; const char *role; int no; char dev[24]; };
struct plan {
    struct disk *disk;
    bool erase;                 /* a new partition table for the whole disk */
    struct region where;        /* the space Kestrel gets */
    int delete_part;            /* "something else": the partition replaced, 0 none */
    uint64_t root_sectors;      /* 0: all that is left */
    struct newpart np[4];       /* ESP, boot volume, root */
    int nnp;
    bool logical;               /* in a new extended partition */
    uint64_t ext_start, ext_sectors;
    char why_not[160];
};
#define ESP_SECTORS     (256u * 2048)           /* 256 MiB */
bool plan_layout(struct plan *p, uint64_t boot_sectors);
int  write_table(const struct plan *p, uint32_t signature, char *err, size_t errcap);

/* ---- fat.c --------------------------------------------------------------- */
struct fat_file { const char *path; const void *data; size_t size; };
/* A FAT32 file system on the partition (fd, from byte `off`, `sectors`
 * long) holding these files, each in one contiguous run. */
int fat_format(int fd, uint64_t off, uint64_t sectors, uint32_t serial, const struct fat_file *f, int n,
               char *err, size_t errcap);
/* Replace a file's contents within its run (the updater). */
int fat_rewrite(int fd, uint64_t off, const char *path, const void *data, size_t size, char *err, size_t errcap);
int fat_read(int fd, uint64_t off, const char *path, void *buf, size_t cap, size_t *size);

/* ---- copy.c -------------------------------------------------------------- */
struct copy_progress { uint64_t bytes, total, files; const char *current; };
typedef void (*copy_cb)(const struct copy_progress *p, void *ctx);
uint64_t tree_size(const char *src, const char *const *skip, uint64_t *files);
int copy_tree(const char *src, const char *dst, const char *const *skip, copy_cb cb, void *ctx,
              char *err, size_t errcap);

/* ---- setup.c ------------------------------------------------------------- */
struct tz { const char *name, *posix; };
extern const struct tz tz_list[];
extern const int tz_count;
struct layout { const char *kernel, *xkb, *name; };
extern const struct layout layouts[];
extern const int layout_count;
struct settings {
    char fullname[64], hostname[64], user[33], password[128];
    bool autologin;
    int tz, layout;
};
int setup_system(const char *root, const struct settings *s, char *err, size_t errcap);
int write_file(const char *path, const char *text, mode_t mode);
char *read_file(const char *path, size_t *len);

/* ---- helpers ------------------------------------------------------------- */
void ilog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_open(const char *path);
void random_bytes(void *buf, size_t n);
int  run(const char *const argv[], char *out, size_t cap);   /* exit status, or -1 */

#endif
