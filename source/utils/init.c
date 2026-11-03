#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <falso_jni/FalsoJNI.h>
#include <fios/fios.h>
#include <psp2/power.h>

#define LOAD_ADDRESS 0x98000000

extern so_module so_mod;

void soloader_init_all(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    l_success("Fusion pthread initialization checks use the lock-free "
              "exact hash registry.");

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0) {
        l_success("FIOS initialized for %s with two I/O workers plus "
                  "callbacks/decompression schedulable across all three "
                  "Vita user cores and an 8 MiB cache using 16 KiB blocks.",
                  DATA_PATH);
    }
#endif

    if (!module_loaded("kubridge")) {
        fatal_error("kubridge.skprx is required but is not loaded.");
    }
    if (!file_exists(SO_PATH)) {
        fatal_error("Missing %s. Run scripts/prepare_data.py and copy the "
                    "staged files to %s.", SO_PATH, DATA_PATH);
    }
    if (!file_exists(OBB_PATH)) {
        fatal_error("Missing matching 1057 expansion file: %s.", OBB_PATH);
    }
    if (!file_exists(DATA_PATH "assets/obb_info.xml")) {
        fatal_error("Missing APK asset %sassets/obb_info.xml. Re-run the "
                    "current prepare_data.py tool.", DATA_PATH);
    }

    int result = so_file_load(&so_mod, SO_PATH, LOAD_ADDRESS);
    if (result < 0) {
        fatal_error("Could not load %s (0x%08X).", SO_PATH, result);
    }
    if (!so_mod.exidx_base || !so_mod.exidx_count) {
        fatal_error("The Android executable has no ARM exception index.");
    }
    l_success("Loaded ARM executable; exception index has %u entries.",
              (unsigned int)so_mod.exidx_count);

    so_relocate(&so_mod);
    l_success("Android relocations applied.");
    resolve_imports(&so_mod);
    l_success("Android imports resolved.");
    so_patch();
    so_flush_caches(&so_mod);
    l_success("Compatibility patches installed and caches flushed.");
    so_initialize(&so_mod);
    l_success("Android static initializers completed.");

    gl_preload();
    l_success("Vita shader compiler ready.");
    jni_init();
    l_success("FalsoJNI initialized.");
}
