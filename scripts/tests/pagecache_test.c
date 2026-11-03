/* Compile the exact Vita-linked buffer protocol with host mutex callbacks. */
#include "utils/pagecache.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE_SIZE ((uint64_t)NJ_PAGECACHE_PAGE_SIZE * 5u + 17u)

typedef struct Fixture {
    pthread_mutex_t mutex;
    int locked;
    int fail_lock;
    int fail_reads;
    int short_reads;
    uint64_t read_calls;
    size_t last_read_bytes;
} Fixture;

/* Deliberately nonsequential first-touch cache-slot layout. */
static const uint32_t pages_by_slot[] = {3, 0, 5, 1, 4, 2};

static uint8_t expected_byte(uint64_t position) {
    return (uint8_t)(position * 131u +
                     (position / NJ_PAGECACHE_PAGE_SIZE) * 29u);
}

static int lock_fixture(void *user) {
    Fixture *f = user;
    if (f->fail_lock) return -1;
    int result = pthread_mutex_lock(&f->mutex);
    if (!result) {
        assert(!f->locked);
        f->locked = 1;
    }
    return result;
}

static void unlock_fixture(void *user) {
    Fixture *f = user;
    assert(f->locked);
    f->locked = 0;
    assert(!pthread_mutex_unlock(&f->mutex));
}

static int64_t read_fixture(void *user, void *output, size_t bytes,
                            uint64_t offset) {
    Fixture *f = user;
    assert(f->locked);
    assert(offset % NJ_PAGECACHE_PAGE_SIZE == 0);
    uint32_t slot = (uint32_t)(offset / NJ_PAGECACHE_PAGE_SIZE);
    assert(slot < sizeof(pages_by_slot) / sizeof(pages_by_slot[0]));
    uint64_t source_position =
        (uint64_t)pages_by_slot[slot] * NJ_PAGECACHE_PAGE_SIZE;
    assert(bytes <= SOURCE_SIZE - source_position);
    ++f->read_calls;
    f->last_read_bytes = bytes;
    if (f->fail_reads) {
        --f->fail_reads;
        return -1;
    }
    size_t actual = bytes;
    if (f->short_reads) {
        --f->short_reads;
        --actual;
    }
    for (size_t i = 0; i < actual; ++i) {
        ((uint8_t *)output)[i] = expected_byte(source_position + i);
    }
    return (int64_t)actual;
}

static uint32_t cache_slot(uint32_t page) {
    for (uint32_t i = 0;
         i < sizeof(pages_by_slot) / sizeof(pages_by_slot[0]); ++i) {
        if (pages_by_slot[i] == page) return i;
    }
    abort();
}

static void check_bytes(const uint8_t *bytes, uint64_t position, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        assert(bytes[i] == expected_byte(position + i));
    }
}

static NjPageCacheStats check_page(NjPageCache *cache, uint32_t page,
                                    size_t offset, size_t count) {
    uint8_t output[128];
    assert(count <= sizeof(output));
    NjPageCacheStats stats;
    assert(nj_pagecache_copy(cache, cache_slot(page), page, offset,
                             output, count, &stats));
    check_bytes(output, (uint64_t)page * NJ_PAGECACHE_PAGE_SIZE + offset,
                count);
    return stats;
}

static size_t read_range(NjPageCache *cache, uint64_t position, void *output,
                         size_t count) {
    if (position >= SOURCE_SIZE) return 0;
    uint64_t remaining = SOURCE_SIZE - position;
    if (count > remaining) count = (size_t)remaining;
    size_t done = 0;
    while (done < count) {
        uint64_t current = position + done;
        uint32_t page = (uint32_t)(current / NJ_PAGECACHE_PAGE_SIZE);
        size_t offset = (size_t)(current % NJ_PAGECACHE_PAGE_SIZE);
        size_t chunk = NJ_PAGECACHE_PAGE_SIZE - offset;
        if (chunk > count - done) chunk = count - done;
        assert(nj_pagecache_copy(cache, cache_slot(page), page, offset,
                                 (uint8_t *)output + done, chunk, NULL));
        done += chunk;
    }
    return done;
}

typedef struct Worker {
    NjPageCache *cache;
    pthread_barrier_t *barrier;
    unsigned int id;
    int mixed;
} Worker;

static void *worker(void *user) {
    Worker *w = user;
    int barrier = pthread_barrier_wait(w->barrier);
    assert(!barrier || barrier == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned int i = 0; i < 1000; ++i) {
        uint32_t page = w->mixed ? (i + w->id * 3u) % 6u : 3u;
        size_t offset = page == 5 ? i % 12u : (i * 37u) % 65000u;
        check_page(w->cache, page, offset, page == 5 ? 5 : 32);
    }
    return NULL;
}

static void concurrent_test(NjPageCache *cache, int mixed) {
    pthread_t threads[4];
    Worker workers[4];
    pthread_barrier_t barrier;
    assert(!pthread_barrier_init(&barrier, NULL, 5));
    for (unsigned int i = 0; i < 4; ++i) {
        workers[i] = (Worker){cache, &barrier, i, mixed};
        assert(!pthread_create(&threads[i], NULL, worker, &workers[i]));
    }
    int result = pthread_barrier_wait(&barrier);
    assert(!result || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned int i = 0; i < 4; ++i) {
        assert(!pthread_join(threads[i], NULL));
    }
    assert(!pthread_barrier_destroy(&barrier));
}

int main(void) {
    Fixture fixture = {0};
    assert(!pthread_mutex_init(&fixture.mutex, NULL));
    NjPageCacheConfig config = {
        .source_size = SOURCE_SIZE, .user = &fixture,
        .read = read_fixture, .lock = lock_fixture, .unlock = unlock_fixture,
    };
    assert(!nj_pagecache_create(NULL));
    NjPageCacheConfig invalid = config;
    invalid.lock = NULL;
    assert(!nj_pagecache_create(&invalid));
    invalid = config;
    invalid.source_size = 0;
    assert(!nj_pagecache_create(&invalid));
    assert(nj_pagecache_memory_bytes() >= NJ_PAGECACHE_SLOTS *
           NJ_PAGECACHE_PAGE_SIZE);
    assert(nj_pagecache_memory_bytes() < NJ_PAGECACHE_SLOTS *
           NJ_PAGECACHE_PAGE_SIZE + 1024u);
    NjPageCache *cache = nj_pagecache_create(&config);
    assert(cache);
    NjPageCacheStats stats = check_page(cache, 0, 11, 9);
    assert(stats.page_fills == 1 && stats.page_read_calls == 1 &&
           stats.page_read_bytes == NJ_PAGECACHE_PAGE_SIZE &&
           !stats.ram_hit_bytes);
    stats = check_page(cache, 0, 190, 100);
    assert(stats.ram_hit_bytes == 100 && !stats.page_read_calls);
    assert(fixture.read_calls == 1);

    check_page(cache, 1, 0, 32);
    check_page(cache, 2, 0, 32);
    check_page(cache, 3, 0, 32);
    for (unsigned int i = 0; i < 20; ++i) {
        assert(check_page(cache, 0, 0, 32).ram_hit_bytes == 32);
    }
    assert(check_page(cache, 4, 0, 32).page_fills == 1);
    assert(check_page(cache, 0, 0, 32).ram_hit_bytes == 32);
    assert(check_page(cache, 2, 0, 32).ram_hit_bytes == 32);
    assert(check_page(cache, 1, 0, 32).page_fills == 1);

    uint8_t sentinel[32];
    memset(sentinel, 0xa5, sizeof(sentinel));
    fixture.short_reads = 1;
    assert(!nj_pagecache_copy(cache, cache_slot(5), 5, 0, sentinel, 5, &stats));
    assert(stats.page_read_calls == 1 && stats.page_read_bytes == 16 &&
           !stats.page_fills);
    for (size_t i = 0; i < sizeof(sentinel); ++i) assert(sentinel[i] == 0xa5);
    stats = check_page(cache, 5, 3, 14);
    assert(stats.page_fills == 1 && stats.page_read_bytes == 17 &&
           fixture.last_read_bytes == 17);
    assert(!nj_pagecache_copy(cache, cache_slot(5), 5, 16, sentinel, 2, &stats));
    assert(!stats.page_read_calls);
    assert(!nj_pagecache_copy(cache, 0, 6, 0, sentinel, 1, &stats));
    assert(!nj_pagecache_copy(cache, 0, 0, NJ_PAGECACHE_PAGE_SIZE,
                              sentinel, 1, &stats));
    assert(!nj_pagecache_copy(cache, 0, 0, 0, sentinel, SIZE_MAX, &stats));

    size_t range_size = NJ_PAGECACHE_PAGE_SIZE + 44u;
    uint8_t *range = malloc(range_size);
    assert(range);
    uint64_t range_start = NJ_PAGECACHE_PAGE_SIZE - 13u;
    assert(read_range(cache, range_start, range, range_size) == range_size);
    check_bytes(range, range_start, range_size);
    memset(range, 0xa5, 32);
    assert(read_range(cache, SOURCE_SIZE - 11u, range, 32) == 11);
    check_bytes(range, SOURCE_SIZE - 11u, 11);
    for (size_t i = 11; i < 32; ++i) assert(range[i] == 0xa5);
    assert(!read_range(cache, SOURCE_SIZE, range, 1));
    free(range);

    nj_pagecache_destroy(cache);
    cache = nj_pagecache_create(&config);
    assert(cache);
    fixture.fail_reads = 1;
    assert(!nj_pagecache_copy(cache, cache_slot(0), 0, 0, sentinel, 8, &stats));
    assert(stats.page_read_calls == 1 && !stats.page_read_bytes);
    assert(check_page(cache, 0, 0, 8).page_fills == 1);
    fixture.fail_lock = 1;
    uint64_t calls_before = fixture.read_calls;
    assert(!nj_pagecache_copy(cache, cache_slot(1), 1, 0, sentinel, 8, &stats));
    assert(!stats.page_read_calls && fixture.read_calls == calls_before);
    fixture.fail_lock = 0;
    nj_pagecache_destroy(cache);

    cache = nj_pagecache_create(&config);
    assert(cache);
    calls_before = fixture.read_calls;
    concurrent_test(cache, 0);
    assert(fixture.read_calls == calls_before + 1);
    concurrent_test(cache, 1);
    nj_pagecache_destroy(cache);
    assert(!pthread_mutex_destroy(&fixture.mutex));
    puts("PASS: bounded four-page LRU, RAM hits, cache-slot mapping, short/error "
         "reads, locking failure, cross-page requests, source EOF, concurrent readers");
    return 0;
}
