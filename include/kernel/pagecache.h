/* include/kernel/pagecache.h -- file pages kept in memory, shared by everyone
 *
 * A disk file system gives each regular file a struct pcache: a sparse
 * array of page frames, one per 4 KiB of the file, each holding the file's
 * bytes as they are on disk (or newer: writes update the cached pages as
 * they go to the disk, so a cached page is never stale). read() copies
 * out of these pages, and private mappings of the file (programs and
 * libraries above all) map them directly, read-only: every process that
 * runs a program or loads a library shares the same frames, and only a
 * write to a private mapping makes a copy (copy-on-write, vm.c).
 *
 * The cache holds one reference to each of its pages (mm.h: page_ref);
 * mappings and readers hold their own. When free memory runs low the least
 * recently used files lose the pages nobody else holds: those are just
 * copies of the disk. write() keeps the disk up to date through the file
 * system's own path; a page written through a shared writable mapping is
 * marked dirty (pcache_set_dirty, from the dirty bits of the page tables:
 * vm.c) and stays until the file system has written it back
 * (pcache_take_dirty).
 *
 * All operations take one spinlock, never across I/O; the file system
 * serialises filling a file's pages under its own lock. */
#ifndef KESTREL_PAGECACHE_H
#define KESTREL_PAGECACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct pcache;

struct pcache *pcache_new(void);                    /* NULL on OOM */
void     pcache_free(struct pcache *pc);            /* drops every page */
/* The cached page `idx`, with a reference for the caller (page_unref when
 * done), or 0. `count`: a lookup that may lead to I/O (hit/miss statistics). */
uint64_t pcache_lookup(struct pcache *pc, uint64_t idx, bool count);
bool     pcache_present(struct pcache *pc, uint64_t idx);
/* Add the filled page `phys` (the caller's reference) as page `idx`.
 * Returns the page to use, still with the caller's reference: phys, now
 * cached too (unless the cache could not grow), or the page a racing fill
 * put there first, phys then being released. `ahead`: read ahead of need. */
uint64_t pcache_insert(struct pcache *pc, uint64_t idx, uint64_t phys, bool ahead);
/* Bytes written to the file: update the pages that are cached. */
void     pcache_write(struct pcache *pc, const void *buf, size_t len, uint64_t off);
/* The file is now `size` bytes: drop the pages past it, zero the tail. */
void     pcache_truncate(struct pcache *pc, uint64_t size);
/* Page `idx` was written through a shared mapping (no effect if it is not
 * cached). */
void     pcache_set_dirty(struct pcache *pc, uint64_t idx);
/* The first dirty page at or after *idx, made clean, with a reference for
 * the caller, its index in *idx; 0 if none. The caller writes it back. */
uint64_t pcache_take_dirty(struct pcache *pc, uint64_t *idx);
bool     pcache_dirty(struct pcache *pc);           /* any dirty page? */
/* Release up to `want` pages nobody maps or reads, least recently used
 * files first. Returns the number freed. */
uint64_t pcache_shrink(uint64_t want);

struct pcache_stats { uint64_t files, pages, hits, misses, ahead, evicted, dirty; };
void     pcache_stats(struct pcache_stats *s);

#endif
