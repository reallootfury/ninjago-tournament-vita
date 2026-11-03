/*
 * fios.c -- use FIOS2 for optimized I/O
 *
 * Copyright (C) 2021 Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See licenses/upstream-MIT.txt for details.
 */

#include <malloc.h>
#include <psp2/kernel/cpu.h>

#include "fios.h"

#define MAX_PATH_LENGTH 256
#define RAMCACHEBLOCKSIZE (16 * 1024)
#define RAMCACHEBLOCKNUM 512

static int64_t g_OpStorage[SCE_FIOS_OP_STORAGE_SIZE(64, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_ChunkStorage[SCE_FIOS_CHUNK_STORAGE_SIZE(1024) / sizeof(int64_t) + 1];
static int64_t g_FHStorage[SCE_FIOS_FH_STORAGE_SIZE(1024, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];
static int64_t g_DHStorage[SCE_FIOS_DH_STORAGE_SIZE(32, MAX_PATH_LENGTH) / sizeof(int64_t) + 1];

static SceFiosRamCacheContext g_RamCacheContext = SCE_FIOS_RAM_CACHE_CONTEXT_INITIALIZER;
static char *g_RamCacheWorkBuffer;

int fios_init(const char * path) {
    int res;

    SceFiosParams params = SCE_FIOS_PARAMS_INITIALIZER;
    params.opStorage.pPtr = g_OpStorage;
    params.opStorage.length = sizeof(g_OpStorage);
    params.chunkStorage.pPtr = g_ChunkStorage;
    params.chunkStorage.length = sizeof(g_ChunkStorage);
    params.fhStorage.pPtr = g_FHStorage;
    params.fhStorage.length = sizeof(g_FHStorage);
    params.dhStorage.pPtr = g_DHStorage;
    params.dhStorage.length = sizeof(g_DHStorage);
    params.pathMax = MAX_PATH_LENGTH;

    /*
     * Fusion loads arenas through two asynchronous storage workers plus
     * callback/decompression work. Hardware traces measured 67.2 seconds with
     * all three user cores available and 87.4 seconds after reserving core 0,
     * so retain the scheduler freedom demonstrated to be faster. Fusion's GL
     * calls remain protected by the loader's existing synchronization.
     */
    params.threadAffinity[SCE_FIOS_IO_THREAD] =
        SCE_KERNEL_CPU_MASK_USER_ALL;
    params.threadAffinity[SCE_FIOS_CALLBACK_THREAD] =
        SCE_KERNEL_CPU_MASK_USER_ALL;
    params.threadAffinity[SCE_FIOS_DECOMPRESSOR_THREAD] =
        SCE_KERNEL_CPU_MASK_USER_ALL;

    params.threadPriority[SCE_FIOS_IO_THREAD] = 64;
    params.threadPriority[SCE_FIOS_CALLBACK_THREAD] = 191;
    params.threadPriority[SCE_FIOS_DECOMPRESSOR_THREAD] = 191;

    res = sceFiosInitialize(&params);
    if (res < 0)
        return res;

    /*
     * Ninjago issues thousands of small, seek-heavy reads. The 16 KiB block
     * geometry measured better than 8 KiB and 32 KiB in controlled runs.
     * Expanding this cache from 8 MiB to 16 MiB then regressed the same arena
     * from 58.9 to 65.0 seconds, so retain the smaller measured configuration.
     */
    g_RamCacheWorkBuffer = memalign(8, RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE);
    if (!g_RamCacheWorkBuffer)
        return -1;

    g_RamCacheContext.pPath = path;
    g_RamCacheContext.pWorkBuffer = g_RamCacheWorkBuffer;
    g_RamCacheContext.workBufferSize = RAMCACHEBLOCKNUM * RAMCACHEBLOCKSIZE;
    g_RamCacheContext.blockSize = RAMCACHEBLOCKSIZE;
    res = sceFiosIOFilterAdd(0, sceFiosIOFilterCache, &g_RamCacheContext);
    if (res < 0)
        return res;

    return 0;
}

void fios_terminate(void) {
    sceFiosTerminate();
    free(g_RamCacheWorkBuffer);
}
