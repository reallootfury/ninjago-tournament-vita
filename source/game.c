#include "game.h"

#include "obb.h"
#include "reimpl/asset_manager.h"
#include "reimpl/egl.h"
#include "utils/logger.h"
#include "utils/utils.h"

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#define EXPECTED_SO_SIZE UINT64_C(4454280)
#define EXPECTED_SO_CRC32 UINT32_C(0xde6aa3c0)
#define EXPECTED_OBB_SIZE UINT64_C(973671465)
#define FUSION_TOUCH_SLOTS 16

extern so_module so_mod;

typedef void (*NativeInit)(JNIEnv *, jclass, jobject, jobject);
typedef void (*NativeVoid)(JNIEnv *, jclass);
typedef void (*NativeResize)(JNIEnv *, jclass, jint, jint);
typedef void (*NativeBool)(JNIEnv *, jclass, jboolean);
typedef jboolean (*NativeBack)(JNIEnv *, jclass);
typedef void (*NativeString)(JNIEnv *, jclass, jstring);
typedef void (*NativeDeviceStrings)(JNIEnv *, jclass, jstring, jstring,
                                    jstring, jstring);
typedef void (*NativeAssetManager)(JNIEnv *, jclass, jobject);
typedef void (*NativeTouch)(JNIEnv *, jclass, jint, jfloat, jfloat, jfloat);

static NativeInit native_init;
static NativeVoid native_render;
static NativeResize native_resize;
static NativeVoid native_pause;
static NativeVoid native_resume;
static NativeBool native_window_focus;
static NativeBack native_back;
static NativeString native_set_write_path;
static NativeString native_set_save_path;
static NativeString native_set_cache_path;
static NativeString native_set_command_line;
static NativeDeviceStrings native_set_device_strings;
static NativeAssetManager native_set_asset_manager;
static NativeTouch native_touch_gesture_start;
static NativeTouch native_touch_gesture_end;
static NativeTouch native_touch_down;
static NativeTouch native_touch_up;
static NativeTouch native_touch_move;
static NativeVoid native_touch_cancel_all;
static FusionAddPackage add_package;
static FusionAddPackageEntry add_package_entry;
static volatile uint8_t *fusion_loading_screen_active;
static atomic_bool exit_requested = ATOMIC_VAR_INIT(false);
static atomic_bool game_paused = ATOMIC_VAR_INIT(true);

static void *required_symbol(const char *name) {
    void *address = (void *)so_symbol(&so_mod, name);
    if (!address) {
        l_error("Required Fusion export is missing: %s", name);
    }
    return address;
}

static int verify_runtime_files(void) {
    FILE *file = fopen(SO_PATH, "rb");
    if (!file) {
        l_error("Could not open %s for fingerprint verification.", SO_PATH);
        return -1;
    }
    unsigned char buffer[64 * 1024];
    uint64_t size = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        crc = crc32(crc, buffer, (uInt)count);
        size += count;
    }
    fclose(file);
    if (size != EXPECTED_SO_SIZE || (uint32_t)crc != EXPECTED_SO_CRC32) {
        l_error("Unsupported libLEGO_M1.so: size=%llu CRC32=%08x.",
                (unsigned long long)size, (unsigned int)crc);
        return -2;
    }
    if (!file_exists(OBB_PATH) || file_size(OBB_PATH) != EXPECTED_OBB_SIZE) {
        l_error("Missing or wrong-sized 1057 expansion file: %s", OBB_PATH);
        return -3;
    }
    l_success("Verified 1.05.2.970/1057 runtime fingerprints.");
    return 0;
}

int ninjago_game_bind(void) {
#define BIND(variable, name) \
    do { variable = (__typeof__(variable))required_symbol(name); } while (0)
    BIND(native_init,
         "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeInit");
    BIND(native_render,
         "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeRender");
    BIND(native_resize,
         "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResize");
    BIND(native_pause,
         "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativePause");
    BIND(native_resume,
         "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeResume");
    BIND(native_window_focus,
         "Java_com_wbgames_LEGOgame_GameGLSurfaceView_nativeWindowFocusChanged");
    BIND(native_back,
         "Java_com_wbgames_LEGOgame_Fusion_nativeBackButtonPressed");
    BIND(native_set_write_path,
         "Java_com_wbgames_LEGOgame_Fusion_nativeSetWritePath");
    BIND(native_set_save_path,
         "Java_com_wbgames_LEGOgame_Fusion_nativeSetSavePath");
    BIND(native_set_cache_path,
         "Java_com_wbgames_LEGOgame_Fusion_nativeSetCachePath");
    BIND(native_set_command_line,
         "Java_com_wbgames_LEGOgame_Fusion_nativeSetCommandLine");
    BIND(native_set_device_strings,
         "Java_com_wbgames_LEGOgame_Fusion_nativeSetDeviceStrings");
    BIND(native_set_asset_manager,
         "Java_com_wbgames_LEGOgame_Fusion_nativeInitializeAssetManager");
    BIND(native_touch_gesture_start,
         "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventGestureStart");
    BIND(native_touch_gesture_end,
         "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventGestureEnd");
    BIND(native_touch_down,
         "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventDown");
    BIND(native_touch_up,
         "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventUp");
    BIND(native_touch_move,
         "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventMove");
    BIND(native_touch_cancel_all,
         "Java_com_wbgames_LEGOgame_Fusion_nativeTouchEventCancelAll");
    BIND(add_package, "_Z21fnOBBPackages_AddFilePKcb");
    BIND(add_package_entry, "_Z26fnOBBPackages_AddFileEntryjPKcyy");
    BIND(fusion_loading_screen_active, "gLoadingScreenIsLoading");
#undef BIND

    if (!native_init || !native_render || !native_resize || !native_pause ||
        !native_resume || !native_window_focus || !native_back ||
        !native_set_write_path || !native_set_save_path ||
        !native_set_cache_path || !native_set_command_line ||
        !native_set_device_strings || !native_set_asset_manager ||
        !native_touch_gesture_start || !native_touch_gesture_end ||
        !native_touch_down || !native_touch_up || !native_touch_move ||
        !native_touch_cancel_all || !add_package || !add_package_entry ||
        !fusion_loading_screen_active) {
        return -1;
    }
    return verify_runtime_files();
}

static jstring game_string(const char *text) {
    return jni->NewStringUTF(&jni, text);
}

static int prepare_runtime_directories(void) {
    /*
     * file_mkpath creates every directory component before the final filename.
     * Fusion expects Android's Java activity to have prepared these locations
     * before nativeResize starts compiling and caching shader programs.
     */
    static const char *sentinel_paths[] = {
        DATA_PATH "save/.directory",
        DATA_PATH "cache/shaderbinaries/.directory",
    };
    for (size_t i = 0;
         i < sizeof(sentinel_paths) / sizeof(sentinel_paths[0]); ++i) {
        if (!file_mkpath(sentinel_paths[i], 0777)) {
            l_error("Could not prepare runtime directory for %s.",
                    sentinel_paths[i]);
            return -1;
        }
    }
    l_success("Prepared save and shader-cache directories.");
    return 0;
}

int ninjago_game_boot(void) {
    if (prepare_runtime_directories() < 0) {
        return -4;
    }

    native_set_write_path(&jni, NULL, game_string(DATA_PATH));
    native_set_save_path(&jni, NULL, game_string(DATA_PATH "save/"));
    native_set_cache_path(&jni, NULL, game_string(DATA_PATH "cache/"));
    native_set_asset_manager(&jni, NULL, (jobject)AAssetManager_create());
    native_set_command_line(&jni, NULL, game_string(""));
    native_set_device_strings(&jni, NULL,
        game_string("PlayStation Vita"),
        game_string("ninjago-vita"),
        game_string("Sony"),
        game_string("SceGxm"));

    int entries = ninjago_register_obb(OBB_PATH, add_package,
                                       add_package_entry);
    if (entries < 0) {
        return entries;
    }

    /*
     * The Android renderer calls nativeInit at surface creation, then
     * nativeResize. Fusion_OnceInit is reached from nativeResize, so package
     * registration must be complete before this point.
     */
    l_info("Preparing the Vita EGL host context.");
    egl_host_prepare();
    l_info("Calling Fusion nativeInit.");
    native_init(&jni, NULL, (jobject)1, (jobject)2);
    l_success("Fusion nativeInit completed.");
    l_info("Calling Fusion nativeResize; initial shaders may take time.");
    native_resize(&jni, NULL, 960, 544);
    l_success("Fusion nativeResize completed.");
    l_info("Sending Fusion resume and window-focus events.");
    /*
     * Use the public state transition here instead of calling the native
     * callbacks directly. game_paused starts true, and the render loop is
     * gated by that flag.
     */
    ninjago_game_resume();
    l_success("Fusion lifecycle initialized at 960x544.");
    return 0;
}

void ninjago_game_render(void) {
    static bool first_render = true;
    if (first_render) {
        l_info("Entering first Fusion nativeRender call.");
    }
    native_render(&jni, NULL);
    if (first_render) {
        l_success("First Fusion nativeRender call returned.");
        first_render = false;
    }
}

void ninjago_game_pause(void) {
    if (atomic_exchange_explicit(&game_paused, true,
                                 memory_order_acq_rel)) {
        return;
    }
    native_window_focus(&jni, NULL, JNI_FALSE);
    native_pause(&jni, NULL);
}

void ninjago_game_resume(void) {
    if (!atomic_exchange_explicit(&game_paused, false,
                                  memory_order_acq_rel)) {
        return;
    }
    native_resume(&jni, NULL);
    native_window_focus(&jni, NULL, JNI_TRUE);
}

void ninjago_game_back(void) {
    if (!native_back(&jni, NULL)) {
        l_info("Fusion did not consume the back-button request.");
    }
}

void ninjago_game_touch_down(int id, float x, float y, float pressure,
                             bool first_contact) {
    if (id < 0 || id >= FUSION_TOUCH_SLOTS) {
        l_error("Rejected out-of-range Fusion touch-down ID %d.", id);
        return;
    }
    NativeTouch callback = first_contact
        ? native_touch_gesture_start : native_touch_down;
    callback(&jni, NULL, id, x, y, pressure);
}

void ninjago_game_touch_move(int id, float x, float y, float pressure) {
    if (id < 0 || id >= FUSION_TOUCH_SLOTS) {
        l_error("Rejected out-of-range Fusion touch-move ID %d.", id);
        return;
    }
    native_touch_move(&jni, NULL, id, x, y, pressure);
}

void ninjago_game_touch_up(int id, float x, float y, float pressure,
                           bool last_contact) {
    if (id < 0 || id >= FUSION_TOUCH_SLOTS) {
        l_error("Rejected out-of-range Fusion touch-up ID %d.", id);
        return;
    }
    NativeTouch callback = last_contact
        ? native_touch_gesture_end : native_touch_up;
    callback(&jni, NULL, id, x, y, pressure);
}

void ninjago_game_cancel_touches(void) {
    native_touch_cancel_all(&jni, NULL);
}

void ninjago_game_request_exit(void) {
    atomic_store_explicit(&exit_requested, true, memory_order_release);
}

bool ninjago_game_exit_requested(void) {
    return atomic_load_explicit(&exit_requested, memory_order_acquire);
}

bool ninjago_game_paused(void) {
    return atomic_load_explicit(&game_paused, memory_order_acquire);
}

bool ninjago_game_is_loading(void) {
    return fusion_loading_screen_active &&
           *fusion_loading_screen_active != 0;
}
