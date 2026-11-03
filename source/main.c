#include "game.h"
#include "reimpl/controls.h"
#include "reimpl/io.h"
#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/init.h"
#include "utils/logger.h"
#include "utils/livearea.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <so_util/so_util.h>

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

#define LOADING_BUSY_FRAME_INTERVAL_US 66667
#define LOADING_IDLE_FRAME_INTERVAL_US 16667
#define LOADING_IO_IDLE_THRESHOLD_US 1500000

/* SCE_AUDIO_OUT_PORT_TYPE_MAIN only accepts 48 kHz.  OpenSLES declares this
 * override weakly and otherwise defaults to 44.1 kHz, which makes the Vita
 * audio thread exit before it can process synchronous player destruction. */
int _opensles_user_freq = 48000;

static void log_startup_duration(const char *phase, uint64_t start,
                                 uint64_t end) {
    uint64_t tenths = ((end - start) + 50000) / 100000;
    l_info("Startup timing: %s completed in %llu.%llu seconds.", phase,
           (unsigned long long)(tenths / 10),
           (unsigned long long)(tenths % 10));
}

int main(void) {
    uint64_t startup_start = sceKernelGetProcessTimeWide();
    log_start_session();
    log_write_raw("Ninjago Tournament " NINJAGO_BUILD_REVISION
                  "; renderer " NINJAGO_RENDERER_REVISION ".\n");
#ifdef NINJAGO_RUNTIME_LOGGING
    livearea_log_assets();
#endif
    soloader_init_all();
    uint64_t loader_ready = sceKernelGetProcessTimeWide();
    log_startup_duration("loader and VitaGL initialization", startup_start,
                         loader_ready);
    io_prepare_hotcache();

    uint64_t fusion_boot_start = sceKernelGetProcessTimeWide();
#ifdef NINJAGO_RUNTIME_LOGGING
    const char *failure_help = "See ux0:data/ninjago/loader.log.";
#else
    const char *failure_help = "Use the logging VPK to collect diagnostic details.";
#endif
    if (ninjago_game_bind() < 0) {
        fatal_error("The loaded executable is not the supported "
                    "1.05.2.970/1057 libLEGO_M1.so. %s", failure_help);
    }
    if (ninjago_game_boot() < 0) {
        fatal_error("Fusion startup or OBB registration failed. %s", failure_help);
    }
    uint64_t fusion_boot_end = sceKernelGetProcessTimeWide();
    log_startup_duration("Fusion native boot", fusion_boot_start,
                         fusion_boot_end);
    controls_init();
    log_startup_duration("total loader startup", startup_start,
                         sceKernelGetProcessTimeWide());

    uint64_t performance_start = sceKernelGetProcessTimeWide();
    uint64_t accumulated_render_us = 0;
    uint64_t accumulated_swap_us = 0;
    unsigned int measured_frames = 0;
    GlPerformanceCounters performance_gl_start;
    gl_get_performance_counters(&performance_gl_start);
    bool loading_active = false;
    bool loading_io_idle = false;
    uint64_t loading_start = 0;
    uint64_t loading_render_us = 0;
    uint64_t loading_swap_us = 0;
    uint64_t loading_redundant_seek_skips = 0;
    ObbIoStats loading_obb_start = {0};
    GlPerformanceCounters loading_gl_start = {0};
    unsigned int last_special_serial = 0, special_reports = 0;
    uint64_t special_start = 0, special_max_frame_us = 0;
    unsigned int special_frames = 0, special_slow_frames = 0;
    GlPerformanceCounters special_gl_start = {0};

    while (!ninjago_game_exit_requested()) {
        controls_poll();
        ninjago_patch_poll();
        unsigned int special_serial = controls_get_special_press_serial();
        if (special_serial != last_special_serial) {
            last_special_serial = special_serial;
            if (!special_start && special_reports < 12) {
                special_start = sceKernelGetProcessTimeWide();
                special_max_frame_us = 0;
                special_frames = special_slow_frames = 0;
                gl_get_performance_counters(&special_gl_start);
            }
        }
        if (!ninjago_game_paused()) {
            uint64_t render_start = sceKernelGetProcessTimeWide();
            ninjago_game_render();
            uint64_t render_end = sceKernelGetProcessTimeWide();
            gl_swap();
            uint64_t swap_end = sceKernelGetProcessTimeWide();
            if (special_start) {
                uint64_t frame_us = swap_end - render_start;
                if (frame_us > special_max_frame_us) special_max_frame_us = frame_us;
                ++special_frames;
                if (frame_us > 25000) ++special_slow_frames;
                if (swap_end - special_start >= 3000000) {
                    GlPerformanceCounters end;
                    gl_get_performance_counters(&end);
                    ++special_reports;
                    l_info("R-special input window %u: frames=%u slow>25ms=%u "
                           "max-render/swap=%llu us shader-compile=%llu us/%u "
                           "link=%llu us/%u texture=%llu us. "
                           "Input correlation, not proof of effect causation.",
                           special_reports, special_frames, special_slow_frames,
                           (unsigned long long)special_max_frame_us,
                           (unsigned long long)(end.shader_compile_us - special_gl_start.shader_compile_us),
                           end.shader_compiles - special_gl_start.shader_compiles,
                           (unsigned long long)(end.program_link_us - special_gl_start.program_link_us),
                           end.program_links - special_gl_start.program_links,
                           (unsigned long long)(end.compressed_texture_upload_us - special_gl_start.compressed_texture_upload_us));
                    special_start = 0;
                }
            }

            bool is_loading = ninjago_game_is_loading();
            if (is_loading && !loading_active) {
                loading_active = true;
                loading_io_idle = false;
                loading_start = render_start;
                loading_render_us = 0;
                loading_swap_us = 0;
                loading_redundant_seek_skips =
                    io_get_obb_redundant_seek_skips();
                io_get_obb_stats(&loading_obb_start);
                gl_get_performance_counters(&loading_gl_start);
                io_loading_profile_begin();
                l_info("Fusion loading phase began; presentation uses an "
                       "adaptive 15 FPS cap during OBB activity and 60 FPS "
                       "after storage becomes idle.");
            } else if (!is_loading && loading_active) {
                uint64_t loading_us = swap_end - loading_start;
                uint64_t tenths = (loading_us + 50000) / 100000;
                l_success("Fusion loading phase completed in %llu.%llu "
                          "seconds.",
                          (unsigned long long)(tenths / 10),
                          (unsigned long long)(tenths % 10));
                l_info("Loading OBB optimization skipped %llu redundant "
                       "same-position filesystem seeks.",
                       (unsigned long long)(
                           io_get_obb_redundant_seek_skips() -
                           loading_redundant_seek_skips));
                ObbIoStats loading_obb_end;
                io_get_obb_stats(&loading_obb_end);
                GlPerformanceCounters loading_gl_end;
                gl_get_performance_counters(&loading_gl_end);
                uint64_t last_obb_activity =
                    io_get_last_obb_activity_us();
                uint64_t loading_idle_us = last_obb_activity >= loading_start &&
                    last_obb_activity < swap_end
                    ? swap_end - last_obb_activity : 0;
                uint64_t loading_active_us = loading_us - loading_idle_us;
                l_info("Loading OBB I/O: reads=%llu bytes=%llu "
                       "read-time=%llu ms, seeks=%llu seek-time=%llu ms "
                       "(cumulative across threads).",
                       (unsigned long long)(loading_obb_end.read_calls -
                                            loading_obb_start.read_calls),
                       (unsigned long long)(loading_obb_end.read_bytes -
                                            loading_obb_start.read_bytes),
                       (unsigned long long)((loading_obb_end.read_us -
                           loading_obb_start.read_us) / 1000),
                       (unsigned long long)(loading_obb_end.seek_calls -
                                            loading_obb_start.seek_calls),
                       (unsigned long long)((loading_obb_end.seek_us -
                           loading_obb_start.seek_us) / 1000));
                l_info("Loading hot cache: hit=%llu bytes, miss=%llu bytes, "
                       "positioned-read=%llu ms, OBB-fallback=%llu ms.",
                       (unsigned long long)(
                           loading_obb_end.hotcache_hit_bytes -
                           loading_obb_start.hotcache_hit_bytes),
                       (unsigned long long)(
                           loading_obb_end.hotcache_miss_bytes -
                           loading_obb_start.hotcache_miss_bytes),
                       (unsigned long long)((
                           loading_obb_end.hotcache_read_us -
                           loading_obb_start.hotcache_read_us) / 1000),
                       (unsigned long long)((
                           loading_obb_end.fallback_read_us -
                           loading_obb_start.fallback_read_us) / 1000));
                l_info("Loading RAM page cache: hit=%llu bytes, fills=%llu, "
                       "storage calls=%llu bytes=%llu (four 64 KiB pages).",
                       (unsigned long long)(loading_obb_end.hotcache_ram_hit_bytes -
                           loading_obb_start.hotcache_ram_hit_bytes),
                       (unsigned long long)(loading_obb_end.hotcache_page_fills -
                           loading_obb_start.hotcache_page_fills),
                       (unsigned long long)(loading_obb_end.hotcache_page_read_calls -
                           loading_obb_start.hotcache_page_read_calls),
                       (unsigned long long)(loading_obb_end.hotcache_page_read_bytes -
                           loading_obb_start.hotcache_page_read_bytes));
                l_info("Loading texture/program work: compressed calls=%u "
                       "bytes=%llu elapsed=%llu ms; ATC base uploads=%u "
                       "elapsed=%llu ms; cached-program materializations=%u "
                       "elapsed=%llu ms (cumulative across threads).",
                       loading_gl_end.compressed_texture_calls -
                           loading_gl_start.compressed_texture_calls,
                       (unsigned long long)(
                           loading_gl_end.compressed_texture_bytes -
                           loading_gl_start.compressed_texture_bytes),
                       (unsigned long long)((
                           loading_gl_end.compressed_texture_upload_us -
                           loading_gl_start.compressed_texture_upload_us) /
                           1000),
                       loading_gl_end.atc_base_uploads -
                           loading_gl_start.atc_base_uploads,
                       (unsigned long long)((
                           loading_gl_end.atc_base_upload_us -
                           loading_gl_start.atc_base_upload_us) / 1000),
                       loading_gl_end.cached_program_materializations -
                           loading_gl_start.cached_program_materializations,
                       (unsigned long long)((
                           loading_gl_end.cached_program_materialization_us -
                           loading_gl_start.cached_program_materialization_us) /
                           1000));
                l_info("Loading phase breakdown: I/O-active=%llu ms, "
                       "idle-finalization=%llu ms, CPU/render=%llu ms, "
                       "swap/fence=%llu ms, wall=%llu ms.",
                       (unsigned long long)(loading_active_us / 1000),
                       (unsigned long long)(loading_idle_us / 1000),
                       (unsigned long long)(loading_render_us / 1000),
                       (unsigned long long)(loading_swap_us / 1000),
                       (unsigned long long)(loading_us / 1000));
                io_loading_profile_finish();
                loading_active = false;
            }

            if (is_loading && loading_active) {
                loading_render_us += render_end - render_start;
                loading_swap_us += swap_end - render_end;
            }

            /*
             * During active reads, 15 FPS previously beat a fixed 30 FPS load
             * and reduced visible transition flicker. The latest trace then
             * spent roughly 25 seconds at 15 FPS with no OBB activity, which
             * can delay Fusion's frame-driven finalization. Raise only that
             * idle tail to 60 FPS and immediately restore 15 when I/O resumes.
             */
            if (is_loading) {
                uint64_t last_obb_activity =
                    io_get_last_obb_activity_us();
                bool io_idle = last_obb_activity != 0 &&
                    swap_end > last_obb_activity &&
                    swap_end - last_obb_activity >=
                        LOADING_IO_IDLE_THRESHOLD_US;
                if (io_idle != loading_io_idle) {
                    loading_io_idle = io_idle;
                    l_info("Fusion loading presentation switched to %s "
                           "because OBB I/O is %s.",
                           io_idle ? "60 FPS" : "15 FPS",
                           io_idle ? "idle" : "active");
                }
                uint64_t frame_interval = io_idle
                    ? LOADING_IDLE_FRAME_INTERVAL_US
                    : LOADING_BUSY_FRAME_INTERVAL_US;
                uint64_t frame_time = swap_end - render_start;
                if (frame_time < frame_interval) {
                    sceKernelDelayThread((unsigned int)(
                        frame_interval - frame_time));
                }
            }
            uint64_t frame_end = sceKernelGetProcessTimeWide();

            accumulated_render_us += render_end - render_start;
            /* Do not report the intentional loading delay as GPU swap time. */
            accumulated_swap_us += swap_end - render_end;
            ++measured_frames;

            uint64_t elapsed_us = frame_end - performance_start;
            if (elapsed_us >= 5 * 1000 * 1000 && measured_frames > 0) {
                GlPerformanceCounters performance_gl_end;
                gl_get_performance_counters(&performance_gl_end);
                unsigned int fps_x10 = (unsigned int)(
                    ((uint64_t)measured_frames * 10 * 1000 * 1000 +
                     elapsed_us / 2) / elapsed_us);
                unsigned int draws_x10 = (unsigned int)(
                    ((uint64_t)(performance_gl_end.draw_calls -
                                performance_gl_start.draw_calls) * 10 +
                     measured_frames / 2) / measured_frames);
                unsigned int skipped_binds_x10 = (unsigned int)(
                    ((uint64_t)(
                         performance_gl_end.redundant_state_binds_skipped -
                         performance_gl_start.redundant_state_binds_skipped) *
                         10 + measured_frames / 2) / measured_frames);
                uint64_t main_gl_wait_us =
                    performance_gl_end.main_gl_mutex_wait_us -
                    performance_gl_start.main_gl_mutex_wait_us;
                uint64_t background_gl_wait_us =
                    performance_gl_end.background_gl_mutex_wait_us -
                    performance_gl_start.background_gl_mutex_wait_us;
                uint64_t gpu_finish_wait_us =
                    performance_gl_end.gpu_finish_wait_us -
                    performance_gl_start.gpu_finish_wait_us;
                unsigned int framebuffer_async_transitions =
                    performance_gl_end.framebuffer_async_transitions -
                    performance_gl_start.framebuffer_async_transitions;
                l_info("Performance: %u.%u FPS, average render=%llu us, "
                       "fence/swap=%llu us, draws=%u.%u/frame, redundant "
                       "binds skipped=%u.%u/frame, main/background GL lock "
                       "wait=%llu/%llu us (%u/%u contentions), GPU finish "
                       "wait=%llu us (%u waits), async framebuffer-to-display "
                       "transitions=%u, shader compile/link=%llu/"
                       "%llu us (%u/%u operations), duplicate compile "
                       "finishes skipped=%u over %u frames.",
                       fps_x10 / 10, fps_x10 % 10,
                       (unsigned long long)(
                           accumulated_render_us / measured_frames),
                       (unsigned long long)(
                           accumulated_swap_us / measured_frames),
                       draws_x10 / 10, draws_x10 % 10,
                       skipped_binds_x10 / 10, skipped_binds_x10 % 10,
                       (unsigned long long)main_gl_wait_us,
                       (unsigned long long)background_gl_wait_us,
                       performance_gl_end.main_gl_mutex_contentions -
                           performance_gl_start.main_gl_mutex_contentions,
                       performance_gl_end.background_gl_mutex_contentions -
                           performance_gl_start.background_gl_mutex_contentions,
                       (unsigned long long)gpu_finish_wait_us,
                       performance_gl_end.gpu_finish_waits -
                           performance_gl_start.gpu_finish_waits,
                       framebuffer_async_transitions,
                       (unsigned long long)(
                           performance_gl_end.shader_compile_us -
                           performance_gl_start.shader_compile_us),
                       (unsigned long long)(
                           performance_gl_end.program_link_us -
                           performance_gl_start.program_link_us),
                       performance_gl_end.shader_compiles -
                           performance_gl_start.shader_compiles,
                       performance_gl_end.program_links -
                           performance_gl_start.program_links,
                       performance_gl_end.duplicate_compile_finishes_skipped -
                           performance_gl_start.
                               duplicate_compile_finishes_skipped,
                       measured_frames);
                performance_gl_start = performance_gl_end;
                performance_start = frame_end;
                accumulated_render_us = 0;
                accumulated_swap_us = 0;
                measured_frames = 0;
            }
        } else {
            /* Avoid a busy spin while Android has suspended rendering. */
            sceKernelDelayThread(1000);
        }
    }

    ninjago_game_pause();
    log_flush();
    sceKernelExitDeleteThread(0);
    return 0;
}

void controls_handler_key(int32_t keycode, ControlsAction action) {
    if (keycode == AKEYCODE_BACK && action == CONTROLS_ACTION_DOWN) {
        ninjago_game_back();
    }
}

void controls_handler_touch(int32_t id, float x, float y,
                            ControlsAction action) {
    switch (action) {
        case CONTROLS_ACTION_DOWN:
            ninjago_game_touch_down(id, x, y, 1.0f, true);
            break;
        case CONTROLS_ACTION_MOVE:
            ninjago_game_touch_move(id, x, y, 1.0f);
            break;
        case CONTROLS_ACTION_UP:
            ninjago_game_touch_up(id, x, y, 0.0f, true);
            break;
        case CONTROLS_ACTION_CANCEL:
            ninjago_game_cancel_touches();
            break;
    }
}

void controls_handler_analog(ControlsStickId which, float x, float y,
                             ControlsAction action) {
    (void)which;
    (void)x;
    (void)y;
    (void)action;
}

void controls_handler_exit_request(void) {
    ninjago_game_request_exit();
}
