/* Bounded, thread-safe page reader for private, immutable cache files. */
#ifndef NINJAGO_PAGECACHE_H
#define NINJAGO_PAGECACHE_H

#include <stddef.h>
#include <stdint.h>

#define NJ_PAGECACHE_PAGE_SIZE (64u * 1024u)
#define NJ_PAGECACHE_SLOTS 4u

typedef struct NjPageCache NjPageCache;

/* Positioned read: return bytes read, or a negative value on failure.
 * lock must return zero on success. The owner supplies one shared mutex for
 * this cache; read and the final copy run under that lock. No Vita ABI or
 * pthread type is embedded in the portable buffer protocol. */
typedef struct NjPageCacheConfig {
    uint64_t source_size;
    void *user;
    int64_t (*read)(void *user, void *output, size_t bytes, uint64_t offset);
    int (*lock)(void *user);
    void (*unlock)(void *user);
} NjPageCacheConfig;

typedef struct NjPageCacheStats {
    uint64_t ram_hit_bytes;
    uint64_t page_fills;
    uint64_t page_read_calls;
    uint64_t page_read_bytes;
} NjPageCacheStats;

/* Exactly four fixed-size pages; NULL on invalid configuration/allocation
 * failure. The caller may then retain its original direct-read path.
 * Creation/destruction require readers to be quiescent. */
NjPageCache *nj_pagecache_create(const NjPageCacheConfig *config);
void nj_pagecache_destroy(NjPageCache *cache);
size_t nj_pagecache_memory_bytes(void);

/* Read one in-page range, keyed by both cache slot and original source page.
 * Returns 1 only after the complete range is copied. Returns 0 without
 * changing output if arguments, locking, or a full-page read fail; the caller
 * must fall back to the original source. The final source page is never read
 * beyond source_size, even if the cache file contains padding. */
int nj_pagecache_copy(NjPageCache *cache, uint32_t cache_slot,
                      uint32_t source_page, size_t page_offset, void *output,
                      size_t bytes, NjPageCacheStats *stats);

#endif
