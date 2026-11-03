#ifndef NINJAGO_HOTCACHE_H
#define NINJAGO_HOTCACHE_H
#include <stdint.h>
typedef struct __attribute__((packed)) ObbPageFileHeader {
    char magic[4];
    uint32_t version, page_size;
    uint64_t obb_size;
    uint8_t obb_sha256[32];
    uint32_t entry_count, reserved;
} ObbPageFileHeader;
typedef struct ObbHotcacheEntry { uint32_t page, cache_slot; } ObbHotcacheEntry;
_Static_assert(sizeof(ObbPageFileHeader)==60,"OBB header layout");
_Static_assert(sizeof(ObbHotcacheEntry)==8,"OBB index layout");
typedef struct HotcacheConfig {
    const char *source, *profile, *data, *index;
    uint64_t source_size;
    uint32_t page_size, maximum_pages;
    const uint8_t *source_sha256;
} HotcacheConfig;
/* Return nonzero from progress to cancel. stage 0 hashes, stage 1 copies. */
typedef int (*HotcacheProgress)(int stage, uint64_t done, uint64_t total, void *user);
/* 1=published, 0=no profile, -1=invalid/I/O failure, -2=cancelled. */
int hotcache_prepare(const HotcacheConfig *config, HotcacheProgress progress, void *user);
/* A present source stamp binds a Vita-built cache to size/mtime/ctime. */
int hotcache_source_matches(const HotcacheConfig *config);
#endif
