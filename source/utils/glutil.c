/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See licenses/upstream-MIT.txt for details.
 */

#include "utils/glutil.h"

#include "utils/utils.h"
#include "utils/dialog.h"
#include "utils/logger.h"
#include "utils/sha256.h"

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>

static bool gl_initialized;
static unsigned int shader_compile_count;
static unsigned int program_link_count;
static unsigned int framebuffer_bind_count;
static unsigned int draw_count;
static unsigned int redundant_state_bind_count;
static unsigned int explicit_finish_count;
static unsigned int gl_main_mutex_contentions;
static unsigned int gl_background_mutex_contentions;
static uint64_t gl_main_mutex_wait_us;
static uint64_t gl_background_mutex_wait_us;
static unsigned int gl_gpu_finish_wait_count;
static uint64_t gl_gpu_finish_wait_us;
static unsigned int gl_framebuffer_async_transition_count;
static unsigned int gl_background_texture_delete_count;
static uint64_t gl_shader_compile_us;
static uint64_t gl_program_link_us;
static unsigned int gl_duplicate_compile_finish_count;
static unsigned int gl_native_binary_identity_reports;
static SceKernelLwMutexWork gl_api_mutex;
static atomic_bool gl_api_mutex_ready = ATOMIC_VAR_INIT(false);
static atomic_bool background_gl_reported = ATOMIC_VAR_INIT(false);
static atomic_uint gl_frame_sequence = ATOMIC_VAR_INIT(0);
static SceUID gl_owner_thread;

#define GL_TRACKED_TEXTURE_UNITS SCE_GXM_MAX_TEXTURE_UNITS
#define GL_TEXTURE_THREAD_SLOTS 32
#define GL_TEXTURE_RECORD_SLOTS 16384
#define GL_UNIFORM_RECORD_SLOTS 8192
#define GL_UNIFORM_LOCATION_TAG 0x40000000u
#define GL_UNIFORM_LOCATION_TAG_MASK 0xC0000000u
#define GL_UNIFORM_LOCATION_OFFSET_BITS 12
#define GL_UNIFORM_LOCATION_OFFSET_MASK 0xFFFu
#define GL_UNIFORM_LOCATION_INDEX_MASK (GL_UNIFORM_RECORD_SLOTS - 1)

typedef struct {
    bool used;
    SceUID thread_id;
    GLenum active_texture;
    GLuint texture_2d[GL_TRACKED_TEXTURE_UNITS];
    GLuint texture_cube[GL_TRACKED_TEXTURE_UNITS];
    GLuint array_buffer;
    GLuint element_array_buffer;
    GLuint framebuffer;
    GLuint program;
} gl_thread_texture_state;

typedef struct {
    bool base_defined;
    bool generated_atc_mips;
    bool skipped_atc_mip;
    GLenum atc_format;
    GLsizei base_width;
    GLsizei base_height;
} gl_texture_record;

typedef struct {
    uint8_t state;
    GLuint program;
    GLint real_location;
    unsigned int array_size;
} gl_uniform_location_record;

/*
 * This loader currently links a VitaGL build without SAFE_UNIFORMS=1.
 * That build represents a uniform location as the negated address of this
 * private record. Only the leading fields are needed to apply an array offset;
 * their offsets are confirmed by the linked glUniform* implementation.
 */
typedef struct {
    const SceGxmProgramParameter *parameter;
    uint8_t *fragment_data;
    uint8_t *vertex_data;
    uint8_t type;
    uint8_t sampler_index;
} gl_vitagl_uniform_record;

_Static_assert((GL_UNIFORM_RECORD_SLOTS &
                (GL_UNIFORM_RECORD_SLOTS - 1)) == 0,
               "Uniform record slots must be a power of two");
_Static_assert(offsetof(gl_vitagl_uniform_record, fragment_data) == 4 &&
               offsetof(gl_vitagl_uniform_record, vertex_data) == 8 &&
               offsetof(gl_vitagl_uniform_record, type) == 12 &&
               offsetof(gl_vitagl_uniform_record, sampler_index) == 13,
               "VitaGL uniform record ABI changed");

extern GLboolean dirty_shader_frag_unifs;
extern GLboolean dirty_shader_vert_unifs;
extern void vglSetUniformData(uint8_t *uniform_buffer,
                              SceGxmParameterType parameter_type,
                              int offset, uint32_t count,
                              uint32_t component_count,
                              const void *source_data,
                              SceGxmParameterType input_type);

/*
 * VitaGL keeps the active unit and all texture bindings in process-global
 * state. Fusion issues texture registration calls from a background worker
 * while the render thread changes those same bindings. A mutex around each
 * individual call prevents simultaneous execution, but does not prevent this:
 *
 *   worker: glBindTexture(new_texture)
 *   render: glBindTexture(draw_texture)
 *   worker: glCompressedTexImage2D(...)  // updates draw_texture by mistake
 *
 * Preserve the binding state expected by each caller and restore it while the
 * API mutex is held before any texture operation or draw.
 */
static gl_thread_texture_state
    gl_texture_threads[GL_TEXTURE_THREAD_SLOTS];
static gl_texture_record gl_texture_records[GL_TEXTURE_RECORD_SLOTS];
static gl_uniform_location_record
    gl_uniform_records[GL_UNIFORM_RECORD_SLOTS];
static GLenum gl_actual_active_texture = GL_TEXTURE0;
static GLuint gl_actual_texture_2d[GL_TRACKED_TEXTURE_UNITS];
static GLuint gl_actual_texture_cube[GL_TRACKED_TEXTURE_UNITS];
static GLuint gl_actual_array_buffer;
static GLuint gl_actual_element_array_buffer;
static unsigned int gl_texture_restore_count;
static unsigned int gl_buffer_restore_count;
static unsigned int gl_framebuffer_restore_count;
static unsigned int gl_compressed_upload_count;
static uint64_t gl_compressed_upload_bytes;
static uint64_t gl_compressed_upload_us;
static unsigned int gl_atc_base_upload_count;
static uint64_t gl_atc_base_upload_us;
static unsigned int gl_atc_mip_skip_count;
static unsigned int gl_uniform_offset_count;
static bool gl_texture_thread_overflow_reported;
static bool gl_texture_record_overflow_reported;
static bool gl_uniform_record_overflow_reported;
static bool gl_uniform_invalid_location_reported;
static GLuint gl_current_program;
static GLuint gl_current_framebuffer;
static unsigned int gl_background_finish_frame = UINT32_MAX;
static unsigned int gl_coalesced_finish_count;

static bool gl_materialize_program_locked(GLuint program);

typedef struct gl_deferred_program_binary {
    GLuint program;
    GLenum format;
    GLsizei length;
    void *data;
    unsigned int sequence;
    struct gl_deferred_program_binary *next;
} gl_deferred_program_binary;

static gl_deferred_program_binary *gl_deferred_programs;
static unsigned int gl_deferred_program_count;
static unsigned int gl_materialized_program_count;
static uint64_t gl_materialized_program_us;

typedef struct {
    unsigned int sequence;
    SceUID thread_id;
    GLenum mode;
    GLsizei count;
    GLenum index_type;
    const void *indices;
    GLuint program;
    GLuint framebuffer;
    bool indexed;
} gl_draw_breadcrumb;

static gl_draw_breadcrumb gl_last_draw;

/*
 * VitaGL exposes one process-global GL/GXM context. Fusion also performs
 * texture, mesh and framebuffer registration/destruction from
 * geMain_BackgroundUpdateFunc, so calls from that worker must not overlap the
 * render thread. In particular, scene_reset() updates active_write_fb,
 * in_use_framebuffer and is_rendering_display in separate stores.
 */
static void gl_api_lock(void) {
    if (atomic_load_explicit(&gl_api_mutex_ready, memory_order_acquire)) {
        SceUID caller = sceKernelGetThreadId();
        if (caller != gl_owner_thread &&
            !atomic_exchange_explicit(&background_gl_reported, true,
                                      memory_order_relaxed)) {
            l_info("Serializing Fusion background VitaGL calls from thread "
                   "0x%08X.", caller);
        }
        int result = sceKernelTryLockLwMutex(&gl_api_mutex, 1);
        if (result == SCE_KERNEL_ERROR_LW_MUTEX_FAILED_TO_OWN) {
            uint64_t started = sceKernelGetProcessTimeWide();
            result = sceKernelLockLwMutex(&gl_api_mutex, 1, NULL);
            if (result >= 0) {
                uint64_t waited =
                    sceKernelGetProcessTimeWide() - started;
                if (caller == gl_owner_thread) {
                    ++gl_main_mutex_contentions;
                    gl_main_mutex_wait_us += waited;
                } else {
                    ++gl_background_mutex_contentions;
                    gl_background_mutex_wait_us += waited;
                }
            }
        } else if (result < 0) {
            /* Preserve the old blocking behavior for unexpected errors. */
            sceKernelLockLwMutex(&gl_api_mutex, 1, NULL);
        }
    }
}

static void gl_api_unlock(void) {
    if (atomic_load_explicit(&gl_api_mutex_ready, memory_order_relaxed)) {
        sceKernelUnlockLwMutex(&gl_api_mutex, 1);
    }
}

static void gl_wait_for_gpu_locked(void) {
    uint64_t started = sceKernelGetProcessTimeWide();
    glFinish();
    gl_gpu_finish_wait_us += sceKernelGetProcessTimeWide() - started;
    ++gl_gpu_finish_wait_count;
}

/*
 * Fusion renders a full-screen offscreen target, then samples it while
 * drawing the display scene. Both scenes are submitted through the same GXM
 * context, so ending the offscreen scene preserves their execution order; a
 * CPU-blocking finish is not needed on the offscreen-to-display edge. The
 * inverse edge still waits before Fusion overwrites that texture on the next
 * frame, preserving the resource-reuse boundary that fixed the GPU crashes.
 */
static void gl_close_framebuffer_transition_locked(GLuint next_framebuffer) {
    if (next_framebuffer == 0) {
        glFlush();
        ++gl_framebuffer_async_transition_count;
    } else {
        gl_wait_for_gpu_locked();
    }
}

/*
 * Fusion explicitly finishes the background resource thread repeatedly. A
 * shader-cache miss then reaches glCompileShader in that same presented frame.
 * One successful completion is enough until gl_swap advances the frame: no
 * render work can be submitted concurrently while this mutex is held.
 */
static bool gl_background_finish_required_locked(SceUID caller) {
    if (caller == gl_owner_thread) {
        return true;
    }
    unsigned int frame = atomic_load_explicit(
        &gl_frame_sequence, memory_order_acquire);
    if (gl_background_finish_frame == frame) {
        return false;
    }
    gl_background_finish_frame = frame;
    return true;
}

static int gl_texture_unit_index(GLenum texture) {
    if (texture < GL_TEXTURE0 ||
        texture >= GL_TEXTURE0 + GL_TRACKED_TEXTURE_UNITS) {
        return -1;
    }
    return (int)(texture - GL_TEXTURE0);
}

static int gl_texture_target_index(GLenum target) {
    if (target == GL_TEXTURE_2D) {
        return 0;
    }
    if (target == GL_TEXTURE_CUBE_MAP ||
        (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
         target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)) {
        return 1;
    }
    return -1;
}

static GLenum gl_texture_binding_target(int target_index) {
    return target_index == 0 ? GL_TEXTURE_2D : GL_TEXTURE_CUBE_MAP;
}

static gl_thread_texture_state *gl_get_thread_texture_state_locked(void) {
    SceUID caller = sceKernelGetThreadId();
    gl_thread_texture_state *free_slot = NULL;

    for (unsigned int i = 0; i < GL_TEXTURE_THREAD_SLOTS; ++i) {
        gl_thread_texture_state *state = &gl_texture_threads[i];
        if (state->used && state->thread_id == caller) {
            return state;
        }
        if (!state->used && !free_slot) {
            free_slot = state;
        }
    }

    if (!free_slot) {
        if (!gl_texture_thread_overflow_reported) {
            l_error("Texture-state thread table is full; reusing a hashed "
                    "slot for thread 0x%08X.", caller);
            gl_texture_thread_overflow_reported = true;
        }
        free_slot =
            &gl_texture_threads[(uint32_t)caller % GL_TEXTURE_THREAD_SLOTS];
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    free_slot->thread_id = caller;
    free_slot->active_texture = GL_TEXTURE0;
    return free_slot;
}

static GLuint *gl_thread_texture_binding(gl_thread_texture_state *state,
                                         int target_index, int unit) {
    return target_index == 0 ? &state->texture_2d[unit]
                             : &state->texture_cube[unit];
}

static GLuint *gl_actual_texture_binding(int target_index, int unit) {
    return target_index == 0 ? &gl_actual_texture_2d[unit]
                             : &gl_actual_texture_cube[unit];
}

static GLuint *gl_thread_buffer_binding(gl_thread_texture_state *state,
                                        GLenum target) {
    if (target == GL_ARRAY_BUFFER) {
        return &state->array_buffer;
    }
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        return &state->element_array_buffer;
    }
    return NULL;
}

static GLuint *gl_actual_buffer_binding(GLenum target) {
    if (target == GL_ARRAY_BUFFER) {
        return &gl_actual_array_buffer;
    }
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        return &gl_actual_element_array_buffer;
    }
    return NULL;
}

static void gl_restore_buffer_target_locked(
    gl_thread_texture_state *state, GLenum target) {
    GLuint *desired = gl_thread_buffer_binding(state, target);
    GLuint *actual = gl_actual_buffer_binding(target);
    if (!desired || !actual || *actual == *desired) {
        return;
    }
    glBindBuffer(target, *desired);
    *actual = *desired;
    unsigned int sequence = ++gl_buffer_restore_count;
    if (sequence <= 24) {
        l_info("Restored buffer binding %u for thread 0x%08X: "
               "target=0x%x object=%p.",
               sequence, state->thread_id, (unsigned int)target,
               (void *)(uintptr_t)*desired);
    }
}

static void gl_restore_all_buffers_locked(
    gl_thread_texture_state *state) {
    gl_restore_buffer_target_locked(state, GL_ARRAY_BUFFER);
    gl_restore_buffer_target_locked(state, GL_ELEMENT_ARRAY_BUFFER);
}

static void gl_restore_framebuffer_locked(
    const gl_thread_texture_state *state) {
    if (gl_current_framebuffer == state->framebuffer) {
        return;
    }

    /* Close the preceding VitaGL GXM scene before changing its target. */
    gl_close_framebuffer_transition_locked(state->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, state->framebuffer);
    gl_current_framebuffer = state->framebuffer;
    unsigned int sequence = ++gl_framebuffer_restore_count;
    if (sequence <= 16) {
        l_info("Restored framebuffer binding %u for thread 0x%08X: "
               "object=%p.", sequence, state->thread_id,
               (void *)(uintptr_t)state->framebuffer);
    }
}

static void gl_restore_program_locked(
    const gl_thread_texture_state *state) {
    if (gl_current_program == state->program) {
        return;
    }
    gl_materialize_program_locked(state->program);
    glUseProgram(state->program);
    gl_current_program = state->program;
}

static void gl_restore_active_texture_locked(
    const gl_thread_texture_state *state) {
    if (gl_actual_active_texture != state->active_texture) {
        glActiveTexture(state->active_texture);
        gl_actual_active_texture = state->active_texture;
    }
}

static void gl_restore_texture_target_locked(
    gl_thread_texture_state *state, GLenum target) {
    int unit = gl_texture_unit_index(state->active_texture);
    int target_index = gl_texture_target_index(target);
    if (unit < 0 || target_index < 0) {
        return;
    }

    gl_restore_active_texture_locked(state);
    GLuint desired =
        *gl_thread_texture_binding(state, target_index, unit);
    GLuint *actual = gl_actual_texture_binding(target_index, unit);
    if (*actual != desired) {
        glBindTexture(gl_texture_binding_target(target_index), desired);
        *actual = desired;
        unsigned int sequence = ++gl_texture_restore_count;
        if (sequence <= 16) {
            l_info("Restored texture binding %u for thread 0x%08X: "
                   "unit=%d target=0x%x object=%p.",
                   sequence, state->thread_id, unit,
                   (unsigned int)gl_texture_binding_target(target_index),
                   (void *)(uintptr_t)desired);
        }
    }
}

static void gl_restore_all_textures_locked(
    gl_thread_texture_state *state) {
    for (int unit = 0; unit < GL_TRACKED_TEXTURE_UNITS; ++unit) {
        for (int target_index = 0; target_index < 2; ++target_index) {
            GLuint desired =
                *gl_thread_texture_binding(state, target_index, unit);
            GLuint *actual =
                gl_actual_texture_binding(target_index, unit);
            if (*actual == desired) {
                continue;
            }
            GLenum texture_unit = GL_TEXTURE0 + unit;
            if (gl_actual_active_texture != texture_unit) {
                glActiveTexture(texture_unit);
                gl_actual_active_texture = texture_unit;
            }
            glBindTexture(gl_texture_binding_target(target_index), desired);
            *actual = desired;
            unsigned int sequence = ++gl_texture_restore_count;
            if (sequence <= 16) {
                l_info("Restored draw texture binding %u for thread "
                       "0x%08X: unit=%d target=0x%x object=%p.",
                       sequence, state->thread_id, unit,
                       (unsigned int)gl_texture_binding_target(target_index),
                       (void *)(uintptr_t)desired);
            }
        }
    }
    gl_restore_active_texture_locked(state);
}

static gl_texture_record *gl_get_texture_record_locked(GLuint texture) {
    if (texture == 0) {
        return NULL;
    }
    if (texture >= GL_TEXTURE_RECORD_SLOTS) {
        if (!gl_texture_record_overflow_reported) {
            l_error("Texture object %u is outside VitaGL's tracked slot "
                    "range; orphan-mip protection is disabled for it.",
                    (unsigned int)texture);
            gl_texture_record_overflow_reported = true;
        }
        return NULL;
    }
    return &gl_texture_records[texture];
}

static GLuint gl_current_texture_locked(gl_thread_texture_state *state,
                                        GLenum target) {
    int unit = gl_texture_unit_index(state->active_texture);
    int target_index = gl_texture_target_index(target);
    if (unit < 0 || target_index < 0) {
        return 0;
    }
    return *gl_thread_texture_binding(state, target_index, unit);
}

static void gl_forget_deleted_textures_locked(GLsizei n,
                                               const GLuint *textures) {
    if (n <= 0 || !textures) {
        return;
    }

    for (GLsizei deleted_index = 0; deleted_index < n; ++deleted_index) {
        GLuint deleted = textures[deleted_index];
        if (deleted == 0) {
            continue;
        }
        if (deleted < GL_TEXTURE_RECORD_SLOTS) {
            memset(&gl_texture_records[deleted], 0,
                   sizeof(gl_texture_records[deleted]));
        }
        for (unsigned int i = 0; i < GL_TEXTURE_THREAD_SLOTS; ++i) {
            gl_thread_texture_state *state = &gl_texture_threads[i];
            if (!state->used) {
                continue;
            }
            for (int unit = 0; unit < GL_TRACKED_TEXTURE_UNITS; ++unit) {
                if (state->texture_2d[unit] == deleted) {
                    state->texture_2d[unit] = 0;
                }
                if (state->texture_cube[unit] == deleted) {
                    state->texture_cube[unit] = 0;
                }
            }
        }
        for (int unit = 0; unit < GL_TRACKED_TEXTURE_UNITS; ++unit) {
            if (gl_actual_texture_2d[unit] == deleted) {
                gl_actual_texture_2d[unit] = UINT32_MAX;
            }
            if (gl_actual_texture_cube[unit] == deleted) {
                gl_actual_texture_cube[unit] = UINT32_MAX;
            }
        }
    }
}

static void gl_forget_deleted_buffers_locked(GLsizei n,
                                              const GLuint *buffers) {
    if (n <= 0 || !buffers) {
        return;
    }

    for (GLsizei deleted_index = 0; deleted_index < n; ++deleted_index) {
        GLuint deleted = buffers[deleted_index];
        if (deleted == 0) {
            continue;
        }
        for (unsigned int i = 0; i < GL_TEXTURE_THREAD_SLOTS; ++i) {
            gl_thread_texture_state *state = &gl_texture_threads[i];
            if (!state->used) {
                continue;
            }
            if (state->array_buffer == deleted) {
                state->array_buffer = 0;
            }
            if (state->element_array_buffer == deleted) {
                state->element_array_buffer = 0;
            }
        }
        if (gl_actual_array_buffer == deleted) {
            gl_actual_array_buffer = UINT32_MAX;
        }
        if (gl_actual_element_array_buffer == deleted) {
            gl_actual_element_array_buffer = UINT32_MAX;
        }
    }
}

static void gl_forget_deleted_framebuffers_locked(
    GLsizei n, const GLuint *framebuffers) {
    if (n <= 0 || !framebuffers) {
        return;
    }

    for (GLsizei deleted_index = 0; deleted_index < n; ++deleted_index) {
        GLuint deleted = framebuffers[deleted_index];
        if (deleted == 0) {
            continue;
        }
        for (unsigned int i = 0; i < GL_TEXTURE_THREAD_SLOTS; ++i) {
            gl_thread_texture_state *state = &gl_texture_threads[i];
            if (state->used && state->framebuffer == deleted) {
                state->framebuffer = 0;
            }
        }
        if (gl_current_framebuffer == deleted) {
            gl_current_framebuffer = UINT32_MAX;
        }
    }
}

enum gl_uniform_location_kind {
    GL_UNIFORM_LOCATION_RAW,
    GL_UNIFORM_LOCATION_VIRTUAL,
    GL_UNIFORM_LOCATION_INVALID,
};

static uint32_t gl_uniform_hash(GLuint program, GLint real_location) {
    uint32_t value = program * 2654435761u;
    value ^= (uint32_t)real_location * 2246822519u;
    return value & GL_UNIFORM_LOCATION_INDEX_MASK;
}

static int gl_find_uniform_record_locked(GLuint program, GLint real_location,
                                         unsigned int array_size) {
    uint32_t slot = gl_uniform_hash(program, real_location);
    int tombstone = -1;

    for (unsigned int probe = 0; probe < GL_UNIFORM_RECORD_SLOTS; ++probe) {
        gl_uniform_location_record *record = &gl_uniform_records[slot];
        if (record->state == 0) {
            if (tombstone >= 0) {
                slot = (uint32_t)tombstone;
                record = &gl_uniform_records[slot];
            }
            record->state = 1;
            record->program = program;
            record->real_location = real_location;
            record->array_size = array_size;
            return (int)slot;
        }
        if (record->state == 1 && record->program == program &&
            record->real_location == real_location) {
            if (array_size > record->array_size) {
                record->array_size = array_size;
            }
            return (int)slot;
        }
        if (record->state == 2 && tombstone < 0) {
            tombstone = (int)slot;
        }
        slot = (slot + 1u) & GL_UNIFORM_LOCATION_INDEX_MASK;
    }

    if (tombstone >= 0) {
        gl_uniform_location_record *record =
            &gl_uniform_records[(unsigned int)tombstone];
        record->state = 1;
        record->program = program;
        record->real_location = real_location;
        record->array_size = array_size;
        return tombstone;
    }

    if (!gl_uniform_record_overflow_reported) {
        l_error("Uniform-location adapter table is full; rejecting new "
                "locations to avoid corrupting VitaGL's uniform records.");
        gl_uniform_record_overflow_reported = true;
    }
    return -1;
}

static void gl_forget_program_uniforms_locked(GLuint program) {
    for (unsigned int i = 0; i < GL_UNIFORM_RECORD_SLOTS; ++i) {
        if (gl_uniform_records[i].state == 1 &&
            gl_uniform_records[i].program == program) {
            gl_uniform_records[i].state = 2;
        }
    }
}

static gl_deferred_program_binary *
gl_find_deferred_program_locked(GLuint program,
                                gl_deferred_program_binary ***link_out) {
    gl_deferred_program_binary **link = &gl_deferred_programs;
    while (*link && (*link)->program != program) {
        link = &(*link)->next;
    }
    if (link_out) {
        *link_out = link;
    }
    return *link;
}

static void gl_forget_deferred_program_locked(GLuint program) {
    gl_deferred_program_binary **link;
    gl_deferred_program_binary *record =
        gl_find_deferred_program_locked(program, &link);
    if (!record) {
        return;
    }
    *link = record->next;
    free(record->data);
    free(record);
    if (gl_deferred_program_count > 0) {
        --gl_deferred_program_count;
    }
}

static bool gl_materialize_program_locked(GLuint program) {
    gl_deferred_program_binary **link;
    gl_deferred_program_binary *record =
        gl_find_deferred_program_locked(program, &link);
    if (!record) {
        return true;
    }

    uint64_t started = sceKernelGetProcessTimeWide();
    gl_forget_program_uniforms_locked(program);
    glProgramBinary(program, record->format, record->data, record->length);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    uint64_t elapsed = sceKernelGetProcessTimeWide() - started;
    gl_materialized_program_us += elapsed;
    unsigned int materialized = ++gl_materialized_program_count;

    if (status != GL_TRUE) {
        l_error("Deferred cached GL program %u (object %u) failed to link "
                "on first use after %llu us.", record->sequence,
                (unsigned int)program, (unsigned long long)elapsed);
    } else if (materialized <= 12 || (materialized & 31u) == 0) {
        l_success("Materialized cached GL program %u (object %u) on first "
                  "use in %llu us; %u cached program(s) remain deferred.",
                  record->sequence, (unsigned int)program,
                  (unsigned long long)elapsed,
                  gl_deferred_program_count - 1);
    }

    *link = record->next;
    free(record->data);
    free(record);
    --gl_deferred_program_count;
    return status == GL_TRUE;
}

static gl_vitagl_uniform_record *
gl_vitagl_uniform_from_location(GLint real_location) {
    uintptr_t pointer = (uintptr_t)(0u - (uint32_t)real_location);
    return (gl_vitagl_uniform_record *)pointer;
}

static enum gl_uniform_location_kind gl_resolve_uniform_location_locked(
    GLint location, GLint *real_location, unsigned int *offset) {
    if (((uint32_t)location & GL_UNIFORM_LOCATION_TAG_MASK) !=
        GL_UNIFORM_LOCATION_TAG) {
        *real_location = location;
        *offset = 0;
        return GL_UNIFORM_LOCATION_RAW;
    }

    unsigned int slot =
        ((uint32_t)location >> GL_UNIFORM_LOCATION_OFFSET_BITS) &
        GL_UNIFORM_LOCATION_INDEX_MASK;
    gl_uniform_location_record *record = &gl_uniform_records[slot];
    unsigned int requested_offset =
        (uint32_t)location & GL_UNIFORM_LOCATION_OFFSET_MASK;
    if (record->state != 1 || requested_offset >= record->array_size) {
        if (!gl_uniform_invalid_location_reported) {
            l_error("Rejected stale or out-of-range virtual GL uniform "
                    "location 0x%08X (slot=%u offset=%u size=%u).",
                    (unsigned int)location, slot, requested_offset,
                    record->state == 1 ? record->array_size : 0);
            gl_uniform_invalid_location_reported = true;
        }
        return GL_UNIFORM_LOCATION_INVALID;
    }

    *real_location = record->real_location;
    *offset = requested_offset;
    return GL_UNIFORM_LOCATION_VIRTUAL;
}

static void gl_upload_uniform_offset_locked(
    GLint real_location, unsigned int offset, GLsizei count,
    unsigned int component_count, const void *value,
    SceGxmParameterType input_type, bool allow_sampler) {
    if (count <= 0 || !value) {
        return;
    }

    gl_vitagl_uniform_record *uniform =
        gl_vitagl_uniform_from_location(real_location);
    if (!uniform || !uniform->parameter) {
        return;
    }
    if (uniform->type != 0) {
        if (allow_sampler) {
            uniform->sampler_index = (uint8_t)*(const GLint *)value;
        }
        return;
    }

    SceGxmParameterType parameter_type =
        sceGxmProgramParameterGetType(uniform->parameter);
    if (uniform->vertex_data) {
        vglSetUniformData(uniform->vertex_data, parameter_type, (int)offset,
                          (uint32_t)count, component_count, value,
                          input_type);
        dirty_shader_vert_unifs = GL_TRUE;
    }
    if (uniform->fragment_data) {
        vglSetUniformData(uniform->fragment_data, parameter_type, (int)offset,
                          (uint32_t)count, component_count, value,
                          input_type);
        dirty_shader_frag_unifs = GL_TRUE;
    }

    unsigned int sequence = ++gl_uniform_offset_count;
    if (sequence <= 16) {
        l_info("Applied GL uniform-array offset %u: real=0x%08X "
               "offset=%u count=%d components=%u.",
               sequence, (unsigned int)real_location, offset, (int)count,
               component_count);
    }
}

static bool gl_parse_uniform_array_name(const GLchar *name,
                                        char *base_name,
                                        size_t base_name_capacity,
                                        unsigned int *offset) {
    const char *open = name ? strrchr(name, '[') : NULL;
    const char *close = open ? strchr(open + 1, ']') : NULL;
    if (!open || !close || close[1] != '\0' || open == name) {
        return false;
    }

    size_t base_length = (size_t)(open - name);
    if (base_length + 1 > base_name_capacity || open + 1 == close) {
        return false;
    }

    unsigned int parsed = 0;
    for (const char *digit = open + 1; digit < close; ++digit) {
        if (*digit < '0' || *digit > '9') {
            return false;
        }
        parsed = parsed * 10u + (unsigned int)(*digit - '0');
        if (parsed > GL_UNIFORM_LOCATION_OFFSET_MASK) {
            return false;
        }
    }

    memcpy(base_name, name, base_length);
    base_name[base_length] = '\0';
    *offset = parsed;
    return true;
}

// Helpers for our handling of shaders
GLboolean skip_next_compile = GL_FALSE;
char next_shader_fname[256];
void load_shader(GLuint shader, const char * string, size_t length);

void gl_preload() {
    if (!file_exists("ur0:/data/libshacccg.suprx")
        && !file_exists("ur0:/data/external/libshacccg.suprx")) {
        fatal_error("Error: libshacccg.suprx is not installed. "
                    "Google \"ShaRKBR33D\" for quick installation.");
    }

#ifdef USE_GLSL_SHADERS
    vglSetSemanticBindingMode(VGL_MODE_POSTPONED);
#endif
}

void gl_init() {
    if (gl_initialized) {
        return;
    }
    int mutex_result =
        sceKernelCreateLwMutex(&gl_api_mutex, "ninjago_gl_api", 0, 0, NULL);
    if (mutex_result < 0) {
        fatal_error("Could not create the VitaGL API mutex: 0x%08X",
                    mutex_result);
    }
    gl_owner_thread = sceKernelGetThreadId();
    atomic_store_explicit(&gl_api_mutex_ready, true, memory_order_release);
    l_success("VitaGL API state serialization initialized.");

    /*
     * The Android renderer requests RGBA8888, depth 24, stencil 8 and no
     * multisampling. Keeping MSAA disabled also preserves GPU memory. A third
     * display/circular-pool buffer lets CPU-side Fusion rendering continue
     * through short GPU spikes without reducing the native 960x544 image.
     */
    vglUseTripleBuffering(GL_TRUE);
    vglInitExtended(0, 960, 544, 8 * 1024 * 1024,
                    SCE_GXM_MULTISAMPLE_NONE);
    l_success("VitaGL initialized at 960x544 with triple buffering and no "
              "MSAA.");
    gl_initialized = true;
}

void gl_swap() {
    static bool first_swap = true;
    static unsigned int submitted_frames;

    if (first_swap) {
        l_info("Submitting first VitaGL display buffer.");
    }
    /*
     * vglSwapBuffers ends the active scene and queues it against VitaGL's
     * display sync objects.  A preceding glFinish serialized the CPU and GPU
     * every frame and cost roughly 2.7-3.4 ms in hardware logs.  Resource
     * mutation and real render-target transitions retain their narrower
     * completion boundaries below.
     */
    gl_api_lock();
    vglSwapBuffers(GL_FALSE);
    atomic_fetch_add_explicit(&gl_frame_sequence, 1,
                              memory_order_release);
    gl_api_unlock();
    ++submitted_frames;

    if (first_swap) {
        l_success("First VitaGL display buffer submitted.");
        first_swap = false;
    } else if (submitted_frames == 2) {
        l_success("Second VitaGL display buffer submitted; render loop is "
                  "still progressing.");
    }
}

void gl_get_performance_counters(GlPerformanceCounters *counters) {
    if (!counters) {
        return;
    }
    gl_api_lock();
    counters->draw_calls = draw_count;
    counters->redundant_state_binds_skipped =
        redundant_state_bind_count;
    counters->main_gl_mutex_contentions = gl_main_mutex_contentions;
    counters->background_gl_mutex_contentions =
        gl_background_mutex_contentions;
    counters->main_gl_mutex_wait_us = gl_main_mutex_wait_us;
    counters->background_gl_mutex_wait_us = gl_background_mutex_wait_us;
    counters->gpu_finish_waits = gl_gpu_finish_wait_count;
    counters->gpu_finish_wait_us = gl_gpu_finish_wait_us;
    counters->framebuffer_async_transitions =
        gl_framebuffer_async_transition_count;
    counters->shader_compiles = shader_compile_count;
    counters->program_links = program_link_count;
    counters->shader_compile_us = gl_shader_compile_us;
    counters->program_link_us = gl_program_link_us;
    counters->duplicate_compile_finishes_skipped =
        gl_duplicate_compile_finish_count;
    counters->compressed_texture_calls = gl_compressed_upload_count;
    counters->compressed_texture_bytes = gl_compressed_upload_bytes;
    counters->compressed_texture_upload_us = gl_compressed_upload_us;
    counters->atc_base_uploads = gl_atc_base_upload_count;
    counters->atc_base_upload_us = gl_atc_base_upload_us;
    counters->cached_program_materializations =
        gl_materialized_program_count;
    counters->cached_program_materialization_us =
        gl_materialized_program_us;
    gl_api_unlock();
}

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glShaderSource<%p>(shader: %i, count: %i, string: %p, length: %p)\n", __builtin_return_address(0), shader, count, string, _length);
#endif
    if (!string) {
        l_error("<%p> Shader source string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    } else if (!*string) {
        l_error("<%p> Shader source *string is NULL, count: %i",
                   __builtin_return_address(0), count);
        skip_next_compile = GL_TRUE;
        return;
    }

    size_t total_length = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            total_length += strlen(string[i]);
        } else {
            total_length += _length[i];
        }
    }

    char * str = malloc(total_length+1);
    size_t l = 0;

    for (int i = 0; i < count; ++i) {
        if (!_length) {
            memcpy(str + l, string[i], strlen(string[i]));
            l += strlen(string[i]);
        } else {
            memcpy(str + l, string[i], _length[i]);
            l += _length[i];
        }
    }
    str[total_length] = '\0';

    gl_api_lock();
    load_shader(shader, str, total_length);
    gl_api_unlock();

    free(str);
}

GLuint glCreateProgram_soloader(void) {
    gl_api_lock();
    GLuint program = glCreateProgram();
    gl_api_unlock();
    return program;
}

GLuint glCreateShader_soloader(GLenum type) {
    gl_api_lock();
    GLuint shader = glCreateShader(type);
    gl_api_unlock();
    return shader;
}

void glAttachShader_soloader(GLuint program, GLuint shader) {
    gl_api_lock();
    glAttachShader(program, shader);
    gl_api_unlock();
}

void glBindAttribLocation_soloader(GLuint program, GLuint index,
                                   const GLchar *name) {
    gl_api_lock();
    glBindAttribLocation(program, index, name);
    gl_api_unlock();
}

void glCompileShader_soloader(GLuint shader) {
#ifdef DEBUG_OPENGL
    sceClibPrintf("[gl_dbg] glCompileShader<%p>(shader: %i)\n", __builtin_return_address(0), shader);
#endif

#ifndef USE_GXP_SHADERS
    if (!skip_next_compile) {
        unsigned int sequence = ++shader_compile_count;
        l_info("Compiling GL shader %u (object %u).", sequence,
               (unsigned int)shader);
        gl_api_lock();
        uint64_t started = sceKernelGetProcessTimeWide();
        /*
         * Shader compilation and program-binary restore both register GXM
         * programs with VitaGL's shared shader patcher. Fusion performs this
         * work on a background thread while the renderer can still have a
         * scene in flight. The API mutex prevents concurrent CPU calls, but
         * it does not make the already-submitted GPU scene idle.
         */
        if (gl_background_finish_required_locked(sceKernelGetThreadId())) {
            gl_wait_for_gpu_locked();
        } else {
            ++gl_duplicate_compile_finish_count;
        }
        glCompileShader(shader);
        GLint status = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (status != GL_TRUE) {
            char message[1024] = {0};
            GLsizei length = 0;
            glGetShaderInfoLog(shader, sizeof(message), &length, message);
            l_error("GL shader %u failed to compile: %s", sequence,
                    length > 0 ? message : "(no compiler message)");
        } else {
            l_success("GL shader %u compiled.", sequence);
        }
#ifdef DUMP_COMPILED_SHADERS
        void *bin = vglMalloc(32 * 1024);
        GLsizei len;
        vglGetShaderBinary(shader, 32 * 1024, &len, bin);
        file_save(next_shader_fname, bin, len);
        vglFree(bin);
#endif
        gl_shader_compile_us += sceKernelGetProcessTimeWide() - started;
        gl_api_unlock();
    }
    skip_next_compile = GL_FALSE;
#endif
}

void glLinkProgram_soloader(GLuint program) {
    unsigned int sequence = ++program_link_count;
    l_info("Linking GL program %u (object %u).", sequence,
           (unsigned int)program);
    gl_api_lock();
    uint64_t started = sceKernelGetProcessTimeWide();
    gl_forget_deferred_program_locked(program);
    gl_forget_program_uniforms_locked(program);
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    uint64_t link_elapsed = sceKernelGetProcessTimeWide() - started;
    if (status != GL_TRUE) {
        char message[1024] = {0};
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof(message), &length, message);
        l_error("GL program %u failed to link: %s", sequence,
                length > 0 ? message : "(no linker message)");
    } else {
        l_success("GL program %u linked in %llu us.", sequence,
                  (unsigned long long)link_elapsed);
    }
    gl_program_link_us += link_elapsed;
    gl_api_unlock();
}

void glGetProgramiv_soloader(GLuint program, GLenum pname, GLint *params) {
    if (pname == GL_PROGRAM_BINARY_LENGTH) {
        l_info("Querying binary length for GL program object %u.",
               (unsigned int)program);
    }
    gl_api_lock();
    gl_deferred_program_binary *deferred =
        gl_find_deferred_program_locked(program, NULL);
    if (deferred && pname == GL_LINK_STATUS) {
        if (params) {
            *params = GL_TRUE;
        }
        gl_api_unlock();
        return;
    }
    gl_materialize_program_locked(program);
    glGetProgramiv(program, pname, params);
    gl_api_unlock();
    if (pname == GL_PROGRAM_BINARY_LENGTH) {
        l_success("GL program object %u reports a %d-byte binary.",
                  (unsigned int)program, params ? (int)*params : -1);
    }
}

void glGetProgramBinary_soloader(GLuint program, GLsizei buf_size,
                                 GLsizei *length, GLenum *binary_format,
                                 void *binary) {
    l_info("Reading GL program object %u binary (capacity %d).",
           (unsigned int)program, (int)buf_size);
    /*
     * VitaGL's program-binary format is internal and its implementation does
     * not write binary_format. Initialize it so Fusion does not persist an
     * uninitialized stack word in the cache header. VitaGL ignores the value
     * again when loading the binary.
     */
    if (binary_format) {
        *binary_format = 0;
    }
    gl_api_lock();
    gl_materialize_program_locked(program);
    glGetProgramBinary(program, buf_size, length, binary_format, binary);
    /* Fusion already writes .glprog files, including the late effect
     * variants. Hash only the actual payload returned to that writer so a
     * copied native cache can be matched without a second cache framework.
     * Bounds and the report cap keep this diagnostic off the steady path. */
    if (gl_native_binary_identity_reports < 24 && binary && length &&
        *length > 0 && *length <= buf_size && *length <= 128 * 1024) {
        NjSha256 hash;
        uint8_t digest[32];
        char hex[65];
        static const char digits[] = "0123456789abcdef";
        nj_sha256_init(&hash);
        nj_sha256_update(&hash, binary, (size_t)*length);
        nj_sha256_final(&hash, digest);
        for (unsigned int i = 0; i < sizeof(digest); ++i) {
            hex[i * 2] = digits[digest[i] >> 4];
            hex[i * 2 + 1] = digits[digest[i] & 15];
        }
        hex[64] = '\0';
        ++gl_native_binary_identity_reports;
        l_info("Native program binary identity: object=%u length=%d "
               "payload-SHA256=%s.", (unsigned int)program, (int)*length,
               hex);
    }
    gl_api_unlock();
    l_success("Read GL program object %u binary: length=%d format=0x%x.",
              (unsigned int)program, length ? (int)*length : -1,
              binary_format ? (unsigned int)*binary_format : 0);
}

void glProgramBinary_soloader(GLuint program, GLenum binary_format,
                              const void *binary, GLsizei length) {
    static unsigned int binary_load_count;
    unsigned int sequence = ++binary_load_count;
    SceUID caller = sceKernelGetThreadId();
    bool report = sequence <= 8 || (sequence & 63u) == 0;
    if (report) {
        l_info("Deferring cached GL program %u (object %u) from thread "
               "0x%08X: length=%d format=0x%x.",
               sequence, (unsigned int)program, caller, (int)length,
               (unsigned int)binary_format);
    }
    gl_api_lock();
    /*
     * Fusion eagerly submits hundreds of distinct cached programs while an
     * arena is loading, even though only a fraction are used by the scene.
     * Copy the small cache blobs now and let glUseProgram or the first uniform
     * query materialize only the working set. This avoids long sequences of
     * GXM shader registrations contending with the loading-screen renderer.
     */
    if (!binary || length <= 0) {
        l_error("Rejected empty cached GL program %u for object %u.",
                sequence, (unsigned int)program);
        gl_api_unlock();
        return;
    }

    void *copy = malloc((size_t)length);
    gl_deferred_program_binary *record =
        (gl_deferred_program_binary *)malloc(sizeof(*record));
    if (!copy || !record) {
        free(copy);
        free(record);
        l_warn("Could not defer cached GL program %u; loading it "
               "synchronously.", sequence);
        gl_forget_program_uniforms_locked(program);
        glProgramBinary(program, binary_format, binary, length);
        gl_api_unlock();
        return;
    }
    memcpy(copy, binary, (size_t)length);
    gl_forget_deferred_program_locked(program);
    record->program = program;
    record->format = binary_format;
    record->length = length;
    record->data = copy;
    record->sequence = sequence;
    record->next = gl_deferred_programs;
    gl_deferred_programs = record;
    ++gl_deferred_program_count;
    gl_api_unlock();
}

GLint glGetUniformLocation_soloader(GLuint program, const GLchar *name) {
    if (!name) {
        return -1;
    }

    char base_name[256];
    unsigned int requested_offset = 0;
    const GLchar *lookup_name = name;
    if (gl_parse_uniform_array_name(name, base_name, sizeof(base_name),
                                    &requested_offset)) {
        lookup_name = base_name;
    }

    gl_api_lock();
    gl_materialize_program_locked(program);
    GLint real_location = glGetUniformLocation(program, lookup_name);
    if (real_location == -1 || real_location == 0) {
        gl_api_unlock();
        return real_location;
    }

    gl_vitagl_uniform_record *uniform =
        gl_vitagl_uniform_from_location(real_location);
    unsigned int array_size = uniform && uniform->parameter
        ? sceGxmProgramParameterGetArraySize(uniform->parameter)
        : 1;
    if (array_size == 0) {
        array_size = 1;
    }
    if (array_size > GL_UNIFORM_LOCATION_OFFSET_MASK + 1u) {
        array_size = GL_UNIFORM_LOCATION_OFFSET_MASK + 1u;
    }
    if (requested_offset >= array_size) {
        gl_api_unlock();
        return -1;
    }

    int slot = gl_find_uniform_record_locked(program, real_location,
                                             array_size);
    GLint result = -1;
    if (slot >= 0) {
        result = (GLint)(GL_UNIFORM_LOCATION_TAG |
                         ((uint32_t)slot <<
                          GL_UNIFORM_LOCATION_OFFSET_BITS) |
                         requested_offset);
    }
    gl_api_unlock();
    return result;
}

void (*eglGetProcAddress_soloader(const char *procname))(void) {
    if (!procname) {
        l_warn("eglGetProcAddress was called with a null name.");
        return NULL;
    }
    if (strcmp(procname, "glGetProgramBinaryOES") == 0 ||
        strcmp(procname, "glGetProgramBinary") == 0) {
        l_info("Providing the GL program-binary read adapter.");
        return (void (*)(void))glGetProgramBinary_soloader;
    }
    if (strcmp(procname, "glProgramBinaryOES") == 0 ||
        strcmp(procname, "glProgramBinary") == 0) {
        l_info("Providing the GL program-binary load adapter.");
        return (void (*)(void))glProgramBinary_soloader;
    }
#define GL_RETURN_ADAPTED_PROC(name)                         \
    if (strcmp(procname, #name) == 0) {                      \
        return (void (*)(void))name##_soloader;              \
    }
    GL_RETURN_ADAPTED_PROC(glGetUniformLocation)
    GL_RETURN_ADAPTED_PROC(glUniform1f)
    GL_RETURN_ADAPTED_PROC(glUniform1fv)
    GL_RETURN_ADAPTED_PROC(glUniform1i)
    GL_RETURN_ADAPTED_PROC(glUniform1iv)
    GL_RETURN_ADAPTED_PROC(glUniform2f)
    GL_RETURN_ADAPTED_PROC(glUniform2fv)
    GL_RETURN_ADAPTED_PROC(glUniform2i)
    GL_RETURN_ADAPTED_PROC(glUniform2iv)
    GL_RETURN_ADAPTED_PROC(glUniform3f)
    GL_RETURN_ADAPTED_PROC(glUniform3fv)
    GL_RETURN_ADAPTED_PROC(glUniform3i)
    GL_RETURN_ADAPTED_PROC(glUniform3iv)
    GL_RETURN_ADAPTED_PROC(glUniform4f)
    GL_RETURN_ADAPTED_PROC(glUniform4fv)
    GL_RETURN_ADAPTED_PROC(glUniform4i)
    GL_RETURN_ADAPTED_PROC(glUniform4iv)
    GL_RETURN_ADAPTED_PROC(glUniformMatrix2fv)
    GL_RETURN_ADAPTED_PROC(glUniformMatrix3fv)
    GL_RETURN_ADAPTED_PROC(glUniformMatrix4fv)
    GL_RETURN_ADAPTED_PROC(glUseProgram)
#undef GL_RETURN_ADAPTED_PROC
    return eglGetProcAddress(procname);
}

void glFinish_soloader(void) {
    gl_api_lock();
    unsigned int sequence = ++explicit_finish_count;
    SceUID caller = sceKernelGetThreadId();
    bool wait_for_gpu = gl_background_finish_required_locked(caller);
    if (!wait_for_gpu) {
        ++gl_coalesced_finish_count;
    }
    bool report = sequence <= 8 ||
                  (caller != gl_owner_thread && (sequence & 511u) == 0);
    if (report) {
        l_info("Explicit GL finish %u %s (coalesced total=%u): "
               "thread=0x%08X; last draw=%u "
               "thread=0x%08X %s mode=0x%x count=%d type=0x%x "
               "indices=%p program=%u framebuffer=%u.",
               sequence, wait_for_gpu ? "waiting" : "coalesced",
               gl_coalesced_finish_count, caller, gl_last_draw.sequence,
               gl_last_draw.thread_id,
               gl_last_draw.indexed ? "elements" : "arrays",
               (unsigned int)gl_last_draw.mode, (int)gl_last_draw.count,
               (unsigned int)gl_last_draw.index_type,
               gl_last_draw.indices, (unsigned int)gl_last_draw.program,
               (unsigned int)gl_last_draw.framebuffer);
    }
    if (wait_for_gpu) {
        gl_wait_for_gpu_locked();
    }
    if (report && wait_for_gpu) {
        l_success("Explicit GL finish %u completed.", sequence);
    }
    gl_api_unlock();
}

void glFlush_soloader(void) {
    gl_api_lock();
    glFlush();
    gl_api_unlock();
}

void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    unsigned int sequence = ++framebuffer_bind_count;
    bool transition = framebuffer != gl_current_framebuffer;
    if (sequence <= 16) {
        l_info("Framebuffer bind %u: target=0x%x object=%p; %s.",
               sequence, (unsigned int)target,
               (void *)(uintptr_t)framebuffer,
               transition
                   ? (target == GL_FRAMEBUFFER && framebuffer == 0
                          ? "queueing the offscreen scene without a CPU wait"
                          : "waiting before render-target reuse")
                   : "same target, no GPU wait");
    }
    /*
     * Fusion changes render targets before setting the next viewport. VitaGL
     * otherwise leaves the previous GXM scene active until a later draw; its
     * glViewport path can then combine that stale scene state with the newly
     * bound framebuffer and dereference a null default-framebuffer pointer.
     *
     * A non-blocking glFlush alone is unsafe before Fusion reuses an offscreen
     * target while SGX is still consuming it. Keep that completion boundary,
     * but do not block when leaving the offscreen target for the display: the
     * two scenes are ordered on the same GXM context, and the next transition
     * back to the offscreen target performs the required reuse wait.
     */
    if (transition) {
        if (target == GL_FRAMEBUFFER) {
            gl_close_framebuffer_transition_locked(framebuffer);
        } else {
            gl_wait_for_gpu_locked();
        }
    }
    if (target == GL_FRAMEBUFFER && !transition) {
        ++redundant_state_bind_count;
    } else {
        glBindFramebuffer(target, framebuffer);
    }
    gl_current_framebuffer = framebuffer;
    if (target == GL_FRAMEBUFFER) {
        state->framebuffer = framebuffer;
    }
    if (sequence <= 16) {
        l_success("Framebuffer bind %u completed.", sequence);
    }
    gl_api_unlock();
}

static SceUID heart_trace_thread = -1;
static unsigned int heart_trace_transition;
static int heart_trace_pending;

void gl_heart_draw_trace_begin(unsigned int transition) {
    gl_api_lock();
    heart_trace_thread = sceKernelGetThreadId();
    heart_trace_transition = transition;
    heart_trace_pending = 1;
    gl_api_unlock();
}
void gl_heart_draw_trace_end(void) {
    gl_api_lock();
    heart_trace_pending = 0;
    heart_trace_thread = -1;
    gl_api_unlock();
}
static void gl_trace_heart_draw_locked(GLsizei count) {
    if (!heart_trace_pending || count <= 0 ||
        sceKernelGetThreadId() != heart_trace_thread) return;
    GLint box[4] = {0};
    glGetIntegerv(GL_SCISSOR_BOX, box);
    l_info("Heart GPU draw: transition=%u framebuffer=%u program=%u "
           "vertices=%d scissor-enabled=%d scissor=[%d %d %d %d].",
           heart_trace_transition, gl_current_framebuffer, gl_current_program,
           count, glIsEnabled(GL_SCISSOR_TEST), box[0], box[1], box[2], box[3]);
    heart_trace_pending = 0;
}

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_framebuffer_locked(state);
    gl_restore_program_locked(state);
    gl_restore_all_textures_locked(state);
    gl_restore_all_buffers_locked(state);
    gl_last_draw.sequence = ++draw_count;
    gl_last_draw.thread_id = sceKernelGetThreadId();
    gl_last_draw.mode = mode;
    gl_last_draw.count = count;
    gl_last_draw.index_type = 0;
    gl_last_draw.indices = (const void *)(uintptr_t)(uint32_t)first;
    gl_last_draw.program = gl_current_program;
    gl_last_draw.framebuffer = gl_current_framebuffer;
    gl_last_draw.indexed = false;
    gl_trace_heart_draw_locked(count);
    glDrawArrays(mode, first, count);
    gl_api_unlock();
}

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type,
                             const void *indices) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_framebuffer_locked(state);
    gl_restore_program_locked(state);
    gl_restore_all_textures_locked(state);
    gl_restore_all_buffers_locked(state);
    gl_last_draw.sequence = ++draw_count;
    gl_last_draw.thread_id = sceKernelGetThreadId();
    gl_last_draw.mode = mode;
    gl_last_draw.count = count;
    gl_last_draw.index_type = type;
    gl_last_draw.indices = indices;
    gl_last_draw.program = gl_current_program;
    gl_last_draw.framebuffer = gl_current_framebuffer;
    gl_last_draw.indexed = true;
    gl_trace_heart_draw_locked(count);
    glDrawElements(mode, count, type, indices);
    gl_api_unlock();
}

#define GL_LOCKED_VOID(name, args, call_args) \
    void name##_soloader args {               \
        gl_api_lock();                         \
        name call_args;                        \
        gl_api_unlock();                       \
    }

#define GL_LOCKED_RET(type, name, args, call_args) \
    type name##_soloader args {                    \
        gl_api_lock();                             \
        type result = name call_args;              \
        gl_api_unlock();                           \
        return result;                             \
    }

/* Fusion enables/disables scissor immediately before drawing Flash clips.
 * These state mutations must share the same lock as scissor and draw calls. */
GL_LOCKED_VOID(glEnable, (GLenum cap), (cap))
GL_LOCKED_VOID(glDisable, (GLenum cap), (cap))

#define GL_RESOLVE_UNIFORM_OR_RETURN(location, real, offset, raw_call)       \
    GLint real = (location);                                                 \
    unsigned int offset = 0;                                                 \
    enum gl_uniform_location_kind location_kind =                           \
        gl_resolve_uniform_location_locked((location), &(real), &(offset));  \
    if (location_kind == GL_UNIFORM_LOCATION_RAW) {                          \
        raw_call;                                                            \
        gl_api_unlock();                                                     \
        return;                                                              \
    }                                                                        \
    if (location_kind == GL_UNIFORM_LOCATION_INVALID) {                      \
        gl_api_unlock();                                                     \
        return;                                                              \
    }

#define GL_DEFINE_UNIFORM1(name, value_type, input_type, allow_sampler)       \
    void name##_soloader(GLint location, value_type v0) {                    \
        value_type values[1] = {v0};                                         \
        gl_api_lock();                                                       \
        GL_RESOLVE_UNIFORM_OR_RETURN(location, real, offset,                 \
                                     name(location, v0));                    \
        if (offset == 0) {                                                   \
            name(real, v0);                                                  \
        } else {                                                             \
            gl_upload_uniform_offset_locked(real, offset, 1, 1, values,      \
                                            input_type, allow_sampler);       \
        }                                                                    \
        gl_api_unlock();                                                     \
    }

#define GL_DEFINE_UNIFORM2(name, value_type, input_type)                     \
    void name##_soloader(GLint location, value_type v0, value_type v1) {     \
        value_type values[2] = {v0, v1};                                     \
        gl_api_lock();                                                       \
        GL_RESOLVE_UNIFORM_OR_RETURN(location, real, offset,                 \
                                     name(location, v0, v1));                \
        if (offset == 0) {                                                   \
            name(real, v0, v1);                                              \
        } else {                                                             \
            gl_upload_uniform_offset_locked(real, offset, 1, 2, values,      \
                                            input_type, false);               \
        }                                                                    \
        gl_api_unlock();                                                     \
    }

#define GL_DEFINE_UNIFORM3(name, value_type, input_type)                     \
    void name##_soloader(GLint location, value_type v0, value_type v1,       \
                         value_type v2) {                                    \
        value_type values[3] = {v0, v1, v2};                                 \
        gl_api_lock();                                                       \
        GL_RESOLVE_UNIFORM_OR_RETURN(location, real, offset,                 \
                                     name(location, v0, v1, v2));            \
        if (offset == 0) {                                                   \
            name(real, v0, v1, v2);                                          \
        } else {                                                             \
            gl_upload_uniform_offset_locked(real, offset, 1, 3, values,      \
                                            input_type, false);               \
        }                                                                    \
        gl_api_unlock();                                                     \
    }

#define GL_DEFINE_UNIFORM4(name, value_type, input_type)                     \
    void name##_soloader(GLint location, value_type v0, value_type v1,       \
                         value_type v2, value_type v3) {                     \
        value_type values[4] = {v0, v1, v2, v3};                             \
        gl_api_lock();                                                       \
        GL_RESOLVE_UNIFORM_OR_RETURN(location, real, offset,                 \
                                     name(location, v0, v1, v2, v3));        \
        if (offset == 0) {                                                   \
            name(real, v0, v1, v2, v3);                                      \
        } else {                                                             \
            gl_upload_uniform_offset_locked(real, offset, 1, 4, values,      \
                                            input_type, false);               \
        }                                                                    \
        gl_api_unlock();                                                     \
    }

#define GL_DEFINE_UNIFORM_ARRAY(name, value_type, input_type, components,    \
                                allow_sampler)                               \
    void name##_soloader(GLint location, GLsizei count,                      \
                         const value_type *value) {                          \
        gl_api_lock();                                                       \
        GL_RESOLVE_UNIFORM_OR_RETURN(location, real, offset,                 \
                                     name(location, count, value));          \
        if (offset == 0) {                                                   \
            name(real, count, value);                                        \
        } else {                                                             \
            gl_upload_uniform_offset_locked(real, offset, count, components, \
                                            value, input_type,                \
                                            allow_sampler);                   \
        }                                                                    \
        gl_api_unlock();                                                     \
    }

GL_DEFINE_UNIFORM1(glUniform1f, GLfloat, SCE_GXM_PARAMETER_TYPE_F32, false)
GL_DEFINE_UNIFORM1(glUniform1i, GLint, SCE_GXM_PARAMETER_TYPE_S32, true)
GL_DEFINE_UNIFORM2(glUniform2f, GLfloat, SCE_GXM_PARAMETER_TYPE_F32)
GL_DEFINE_UNIFORM2(glUniform2i, GLint, SCE_GXM_PARAMETER_TYPE_S32)
GL_DEFINE_UNIFORM3(glUniform3f, GLfloat, SCE_GXM_PARAMETER_TYPE_F32)
GL_DEFINE_UNIFORM3(glUniform3i, GLint, SCE_GXM_PARAMETER_TYPE_S32)
GL_DEFINE_UNIFORM4(glUniform4f, GLfloat, SCE_GXM_PARAMETER_TYPE_F32)
GL_DEFINE_UNIFORM4(glUniform4i, GLint, SCE_GXM_PARAMETER_TYPE_S32)

GL_DEFINE_UNIFORM_ARRAY(glUniform1fv, GLfloat,
                        SCE_GXM_PARAMETER_TYPE_F32, 1, false)
GL_DEFINE_UNIFORM_ARRAY(glUniform1iv, GLint,
                        SCE_GXM_PARAMETER_TYPE_S32, 1, true)
GL_DEFINE_UNIFORM_ARRAY(glUniform2fv, GLfloat,
                        SCE_GXM_PARAMETER_TYPE_F32, 2, false)
GL_DEFINE_UNIFORM_ARRAY(glUniform2iv, GLint,
                        SCE_GXM_PARAMETER_TYPE_S32, 2, false)
GL_DEFINE_UNIFORM_ARRAY(glUniform3fv, GLfloat,
                        SCE_GXM_PARAMETER_TYPE_F32, 3, false)
GL_DEFINE_UNIFORM_ARRAY(glUniform3iv, GLint,
                        SCE_GXM_PARAMETER_TYPE_S32, 3, false)
GL_DEFINE_UNIFORM_ARRAY(glUniform4fv, GLfloat,
                        SCE_GXM_PARAMETER_TYPE_F32, 4, false)
GL_DEFINE_UNIFORM_ARRAY(glUniform4iv, GLint,
                        SCE_GXM_PARAMETER_TYPE_S32, 4, false)

#define GL_DEFINE_UNIFORM_MATRIX(name, dimension)                            \
    void name##_soloader(GLint location, GLsizei count,                      \
                         GLboolean transpose, const GLfloat *value) {        \
        gl_api_lock();                                                       \
        GL_RESOLVE_UNIFORM_OR_RETURN(                                        \
            location, real, offset,                                          \
            name(location, count, transpose, value));                        \
        if (offset == 0) {                                                   \
            name(real, count, transpose, value);                             \
            gl_api_unlock();                                                 \
            return;                                                          \
        }                                                                    \
        const GLfloat *source = value;                                       \
        GLfloat *transposed = NULL;                                          \
        if (transpose && count > 0 && value) {                               \
            size_t matrix_values = (size_t)(dimension) * (dimension);        \
            transposed = malloc((size_t)count * matrix_values *              \
                                sizeof(*transposed));                         \
            if (!transposed) {                                               \
                l_error("Could not transpose an offset GL matrix uniform.");\
                gl_api_unlock();                                             \
                return;                                                      \
            }                                                                \
            for (GLsizei matrix = 0; matrix < count; ++matrix) {             \
                for (unsigned int row = 0; row < (dimension); ++row) {       \
                    for (unsigned int column = 0; column < (dimension);      \
                         ++column) {                                         \
                        transposed[(size_t)matrix * matrix_values +          \
                                   column * (dimension) + row] =             \
                            value[(size_t)matrix * matrix_values +           \
                                  row * (dimension) + column];               \
                    }                                                        \
                }                                                            \
            }                                                                \
            source = transposed;                                             \
        }                                                                    \
        gl_upload_uniform_offset_locked(                                     \
            real, offset, count * (dimension), (dimension), source,          \
            SCE_GXM_PARAMETER_TYPE_F32, false);                              \
        free(transposed);                                                     \
        gl_api_unlock();                                                     \
    }

GL_DEFINE_UNIFORM_MATRIX(glUniformMatrix2fv, 2)
GL_DEFINE_UNIFORM_MATRIX(glUniformMatrix3fv, 3)
GL_DEFINE_UNIFORM_MATRIX(glUniformMatrix4fv, 4)

/*
 * Resource registration and destruction run on Fusion's background worker.
 * Keep these calls in the same critical section as scene submission so the
 * worker cannot delete an active FBO/VBO/texture or alter a binding while a
 * draw is consuming it.
 */
void glActiveTexture_soloader(GLenum texture) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    bool tracked = gl_texture_unit_index(texture) >= 0;
    if (tracked && gl_actual_active_texture == texture) {
        ++redundant_state_bind_count;
    } else {
        glActiveTexture(texture);
    }
    if (tracked) {
        state->active_texture = texture;
        gl_actual_active_texture = texture;
    }
    gl_api_unlock();
}

void glBindBuffer_soloader(GLenum target, GLuint buffer) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    GLuint *thread_binding = gl_thread_buffer_binding(state, target);
    GLuint *actual_binding = gl_actual_buffer_binding(target);
    if (actual_binding && *actual_binding == buffer) {
        ++redundant_state_bind_count;
    } else {
        glBindBuffer(target, buffer);
    }
    if (thread_binding && actual_binding) {
        *thread_binding = buffer;
        *actual_binding = buffer;
    }
    gl_api_unlock();
}

GL_LOCKED_VOID(glBindRenderbuffer,
               (GLenum target, GLuint renderbuffer),
               (target, renderbuffer))

void glBindTexture_soloader(GLenum target, GLuint texture) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_active_texture_locked(state);
    int unit = gl_texture_unit_index(state->active_texture);
    int target_index = gl_texture_target_index(target);
    GLuint *actual_binding = unit >= 0 && target_index >= 0
        ? gl_actual_texture_binding(target_index, unit) : NULL;
    bool canonical_target = target_index >= 0 &&
        target == gl_texture_binding_target(target_index);
    if (actual_binding && canonical_target &&
        *actual_binding == texture) {
        ++redundant_state_bind_count;
    } else {
        glBindTexture(target, texture);
    }
    if (unit >= 0 && target_index >= 0) {
        *gl_thread_texture_binding(state, target_index, unit) = texture;
        *actual_binding = texture;
        gl_get_texture_record_locked(texture);
    }
    gl_api_unlock();
}

void glBufferData_soloader(GLenum target, GLsizeiptr size,
                           const GLvoid *data, GLenum usage) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_buffer_target_locked(state, target);
    glBufferData(target, size, data, usage);
    gl_api_unlock();
}

void glBufferSubData_soloader(GLenum target, GLintptr offset,
                              GLsizeiptr size, const GLvoid *data) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_buffer_target_locked(state, target);
    glBufferSubData(target, offset, size, data);
    gl_api_unlock();
}
GL_LOCKED_RET(GLenum, glCheckFramebufferStatus,
              (GLenum target), (target))
GL_LOCKED_VOID(glClear, (GLbitfield mask), (mask))

void glCompressedTexImage2D_soloader(
    GLenum target, GLint level, GLenum internalformat, GLsizei width,
    GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_texture_target_locked(state, target);

    GLuint texture = gl_current_texture_locked(state, target);
    gl_texture_record *record =
        gl_get_texture_record_locked(texture);
    unsigned int sequence = ++gl_compressed_upload_count;
    if (imageSize > 0) {
        gl_compressed_upload_bytes += (uint64_t)imageSize;
    }
    if (sequence <= 48) {
        l_info("Compressed texture upload %u: thread=0x%08X object=%p "
               "target=0x%x level=%d format=0x%x size=%dx%d bytes=%d "
               "data=%p.",
               sequence, state->thread_id, (void *)(uintptr_t)texture,
               (unsigned int)target, (int)level,
               (unsigned int)internalformat, (int)width, (int)height,
               (int)imageSize, data);
    }

    bool is_atc = internalformat == GL_ATC_RGB_AMD ||
                  internalformat == GL_ATC_RGBA_EXPLICIT_ALPHA_AMD ||
                  internalformat == GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD;

    /*
     * VitaGL software-decodes ATC into a temporary RGBA allocation. For a
     * nonzero mip it then regenerates mip data from the base texture rather
     * than consuming the supplied decoded pixels. Fusion submits every mip
     * followed by the base a second time, multiplying decode/alloc/free work.
     * The Chen's Island crash was observed in the temporary buffer's free
     * path, so removing that redundant allocator churn is the narrowest fix.
     *
     * Decode the base once, let VitaGL generate the complete chain once, skip
     * the redundant supplied mips, then consume Fusion's final duplicate base
     * as an end-of-chain marker. A later upload to the same texture can start
     * a fresh chain normally.
     */
    if (record && is_atc && record->generated_atc_mips && level > 0) {
        record->skipped_atc_mip = true;
        unsigned int skipped = ++gl_atc_mip_skip_count;
        if (skipped <= 24 || (skipped & 255u) == 0) {
            l_info("Skipped redundant ATC mip %u: object=%p level=%d "
                   "size=%dx%d.",
                   skipped, (void *)(uintptr_t)texture, (int)level,
                   (int)width, (int)height);
        }
        gl_api_unlock();
        return;
    }
    if (record && is_atc && record->generated_atc_mips &&
        record->skipped_atc_mip && level == 0 &&
        record->atc_format == internalformat &&
        record->base_width == width && record->base_height == height) {
        record->generated_atc_mips = false;
        record->skipped_atc_mip = false;
        gl_api_unlock();
        return;
    }

    /*
     * VitaGL's software-decoded ATC path assumes level zero already owns a
     * backing allocation. If a future call reaches us without that base,
     * treating the supplied mip as a temporary base is both safe and visible;
     * passing it through would dereference tex->data == NULL in
     * gpu_alloc_mipmaps().
     */
    bool orphan_atc_mip =
        level > 0 && record && !record->base_defined &&
        is_atc;
    uint64_t upload_started = sceKernelGetProcessTimeWide();
    if (orphan_atc_mip) {
        l_warn("Recovering orphan ATC mip for texture %p: level=%d "
               "size=%dx%d. Uploading it as a temporary base.",
               (void *)(uintptr_t)texture, (int)level, (int)width,
               (int)height);
        glCompressedTexImage2D(target, 0, internalformat, width, height,
                               border, imageSize, data);
        record->base_defined = true;
        if (target == GL_TEXTURE_2D && width > 1 && height > 1) {
            glGenerateMipmap(target);
            record->generated_atc_mips = true;
            record->skipped_atc_mip = false;
            record->atc_format = internalformat;
            record->base_width = width;
            record->base_height = height;
        }
    } else {
        glCompressedTexImage2D(target, level, internalformat, width, height,
                               border, imageSize, data);
        if (record && level == 0) {
            record->base_defined = true;
            record->generated_atc_mips = false;
            record->skipped_atc_mip = false;
            if (is_atc && target == GL_TEXTURE_2D && width > 1 &&
                height > 1) {
                glGenerateMipmap(target);
                record->generated_atc_mips = true;
                record->atc_format = internalformat;
                record->base_width = width;
                record->base_height = height;
            }
        }
    }
    uint64_t upload_elapsed =
        sceKernelGetProcessTimeWide() - upload_started;
    gl_compressed_upload_us += upload_elapsed;
    if (is_atc && (level == 0 || orphan_atc_mip)) {
        ++gl_atc_base_upload_count;
        gl_atc_base_upload_us += upload_elapsed;
    }
    gl_api_unlock();
}

void glCopyTexImage2D_soloader(
    GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
    GLsizei width, GLsizei height, GLint border) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_texture_target_locked(state, target);
    glCopyTexImage2D(target, level, internalformat, x, y, width, height,
                     border);
    if (level == 0) {
        gl_texture_record *record = gl_get_texture_record_locked(
            gl_current_texture_locked(state, target));
        if (record) {
            record->base_defined = true;
            record->generated_atc_mips = false;
            record->skipped_atc_mip = false;
        }
    }
    gl_api_unlock();
}

void glCopyTexSubImage2D_soloader(
    GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x,
    GLint y, GLsizei width, GLsizei height) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_texture_target_locked(state, target);
    glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width,
                        height);
    gl_api_unlock();
}

void glDeleteBuffers_soloader(GLsizei n, const GLuint *buffers) {
    gl_api_lock();
    glDeleteBuffers(n, buffers);
    gl_forget_deleted_buffers_locked(n, buffers);
    gl_api_unlock();
}
void glDeleteFramebuffers_soloader(GLsizei n,
                                   const GLuint *framebuffers) {
    gl_api_lock();
    glDeleteFramebuffers(n, framebuffers);
    gl_forget_deleted_framebuffers_locked(n, framebuffers);
    gl_api_unlock();
}

void glDeleteProgram_soloader(GLuint program) {
    gl_api_lock();
    gl_forget_deferred_program_locked(program);
    gl_forget_program_uniforms_locked(program);
    glDeleteProgram(program);
    for (unsigned int i = 0; i < GL_TEXTURE_THREAD_SLOTS; ++i) {
        if (gl_texture_threads[i].used &&
            gl_texture_threads[i].program == program) {
            gl_texture_threads[i].program = 0;
        }
    }
    if (gl_current_program == program) {
        gl_current_program = UINT32_MAX;
    }
    gl_api_unlock();
}

GL_LOCKED_VOID(glDeleteRenderbuffers,
               (GLsizei n, const GLuint *renderbuffers), (n, renderbuffers))
GL_LOCKED_VOID(glDeleteShader, (GLuint shader), (shader))

void glDeleteTextures_soloader(GLsizei n, const GLuint *textures) {
    gl_api_lock();
    if (n > 0 && textures && sceKernelGetThreadId() != gl_owner_thread) {
        unsigned int event = ++gl_background_texture_delete_count;
        if (event <= 16 || event % 64 == 0) {
            l_info("Background texture delete %u: count=%d first=%u.",
                   event, (int)n, (unsigned int)textures[0]);
        }
    }
    glDeleteTextures(n, textures);
    gl_forget_deleted_textures_locked(n, textures);
    gl_api_unlock();
}

GL_LOCKED_VOID(glFramebufferRenderbuffer,
               (GLenum target, GLenum attachment, GLenum renderbuffertarget,
                GLuint renderbuffer),
               (target, attachment, renderbuffertarget, renderbuffer))
GL_LOCKED_VOID(glFramebufferTexture2D,
               (GLenum target, GLenum attachment, GLenum textarget,
                GLuint texture, GLint level),
               (target, attachment, textarget, texture, level))
GL_LOCKED_VOID(glGenBuffers, (GLsizei n, GLuint *buffers), (n, buffers))

void glGenerateMipmap_soloader(GLenum target) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_texture_target_locked(state, target);
    glGenerateMipmap(target);
    gl_api_unlock();
}

GL_LOCKED_VOID(glGenFramebuffers,
               (GLsizei n, GLuint *framebuffers), (n, framebuffers))
GL_LOCKED_VOID(glGenRenderbuffers,
               (GLsizei n, GLuint *renderbuffers), (n, renderbuffers))
GL_LOCKED_VOID(glGenTextures, (GLsizei n, GLuint *textures), (n, textures))

void glUseProgram_soloader(GLuint program) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_materialize_program_locked(program);
    /* VitaGL uses glUseProgram to dirty its uniform upload state. */
    glUseProgram(program);
    state->program = program;
    gl_current_program = program;
    gl_api_unlock();
}

void glGetBufferParameteriv_soloader(GLenum target, GLenum pname,
                                     GLint *params) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_buffer_target_locked(state, target);
    glGetBufferParameteriv(target, pname, params);
    gl_api_unlock();
}
GL_LOCKED_VOID(glReadPixels,
               (GLint x, GLint y, GLsizei width, GLsizei height,
                GLenum format, GLenum type, GLvoid *pixels),
               (x, y, width, height, format, type, pixels))
GL_LOCKED_VOID(glRenderbufferStorage,
               (GLenum target, GLenum internalformat, GLsizei width,
                GLsizei height),
               (target, internalformat, width, height))
GL_LOCKED_VOID(glScissor,
               (GLint x, GLint y, GLsizei width, GLsizei height),
               (x, y, width, height))

void glTexImage2D_soloader(
    GLenum target, GLint level, GLint internalformat, GLsizei width,
    GLsizei height, GLint border, GLenum format, GLenum type,
    const GLvoid *pixels) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_texture_target_locked(state, target);
    glTexImage2D(target, level, internalformat, width, height, border,
                 format, type, pixels);
    if (level == 0) {
        gl_texture_record *record = gl_get_texture_record_locked(
            gl_current_texture_locked(state, target));
        if (record) {
            record->base_defined = true;
            record->generated_atc_mips = false;
            record->skipped_atc_mip = false;
        }
    }
    gl_api_unlock();
}

void glTexParameterf_soloader(GLenum target, GLenum pname, GLfloat param) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_texture_target_locked(state, target);
    glTexParameterf(target, pname, param);
    gl_api_unlock();
}

void glTexParameteri_soloader(GLenum target, GLenum pname, GLint param) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_texture_target_locked(state, target);
    glTexParameteri(target, pname, param);
    gl_api_unlock();
}

void glTexParameteriv_soloader(GLenum target, GLenum pname,
                               const GLint *params) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_texture_target_locked(state, target);
    glTexParameteriv(target, pname, (GLint *)params);
    gl_api_unlock();
}

void glTexParameterx_soloader(GLenum target, GLenum pname, GLfixed param) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_texture_target_locked(state, target);
    glTexParameterx(target, pname, param);
    gl_api_unlock();
}

void glTexSubImage2D_soloader(
    GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_texture_target_locked(state, target);
    glTexSubImage2D(target, level, xoffset, yoffset, width, height, format,
                    type, pixels);
    gl_api_unlock();
}

void glDisableVertexAttribArray_soloader(GLuint index) {
    gl_api_lock();
    glDisableVertexAttribArray(index);
    gl_api_unlock();
}

void glEnableVertexAttribArray_soloader(GLuint index) {
    gl_api_lock();
    glEnableVertexAttribArray(index);
    gl_api_unlock();
}

void glVertexAttribPointer_soloader(
    GLuint index, GLint size, GLenum type, GLboolean normalized,
    GLsizei stride, const void *pointer) {
    gl_api_lock();
    gl_thread_texture_state *state =
        gl_get_thread_texture_state_locked();
    gl_restore_buffer_target_locked(state, GL_ARRAY_BUFFER);
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
    gl_api_unlock();
}

GL_LOCKED_VOID(glViewport,
               (GLint x, GLint y, GLsizei width, GLsizei height),
               (x, y, width, height))

#undef GL_LOCKED_RET
#undef GL_LOCKED_VOID

#if defined(USE_GLSL_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else {
        glShaderSource(shader, 1, &string, &length);
        strcpy(next_shader_fname, gxp_path);
    }

    free(sha_name);
}
#elif defined(USE_GLSL_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    glShaderSource(shader, 1, &string, &length);
}
#elif defined(USE_CG_SHADERS) && defined(DUMP_COMPILED_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char gxp_path[256];
    char cg_path[256];
    snprintf(gxp_path, sizeof(gxp_path), DATA_PATH"gxp/%s.gxp", sha_name);
    snprintf(cg_path, sizeof(cg_path), DATA_PATH"cg/%s.cg", sha_name);

    if (file_exists(gxp_path)) {
        uint8_t *buffer;
        size_t size;

        file_load(gxp_path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
        skip_next_compile = GL_TRUE;
    } else if (file_exists(cg_path)) {
        char *buffer;
        size_t size;

        file_load(cg_path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);
        strcpy(next_shader_fname, gxp_path);

        free(buffer);
        skip_next_compile = GL_FALSE;
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }

        skip_next_compile = GL_FALSE;
    }

    free(sha_name);
}
#elif defined(USE_CG_SHADERS) || defined(USE_GXP_SHADERS)
void load_shader(GLuint shader, const char * string, size_t length) {
    char* sha_name = str_sha1sum(string, length);

    char path[256];
#ifdef USE_CG_SHADERS
    snprintf(path, sizeof(path), DATA_PATH"cg/%s.cg", sha_name);
#else
    snprintf(path, sizeof(path), DATA_PATH"gxp/%s.gxp", sha_name);
#endif

    if (file_exists(path)) {
#ifdef USE_CG_SHADERS
        char *buffer;
        size_t size;

        file_load(path, (uint8_t **) &buffer, &size);

        glShaderSource(shader, 1, &string, &size);

        free(buffer);
#else
        uint8_t *buffer;
        size_t size;

        file_load(path, &buffer, &size);

        glShaderBinary(1, &shader, 0, buffer, (int32_t) size);

        free(buffer);
#endif
    } else {
        l_warn("Encountered an untranslated shader %s, saving GLSL "
               "and using a dummy shader.", sha_name);

        char glsl_path[256];
        snprintf(glsl_path, sizeof(glsl_path), DATA_PATH"glsl/%s.glsl", sha_name);
        file_mkpath(glsl_path, 0777);
        file_save(glsl_path, (const uint8_t *) string, length);

        if (strstr(string, "gl_FragColor")) {
            const char *dummy_shader = "float4 main() { return float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        } else {
            const char *dummy_shader = "void main(float4 out gl_Position : POSITION ) { gl_Position = float4(1.0,1.0,1.0,1.0); }";
            int32_t dummy_shader_len = (int32_t) strlen(dummy_shader);
            glShaderSource(shader, 1, &dummy_shader, &dummy_shader_len);
        }
    }

    free(sha_name);
}
#else
#error "Define one of (USE_GLSL_SHADERS, USE_CG_SHADERS, USE_GXP_SHADERS)"
#endif
