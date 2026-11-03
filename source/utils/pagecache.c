#include "utils/pagecache.h"

#include <stdlib.h>
#include <string.h>

typedef struct NjPageCacheSlot {
    uint32_t cache_slot;
    uint32_t source_page;
    unsigned char valid;
    unsigned char age;
    uint8_t data[NJ_PAGECACHE_PAGE_SIZE];
} NjPageCacheSlot;

struct NjPageCache {
    NjPageCacheConfig config;
    NjPageCacheSlot slots[NJ_PAGECACHE_SLOTS];
};

NjPageCache *nj_pagecache_create(const NjPageCacheConfig *config) {
    if (!config || !config->source_size || !config->read ||
        !config->lock || !config->unlock) return NULL;
    NjPageCache *cache = calloc(1, sizeof(*cache));
    if (cache) cache->config = *config;
    return cache;
}

void nj_pagecache_destroy(NjPageCache *cache) {
    free(cache);
}

size_t nj_pagecache_memory_bytes(void) {
    return sizeof(NjPageCache);
}

static void touch(NjPageCache *cache, unsigned int index) {
    unsigned char previous_age = cache->slots[index].age;
    for (unsigned int i = 0; i < NJ_PAGECACHE_SLOTS; ++i) {
        if (i != index && cache->slots[i].valid &&
            cache->slots[i].age < previous_age) {
            ++cache->slots[i].age;
        }
    }
    cache->slots[index].age = 0;
}

int nj_pagecache_copy(NjPageCache *cache, uint32_t cache_slot,
                      uint32_t source_page, size_t page_offset, void *output,
                      size_t bytes, NjPageCacheStats *stats) {
    if (stats) memset(stats, 0, sizeof(*stats));
    if (!cache || !output || !bytes) return 0;
    uint64_t source_offset = (uint64_t)source_page * NJ_PAGECACHE_PAGE_SIZE;
    if (source_offset >= cache->config.source_size) return 0;
    uint64_t remaining = cache->config.source_size - source_offset;
    size_t page_bytes = remaining < NJ_PAGECACHE_PAGE_SIZE
        ? (size_t)remaining : NJ_PAGECACHE_PAGE_SIZE;
    if (page_offset >= page_bytes || bytes > page_bytes - page_offset) return 0;
    if (cache->config.lock(cache->config.user) != 0) return 0;

    unsigned int index = NJ_PAGECACHE_SLOTS;
    for (unsigned int i = 0; i < NJ_PAGECACHE_SLOTS; ++i) {
        NjPageCacheSlot *slot = &cache->slots[i];
        if (slot->valid && slot->cache_slot == cache_slot &&
            slot->source_page == source_page) {
            index = i;
            if (stats) stats->ram_hit_bytes = bytes;
            break;
        }
    }
    if (index == NJ_PAGECACHE_SLOTS) {
        index = 0;
        for (unsigned int i = 0; i < NJ_PAGECACHE_SLOTS; ++i) {
            if (!cache->slots[i].valid) {
                index = i;
                break;
            }
            if (cache->slots[i].age > cache->slots[index].age) index = i;
        }
        NjPageCacheSlot *slot = &cache->slots[index];
        /* A short/failed fill must not leave either the previous page or a
         * partially replaced page visible to a subsequent reader. */
        slot->valid = 0;
        int64_t got = cache->config.read(cache->config.user, slot->data,
            page_bytes, (uint64_t)cache_slot * NJ_PAGECACHE_PAGE_SIZE);
        if (stats) {
            stats->page_read_calls = 1;
            if (got > 0) stats->page_read_bytes = (uint64_t)got;
        }
        if (got != (int64_t)page_bytes) {
            cache->config.unlock(cache->config.user);
            return 0;
        }
        slot->cache_slot = cache_slot;
        slot->source_page = source_page;
        slot->valid = 1;
        slot->age = NJ_PAGECACHE_SLOTS;
        if (stats) stats->page_fills = 1;
    }
    memcpy(output, cache->slots[index].data + page_offset, bytes);
    touch(cache, index);
    cache->config.unlock(cache->config.user);
    return 1;
}
