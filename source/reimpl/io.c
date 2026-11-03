/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See licenses/upstream-MIT.txt for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <dirent.h>
#include <stdarg.h>
#include <limits.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/hotcache.h"
#include "utils/pagecache.h"
#include "utils/glutil.h"
#include <psp2/ctrl.h>
#include <psp2/message_dialog.h>

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

#define TRACKED_OBB_STREAMS 32
#define OBB_PAGE_SIZE (64u * 1024u)
#define EXPECTED_OBB_SIZE UINT64_C(973671465)
#define OBB_PAGE_COUNT \
    ((EXPECTED_OBB_SIZE + OBB_PAGE_SIZE - 1u) / OBB_PAGE_SIZE)
#define OBB_PROFILE_PATH DATA_PATH "cache/obb-pages-r21.bin"
#define OBB_PROFILE_TEMP_PATH DATA_PATH "cache/obb-pages-r21.bin.tmp"
#define OBB_HOTCACHE_INDEX_PATH DATA_PATH "cache/obb-hotcache.idx"
#define OBB_HOTCACHE_DATA_PATH DATA_PATH "cache/obb-hotcache.bin"

static const uint8_t expected_obb_sha256[32] = {
    0xa4, 0x2a, 0x74, 0xb4, 0xa7, 0xa9, 0xc2, 0x2c,
    0x97, 0x0c, 0xd5, 0x04, 0xce, 0x64, 0x63, 0xa0,
    0x70, 0x40, 0x1d, 0x09, 0x26, 0x05, 0xde, 0xb2,
    0x0f, 0xbf, 0x66, 0xf8, 0x9b, 0x6d, 0xd1, 0xd4,
};

typedef struct TrackedObbStream {
    FILE *stream;
    int64_t position;
    int position_known;
    int64_t physical_position;
    int physical_position_known;
} TrackedObbStream;

static TrackedObbStream tracked_obb_streams[TRACKED_OBB_STREAMS];
static uint64_t obb_last_activity_us;
static uint64_t obb_redundant_seek_skips;
static ObbIoStats obb_io_stats;
static int obb_tracking_capacity_reported;
static uint32_t obb_profile_bitmap[(OBB_PAGE_COUNT + 31u) / 32u];
static uint32_t obb_profile_order[OBB_PAGE_COUNT];
static uint32_t obb_profile_count;
static int obb_profile_active;
static int obb_hotcache_state;
static SceUID obb_hotcache_fd = -1;
static ObbHotcacheEntry *obb_hotcache_entries;
static uint32_t obb_hotcache_entry_count;
static NjPageCache *obb_hotcache_pages;

static int64_t obb_hotcache_positioned_read(void *user, void *output,
                                           size_t bytes, uint64_t offset) {
    (void)user;
    uint64_t started = sceKernelGetProcessTimeWide();
    int got = sceIoPread(obb_hotcache_fd, output, bytes, (SceOff)offset);
    __atomic_fetch_add(&obb_io_stats.hotcache_read_us,
        sceKernelGetProcessTimeWide() - started, __ATOMIC_RELAXED);
    return got;
}


static size_t raw_fread(void *ptr, size_t size, size_t count, FILE *stream) {
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fread(ptr, size, count, stream);
#else
    return fread(ptr, size, count, stream);
#endif
}

static int raw_fseek(FILE *stream, long offset, int whence) {
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_fseek(stream, offset, whence);
#else
    return fseek(stream, offset, whence);
#endif
}

static long raw_ftell(FILE *stream) {
#ifdef USE_SCELIBC_IO
    return sceLibcBridge_ftell(stream);
#else
    return ftell(stream);
#endif
}

static void raw_clearerr(FILE *stream) {
#ifdef USE_SCELIBC_IO
    sceLibcBridge_clearerr(stream);
#else
    clearerr(stream);
#endif
}

static void obb_profile_record(uint64_t position, uint64_t bytes) {
    if (!__atomic_load_n(&obb_profile_active, __ATOMIC_ACQUIRE) ||
        bytes == 0 || position >= EXPECTED_OBB_SIZE) {
        return;
    }
    uint64_t end = position + bytes - 1;
    if (end >= EXPECTED_OBB_SIZE) {
        end = EXPECTED_OBB_SIZE - 1;
    }
    uint32_t first = (uint32_t)(position / OBB_PAGE_SIZE);
    uint32_t last = (uint32_t)(end / OBB_PAGE_SIZE);
    for (uint32_t page = first; page <= last; ++page) {
        uint32_t mask = UINT32_C(1) << (page & 31u);
        uint32_t old = __atomic_fetch_or(&obb_profile_bitmap[page >> 5],
                                         mask, __ATOMIC_RELAXED);
        if (!(old & mask)) {
            uint32_t slot = __atomic_fetch_add(&obb_profile_count, 1,
                                                __ATOMIC_RELAXED);
            if (slot < OBB_PAGE_COUNT) {
                obb_profile_order[slot] = page;
            }
        }
    }
}

static int obb_hotcache_find(uint32_t page, uint32_t *cache_slot) {
    uint32_t low = 0;
    uint32_t high = obb_hotcache_entry_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        uint32_t candidate = obb_hotcache_entries[middle].page;
        if (candidate < page) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low >= obb_hotcache_entry_count ||
        obb_hotcache_entries[low].page != page) {
        return 0;
    }
    *cache_slot = obb_hotcache_entries[low].cache_slot;
    return 1;
}

static int obb_hotcache_try_init(void) {
    int state = __atomic_load_n(&obb_hotcache_state, __ATOMIC_ACQUIRE);
    if (state != 0) {
        return state == 2;
    }
    int expected = 0;
    if (!__atomic_compare_exchange_n(&obb_hotcache_state, &expected, 1,
                                     false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        return 0;
    }

    HotcacheConfig source_config = {
        .source = OBB_PATH, .data = OBB_HOTCACHE_DATA_PATH,
        .source_size = EXPECTED_OBB_SIZE, .source_sha256 = expected_obb_sha256,
    };
    if (!hotcache_source_matches(&source_config)) {
        l_warn("Vita-built cache source changed; using original OBB until "
               "cache preparation re-verifies its SHA-256.");
        __atomic_store_n(&obb_hotcache_state, -1, __ATOMIC_RELEASE);
        return 0;
    }
    FILE *index = fopen(OBB_HOTCACHE_INDEX_PATH, "rb");
    if (!index) {
        l_info("Optional 64 KiB OBB hot cache is absent; using the complete "
               "original OBB path.");
        __atomic_store_n(&obb_hotcache_state, -1, __ATOMIC_RELEASE);
        return 0;
    }

    ObbPageFileHeader header;
    bool valid = fread(&header, sizeof(header), 1, index) == 1 &&
        memcmp(header.magic, "NHC1", 4) == 0 && header.version == 1 &&
        header.page_size == OBB_PAGE_SIZE &&
        header.obb_size == EXPECTED_OBB_SIZE &&
        memcmp(header.obb_sha256, expected_obb_sha256,
               sizeof(expected_obb_sha256)) == 0 &&
        header.reserved == 0 &&
        header.entry_count > 0 && header.entry_count <= OBB_PAGE_COUNT;
    ObbHotcacheEntry *entries = NULL;
    if (valid) {
        entries = malloc((size_t)header.entry_count * sizeof(*entries));
        valid = entries && fread(entries, sizeof(*entries),
                                 header.entry_count, index) ==
                             header.entry_count;
    }
    if (valid && (fgetc(index) != EOF || ferror(index))) valid = false;
    fclose(index);

    if (valid) {
        uint8_t slots[(OBB_PAGE_COUNT + 7) / 8] = {0};
        for (uint32_t i = 0; i < header.entry_count; ++i) {
            if (entries[i].page >= OBB_PAGE_COUNT ||
                entries[i].cache_slot >= header.entry_count ||
                (i > 0 && entries[i - 1].page >= entries[i].page) ||
                (slots[entries[i].cache_slot / 8] &
                 (1u << (entries[i].cache_slot % 8)))) {
                valid = false;
                break;
            }
            slots[entries[i].cache_slot / 8] |=
                (1u << (entries[i].cache_slot % 8));
        }
    }

    SceUID data_fd = valid
        ? sceIoOpen(OBB_HOTCACHE_DATA_PATH, SCE_O_RDONLY, 0) : -1;
    SceIoStat data_stat;
    memset(&data_stat, 0, sizeof(data_stat));
    if (data_fd >= 0 &&
        (sceIoGetstatByFd(data_fd, &data_stat) < 0 ||
         data_stat.st_size !=
             (SceOff)((uint64_t)header.entry_count * OBB_PAGE_SIZE))) {
        sceIoClose(data_fd);
        data_fd = -1;
        valid = false;
    }
    if (!valid || data_fd < 0) {
        free(entries);
        l_warn("Optional OBB hot cache failed identity/index validation; "
               "using the original OBB only.");
        __atomic_store_n(&obb_hotcache_state, -1, __ATOMIC_RELEASE);
        return 0;
    }

    obb_hotcache_entries = entries;
    obb_hotcache_entry_count = header.entry_count;
    obb_hotcache_fd = data_fd;
    /* Page buffering read 200 MB to deliver 38.6 MB in the device run.
     * Retain the disk hot cache, but reject whole-page RAM overreads. */
    l_info("RAM page reader disabled after measured overreads; "
           "retaining direct positioned hot-cache reads.");
    __atomic_store_n(&obb_hotcache_state, 2, __ATOMIC_RELEASE);
    l_success("Validated optional OBB hot cache: %u profiled 64 KiB pages "
              "for the pinned version-1057 OBB.", header.entry_count);
    return 1;
}

typedef struct CacheProgressState {
    uint64_t last_draw;
    int dialog_open, stage;
} CacheProgressState;

static int cache_progress(int stage, uint64_t done, uint64_t total, void *user) {
    CacheProgressState *state = user;
    SceCtrlData pad = {0};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_CIRCLE) return 1;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (state->last_draw && now - state->last_draw < 100000 && done != total)
        return 0;
    state->last_draw = now;
    if (state->dialog_open) {
        if (stage != state->stage) {
            state->stage = stage;
            sceMsgDialogProgressBarSetMsg(0, stage == 0
                ? "Verifying owned game data. Circle: skip cache preparation."
                : "Preparing arena cache on Vita. Circle: cancel safely.");
        }
        sceMsgDialogProgressBarSetValue(0, (uint32_t)(done * 100 / total));
    }
    sceKernelPowerTick(0);
    glClear(GL_COLOR_BUFFER_BIT);
    vglSwapBuffers(GL_TRUE);
    return 0;
}

void io_prepare_hotcache(void) {
    /* Before native game startup: no tracked stream or cache reader exists. */
    if (file_exists(DATA_PATH "cache/disable-hotcache")) {
        __atomic_store_n(&obb_hotcache_state, -1, __ATOMIC_RELEASE);
        l_info("OBB hot cache disabled by cache/disable-hotcache.");
        return;
    }
    if (obb_hotcache_try_init()) {
        l_info("Existing arena cache is ready; L+R does not rebuild a valid "
               "cache and no preparation dialog is needed.");
        return;
    }
    /* Optional preparation is integrated, but never interrupts an ordinary
     * launch. A skipped/invalid cache must not cause a recurring dialog. */
    SceCtrlData request = {0};
    sceCtrlPeekBufferPositive(0, &request, 1);
    const uint32_t prepare_buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    if ((request.buttons & prepare_buttons) != prepare_buttons) {
        l_info("Optional cache preparation not requested; normal launch "
               "continues. Hold L+R at launch to prepare a missing cache.");
        return;
    }
    if (!file_exists(OBB_PROFILE_PATH)) {
        l_info("Vita cache preparation deferred: complete an arena load, "
               "then hold L+R at the next launch. No PC script is required.");
        return;
    }
    if (!file_mkpath(OBB_HOTCACHE_DATA_PATH, 0777)) return;
    HotcacheConfig config = {
        .source = OBB_PATH, .profile = OBB_PROFILE_PATH,
        .data = OBB_HOTCACHE_DATA_PATH, .index = OBB_HOTCACHE_INDEX_PATH,
        .source_size = EXPECTED_OBB_SIZE, .page_size = OBB_PAGE_SIZE,
        .maximum_pages = 2048, .source_sha256 = expected_obb_sha256,
    };
    l_info("Preparing private OBB cache on Vita (maximum 128 MiB); "
           "full source SHA-256 verification runs once before copying.");
    gl_init();
    glClearColor(0.04f, 0.04f, 0.06f, 1.0f);
    SceMsgDialogProgressBarParam bar = {0};
    bar.msg = "Verifying owned game data. Circle: skip cache preparation.";
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_PROGRESS_BAR;
    param.progBarParam = &bar;
    CacheProgressState state = {.stage = 0};
    state.dialog_open = sceMsgDialogInit(&param) >= 0;
    if (!state.dialog_open) {
        l_warn("Cache progress dialog is unavailable; skipping preparation "
               "and starting the game with original OBB data.");
        return;
    }
    uint64_t start = sceKernelGetProcessTimeWide();
    int result = hotcache_prepare(&config, cache_progress, &state);
    if (state.dialog_open) {
        sceMsgDialogClose();
        for (unsigned i = 0; i < 120 &&
             sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED; ++i) {
            glClear(GL_COLOR_BUFFER_BIT);
            vglSwapBuffers(GL_TRUE);
            sceKernelDelayThread(16667);
        }
        sceMsgDialogTerm();
    }
    l_info("Vita cache preparation result=%d duration=%llu ms "
           "(1=ready, -2=cancelled; other results use original OBB).", result,
           (unsigned long long)((sceKernelGetProcessTimeWide()-start)/1000));
    __atomic_store_n(&obb_hotcache_state, 0, __ATOMIC_RELEASE);
    obb_hotcache_try_init();
}

static int tracked_obb_stream_index(FILE *stream) {
    for (unsigned int i = 0; i < TRACKED_OBB_STREAMS; ++i) {
        if (__atomic_load_n(&tracked_obb_streams[i].stream,
                            __ATOMIC_RELAXED) == stream) {
            return (int)i;
        }
    }
    return -1;
}

static void track_obb_stream(FILE *stream) {
    for (unsigned int i = 0; i < TRACKED_OBB_STREAMS; ++i) {
        FILE *expected = NULL;
        if (__atomic_compare_exchange_n(&tracked_obb_streams[i].stream,
                                        &expected, stream, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            __atomic_store_n(&tracked_obb_streams[i].position, 0,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&tracked_obb_streams[i].position_known, 1,
                             __ATOMIC_RELAXED);
            __atomic_store_n(&tracked_obb_streams[i].physical_position, 0,
                             __ATOMIC_RELAXED);
            __atomic_store_n(
                &tracked_obb_streams[i].physical_position_known, 1,
                __ATOMIC_RELAXED);
            return;
        }
    }
    if (__atomic_exchange_n(&obb_tracking_capacity_reported, 1,
                            __ATOMIC_RELAXED) == 0) {
        l_warn("OBB stream tracking capacity exhausted; redundant-seek "
               "elision will be incomplete.");
    }
}

static void untrack_obb_stream(FILE *stream) {
    for (unsigned int i = 0; i < TRACKED_OBB_STREAMS; ++i) {
        FILE *expected = stream;
        if (__atomic_compare_exchange_n(&tracked_obb_streams[i].stream,
                                        &expected, NULL, false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            __atomic_store_n(&tracked_obb_streams[i].position_known, 0,
                             __ATOMIC_RELAXED);
            __atomic_store_n(
                &tracked_obb_streams[i].physical_position_known, 0,
                __ATOMIC_RELAXED);
            return;
        }
    }
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    static int save_write_reported;
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    /*
     * Fusion normally relies on its Android activity to create versioned
     * shader-cache directories. There is no Java filesystem layer on Vita, so
     * prepare those parents immediately before the first cache write.
     */
    if (filename && mode &&
        (strchr(mode, 'w') || strchr(mode, 'a')) &&
        str_starts_with(filename, DATA_PATH "cache/") &&
        !file_mkpath(filename, 0777)) {
        l_error("Could not prepare cache path for %s.", filename);
    }

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    if (ret && filename && mode && strcmp(filename, OBB_PATH) == 0 &&
        strchr(mode, 'r') && !strchr(mode, 'w') && !strchr(mode, 'a')) {
        /*
         * Fusion performs small, seek-heavy reads from several concurrent OBB
         * streams. A 512 KiB stdio buffer regressed Chen's Island from 67.2
         * to 178.0 seconds by repeatedly fetching data that the next seek
         * discarded. Keep libc's default buffering and track only each
         * stream's logical position so redundant same-position seeks can be
         * eliminated safely.
         */
        track_obb_stream(ret);
        obb_hotcache_try_init();
        __atomic_store_n(&obb_last_activity_us,
                         sceKernelGetProcessTimeWide(), __ATOMIC_RELAXED);
    }

    if (ret && !save_write_reported && filename && mode &&
        (strchr(mode, 'w') || strchr(mode, 'a')) &&
        strstr(filename, "savegame.dat")) {
        save_write_reported = 1;
        l_success("Fusion autosave opened its persistent file: %s.",
                  filename);
    }

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

size_t fread_soloader(void *ptr, size_t size, size_t count, FILE *stream) {
    int tracked_index = tracked_obb_stream_index(stream);
    bool tracked = tracked_index >= 0;
    if (!tracked || size == 0 || count == 0) {
        return raw_fread(ptr, size, count, stream);
    }

    uint64_t read_start = sceKernelGetProcessTimeWide();
    bool logical_known = __atomic_load_n(
        &tracked_obb_streams[tracked_index].position_known,
        __ATOMIC_RELAXED);
    uint64_t logical_position = logical_known ? (uint64_t)__atomic_load_n(
        &tracked_obb_streams[tracked_index].position,
        __ATOMIC_RELAXED) : 0;
    bool hotcache = logical_known &&
        __atomic_load_n(&obb_hotcache_state, __ATOMIC_ACQUIRE) == 2 &&
        size <= SIZE_MAX / count;
    size_t requested = hotcache ? size * count : 0;
    if (hotcache) {
        /* Cache slots pad their last page, but fread must expose only the
         * original OBB's bytes, not those private padding bytes. */
        uint64_t remaining = logical_position < EXPECTED_OBB_SIZE
            ? EXPECTED_OBB_SIZE - logical_position : 0;
        if (remaining < requested) requested = (size_t)remaining;
    }
    size_t completed = 0;

    if (hotcache) {
        uint8_t *output = ptr;
        while (completed < requested) {
            uint64_t position = logical_position + completed;
            uint32_t page = (uint32_t)(position / OBB_PAGE_SIZE);
            size_t page_offset = (size_t)(position % OBB_PAGE_SIZE);
            size_t chunk = OBB_PAGE_SIZE - page_offset;
            if (chunk > requested - completed) {
                chunk = requested - completed;
            }

            uint32_t cache_slot;
            if (page < OBB_PAGE_COUNT &&
                obb_hotcache_find(page, &cache_slot)) {
                int64_t got;
                if (obb_hotcache_pages) {
                    NjPageCacheStats page_stats;
                    got = nj_pagecache_copy(obb_hotcache_pages, cache_slot,
                        page, page_offset, output + completed, chunk,
                        &page_stats) ? (int64_t)chunk : -1;
                    __atomic_fetch_add(&obb_io_stats.hotcache_ram_hit_bytes,
                        page_stats.ram_hit_bytes, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&obb_io_stats.hotcache_page_fills,
                        page_stats.page_fills, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&obb_io_stats.hotcache_page_read_calls,
                        page_stats.page_read_calls, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&obb_io_stats.hotcache_page_read_bytes,
                        page_stats.page_read_bytes, __ATOMIC_RELAXED);
                } else {
                    got = obb_hotcache_positioned_read(NULL,
                        output + completed, chunk,
                        (uint64_t)cache_slot * OBB_PAGE_SIZE + page_offset);
                }
                if (got == (int)chunk) {
                    __atomic_fetch_add(&obb_io_stats.hotcache_hit_bytes,
                                       chunk, __ATOMIC_RELAXED);
                    completed += chunk;
                    continue;
                }
                l_warn("OBB hot-cache page %u returned %d/%u bytes; "
                       "falling back to the source OBB.", page, (int)got,
                       (unsigned int)chunk);
            }

            uint64_t fallback_start = sceKernelGetProcessTimeWide();
            bool physical_known = __atomic_load_n(
                &tracked_obb_streams[tracked_index].physical_position_known,
                __ATOMIC_RELAXED);
            int64_t physical_position = physical_known
                ? __atomic_load_n(
                      &tracked_obb_streams[tracked_index].physical_position,
                      __ATOMIC_RELAXED)
                : -1;
            if (!physical_known || physical_position != (int64_t)position) {
                if (raw_fseek(stream, (long)position, SEEK_SET) != 0) {
                    __atomic_store_n(
                        &tracked_obb_streams[tracked_index]
                             .physical_position_known,
                        0, __ATOMIC_RELAXED);
                    __atomic_fetch_add(
                        &obb_io_stats.fallback_read_us,
                        sceKernelGetProcessTimeWide() - fallback_start,
                        __ATOMIC_RELAXED);
                    break;
                }
                __atomic_store_n(
                    &tracked_obb_streams[tracked_index].physical_position,
                    (int64_t)position, __ATOMIC_RELAXED);
                __atomic_store_n(
                    &tracked_obb_streams[tracked_index]
                         .physical_position_known,
                    1, __ATOMIC_RELAXED);
            }
            size_t got = raw_fread(output + completed, 1, chunk, stream);
            uint64_t fallback_elapsed =
                sceKernelGetProcessTimeWide() - fallback_start;
            __atomic_fetch_add(&obb_io_stats.fallback_read_us,
                               fallback_elapsed, __ATOMIC_RELAXED);
            __atomic_fetch_add(&obb_io_stats.hotcache_miss_bytes, got,
                               __ATOMIC_RELAXED);
            completed += got;
            __atomic_store_n(
                &tracked_obb_streams[tracked_index].physical_position,
                (int64_t)(position + got), __ATOMIC_RELAXED);
            if (got != chunk) {
                break;
            }
        }
    } else {
        uint64_t fallback_start = sceKernelGetProcessTimeWide();
        size_t result = raw_fread(ptr, size, count, stream);
        completed = result * size;
        __atomic_fetch_add(&obb_io_stats.fallback_read_us,
                           sceKernelGetProcessTimeWide() - fallback_start,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&obb_io_stats.hotcache_miss_bytes, completed,
                           __ATOMIC_RELAXED);
        if (logical_known) {
            __atomic_store_n(
                &tracked_obb_streams[tracked_index].physical_position,
                (int64_t)(logical_position + completed),
                __ATOMIC_RELAXED);
            __atomic_store_n(
                &tracked_obb_streams[tracked_index].physical_position_known,
                1, __ATOMIC_RELAXED);
        } else {
            __atomic_store_n(
                &tracked_obb_streams[tracked_index].physical_position_known,
                0, __ATOMIC_RELAXED);
        }
    }

    uint64_t read_end = sceKernelGetProcessTimeWide();
    __atomic_fetch_add(&obb_io_stats.read_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&obb_io_stats.read_bytes, completed,
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(&obb_io_stats.read_us, read_end - read_start,
                       __ATOMIC_RELAXED);
    if (logical_known) {
        __atomic_store_n(&tracked_obb_streams[tracked_index].position,
                         (int64_t)(logical_position + completed),
                         __ATOMIC_RELAXED);
        obb_profile_record(logical_position, completed);
    } else {
        __atomic_store_n(&tracked_obb_streams[tracked_index].position_known,
                         0, __ATOMIC_RELAXED);
    }
    if (completed != 0) {
        __atomic_store_n(&obb_last_activity_us, read_end, __ATOMIC_RELAXED);
    }
    return completed / size;
}

int fseek_soloader(FILE *stream, long offset, int whence) {
    int tracked_index = tracked_obb_stream_index(stream);
    bool tracked = tracked_index >= 0;
    int old_known = tracked && __atomic_load_n(
        &tracked_obb_streams[tracked_index].position_known,
        __ATOMIC_RELAXED);
    int64_t old_position = old_known ? __atomic_load_n(
        &tracked_obb_streams[tracked_index].position,
        __ATOMIC_RELAXED) : 0;
    bool new_known = tracked &&
        (whence == SEEK_SET || (whence == SEEK_CUR && old_known));
    int64_t new_position = whence == SEEK_SET
        ? (int64_t)offset : old_position + (int64_t)offset;
    if (new_known && new_position == old_position) {
        /*
         * The latest physical-Vita trace found 4,094 no-op seeks in one
         * 7,033-seek arena load. The OBB is read-only and position tracking
         * follows successful reads/seeks, so retain libc's useful buffered
         * data and preserve fseek's status-reset behavior without entering
         * the filesystem for the same logical position.
         */
        raw_clearerr(stream);
        __atomic_fetch_add(&obb_redundant_seek_skips, 1,
                           __ATOMIC_RELAXED);
        return 0;
    }
    uint64_t seek_start = tracked ? sceKernelGetProcessTimeWide() : 0;
    bool hotcache = tracked && new_known && new_position >= 0 &&
        __atomic_load_n(&obb_hotcache_state, __ATOMIC_ACQUIRE) == 2;
    int result;
    if (hotcache) {
        raw_clearerr(stream);
        result = 0;
    } else {
        result = raw_fseek(stream, offset, whence);
    }
    if (tracked) {
        uint64_t seek_end = sceKernelGetProcessTimeWide();
        __atomic_fetch_add(&obb_io_stats.seek_calls, 1,
                           __ATOMIC_RELAXED);
        __atomic_fetch_add(&obb_io_stats.seek_us, seek_end - seek_start,
                           __ATOMIC_RELAXED);
        if (result == 0 && new_known) {
            __atomic_store_n(&tracked_obb_streams[tracked_index].position,
                             new_position, __ATOMIC_RELAXED);
            __atomic_store_n(
                &tracked_obb_streams[tracked_index].position_known, 1,
                __ATOMIC_RELAXED);
            if (!hotcache) {
                __atomic_store_n(
                    &tracked_obb_streams[tracked_index].physical_position,
                    new_position, __ATOMIC_RELAXED);
                __atomic_store_n(
                    &tracked_obb_streams[tracked_index]
                         .physical_position_known,
                    1, __ATOMIC_RELAXED);
            }
        } else {
            __atomic_store_n(
                &tracked_obb_streams[tracked_index].position_known, 0,
                __ATOMIC_RELAXED);
            __atomic_store_n(
                &tracked_obb_streams[tracked_index].physical_position_known,
                0, __ATOMIC_RELAXED);
        }
    }
    return result;
}

long ftell_soloader(FILE *stream) {
    int tracked_index = tracked_obb_stream_index(stream);
    if (tracked_index >= 0 && __atomic_load_n(
            &tracked_obb_streams[tracked_index].position_known,
            __ATOMIC_RELAXED)) {
        int64_t position = __atomic_load_n(
            &tracked_obb_streams[tracked_index].position,
            __ATOMIC_RELAXED);
        if (position >= 0 && position <= LONG_MAX) {
            return (long)position;
        }
    }
    return raw_ftell(stream);
}

uint64_t io_get_last_obb_activity_us(void) {
    return __atomic_load_n(&obb_last_activity_us, __ATOMIC_RELAXED);
}

uint64_t io_get_obb_redundant_seek_skips(void) {
    return __atomic_load_n(&obb_redundant_seek_skips, __ATOMIC_RELAXED);
}

void io_get_obb_stats(ObbIoStats *stats) {
    if (!stats) {
        return;
    }
    stats->read_calls = __atomic_load_n(&obb_io_stats.read_calls,
                                        __ATOMIC_RELAXED);
    stats->read_bytes = __atomic_load_n(&obb_io_stats.read_bytes,
                                        __ATOMIC_RELAXED);
    stats->read_us = __atomic_load_n(&obb_io_stats.read_us,
                                     __ATOMIC_RELAXED);
    stats->seek_calls = __atomic_load_n(&obb_io_stats.seek_calls,
                                        __ATOMIC_RELAXED);
    stats->seek_us = __atomic_load_n(&obb_io_stats.seek_us,
                                     __ATOMIC_RELAXED);
    stats->hotcache_hit_bytes = __atomic_load_n(
        &obb_io_stats.hotcache_hit_bytes, __ATOMIC_RELAXED);
    stats->hotcache_miss_bytes = __atomic_load_n(
        &obb_io_stats.hotcache_miss_bytes, __ATOMIC_RELAXED);
    stats->hotcache_read_us = __atomic_load_n(
        &obb_io_stats.hotcache_read_us, __ATOMIC_RELAXED);
    stats->hotcache_ram_hit_bytes = __atomic_load_n(
        &obb_io_stats.hotcache_ram_hit_bytes, __ATOMIC_RELAXED);
    stats->hotcache_page_fills = __atomic_load_n(
        &obb_io_stats.hotcache_page_fills, __ATOMIC_RELAXED);
    stats->hotcache_page_read_calls = __atomic_load_n(
        &obb_io_stats.hotcache_page_read_calls, __ATOMIC_RELAXED);
    stats->hotcache_page_read_bytes = __atomic_load_n(
        &obb_io_stats.hotcache_page_read_bytes, __ATOMIC_RELAXED);
    stats->fallback_read_us = __atomic_load_n(
        &obb_io_stats.fallback_read_us, __ATOMIC_RELAXED);
}

void io_loading_profile_begin(void) {
    __atomic_store_n(&obb_profile_active, 0, __ATOMIC_RELEASE);
    memset(obb_profile_bitmap, 0, sizeof(obb_profile_bitmap));
    __atomic_store_n(&obb_profile_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&obb_profile_active, 1, __ATOMIC_RELEASE);
}

void io_loading_profile_finish(void) {
    __atomic_store_n(&obb_profile_active, 0, __ATOMIC_RELEASE);
    uint32_t count = __atomic_load_n(&obb_profile_count, __ATOMIC_ACQUIRE);
    if (count > OBB_PAGE_COUNT) {
        count = OBB_PAGE_COUNT;
    }

    if (!file_mkpath(OBB_PROFILE_PATH, 0777)) {
        l_warn("Could not create the OBB page-profile directory.");
        return;
    }
    FILE *output = fopen(OBB_PROFILE_TEMP_PATH, "wb");
    if (!output) {
        l_warn("Could not open %s for the OBB page profile.",
               OBB_PROFILE_TEMP_PATH);
        return;
    }
    ObbPageFileHeader header = {
        .magic = {'N', 'H', 'P', '1'},
        .version = 1,
        .page_size = OBB_PAGE_SIZE,
        .obb_size = EXPECTED_OBB_SIZE,
        .entry_count = count,
        .reserved = 0,
    };
    memcpy(header.obb_sha256, expected_obb_sha256,
           sizeof(expected_obb_sha256));
    bool ok = fwrite(&header, sizeof(header), 1, output) == 1;
    if (ok) {
        ok = fwrite(obb_profile_order, sizeof(obb_profile_order[0]), count,
                    output) == count;
    }
    if (fclose(output) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(OBB_PROFILE_TEMP_PATH);
        l_warn("Could not finish the OBB first-touch page profile.");
        return;
    }
    remove(OBB_PROFILE_PATH);
    if (rename(OBB_PROFILE_TEMP_PATH, OBB_PROFILE_PATH) != 0) {
        remove(OBB_PROFILE_TEMP_PATH);
        l_warn("Could not publish the OBB first-touch page profile.");
        return;
    }
    l_success("Saved bounded OBB first-touch profile: %u unique 64 KiB "
              "pages at %s.", count, OBB_PROFILE_PATH);
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {
        l_debug("stat(%s): returning 0 in case this is a check for OpenSLES support", path);
        return 0;
    }

    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
    untrack_obb_stream(f);
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    DIR* ret = opendir(_pathname);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
