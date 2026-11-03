/*
 * This file is part of vitaGL
 * Copyright 2026 vitaGL contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * vgl_optimizer.c:
 * Runtime GXM state caching and performance counters
 */

#define VGL_OPTIMIZER_IMPLEMENTATION
#include "shared.h"
#include "vgl_optimizer.h"
#include <string.h>

#define VGL_OPT_VALID_VERTEX_PROGRAM (1ULL << 0)
#define VGL_OPT_VALID_FRAGMENT_PROGRAM (1ULL << 1)
#define VGL_OPT_VALID_VERTEX_DEFAULT_BUFFER (1ULL << 2)
#define VGL_OPT_VALID_FRAGMENT_DEFAULT_BUFFER (1ULL << 3)
#define VGL_OPT_VALID_VIEWPORT (1ULL << 4)
#define VGL_OPT_VALID_REGION_CLIP (1ULL << 5)
#define VGL_OPT_VALID_CULL_MODE (1ULL << 6)
#define VGL_OPT_VALID_FRONT_DEPTH_FUNC (1ULL << 7)
#define VGL_OPT_VALID_BACK_DEPTH_FUNC (1ULL << 8)
#define VGL_OPT_VALID_FRONT_DEPTH_WRITE (1ULL << 9)
#define VGL_OPT_VALID_BACK_DEPTH_WRITE (1ULL << 10)
#define VGL_OPT_VALID_FRONT_POLYGON_MODE (1ULL << 11)
#define VGL_OPT_VALID_BACK_POLYGON_MODE (1ULL << 12)
#define VGL_OPT_VALID_TWO_SIDED (1ULL << 13)
#define VGL_OPT_VALID_FRONT_STENCIL_REF (1ULL << 14)
#define VGL_OPT_VALID_BACK_STENCIL_REF (1ULL << 15)
#define VGL_OPT_VALID_FRONT_STENCIL_FUNC (1ULL << 16)
#define VGL_OPT_VALID_BACK_STENCIL_FUNC (1ULL << 17)
#define VGL_OPT_VALID_FRONT_FRAGMENT_ENABLE (1ULL << 18)
#define VGL_OPT_VALID_BACK_FRAGMENT_ENABLE (1ULL << 19)
#define VGL_OPT_VALID_FRONT_LINE_FILL (1ULL << 20)
#define VGL_OPT_VALID_BACK_LINE_FILL (1ULL << 21)
#define VGL_OPT_VALID_FRONT_POINT_LINE_WIDTH (1ULL << 22)
#define VGL_OPT_VALID_BACK_POINT_LINE_WIDTH (1ULL << 23)
#define VGL_OPT_VALID_FRONT_DEPTH_BIAS (1ULL << 24)
#define VGL_OPT_VALID_BACK_DEPTH_BIAS (1ULL << 25)
#define VGL_OPT_VALID_W_CLAMP_VALUE (1ULL << 26)
#define VGL_OPT_VALID_W_CLAMP_ENABLE (1ULL << 27)
#define VGL_OPT_VALID_VIEWPORT_ENABLE (1ULL << 28)
#define VGL_OPT_VALID_W_BUFFER_ENABLE (1ULL << 29)
#define VGL_OPT_VALID_FRONT_VISIBILITY_INDEX (1ULL << 30)
#define VGL_OPT_VALID_BACK_VISIBILITY_INDEX (1ULL << 31)
#define VGL_OPT_VALID_FRONT_VISIBILITY_OP (1ULL << 32)
#define VGL_OPT_VALID_BACK_VISIBILITY_OP (1ULL << 33)
#define VGL_OPT_VALID_FRONT_VISIBILITY_ENABLE (1ULL << 34)
#define VGL_OPT_VALID_BACK_VISIBILITY_ENABLE (1ULL << 35)
#define VGL_OPT_VALID_PRECOMPUTED_VERTEX_STATE (1ULL << 36)
#define VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE (1ULL << 37)
#define VGL_OPT_VALID_PREPARED_VERTEX_BINDING (1ULL << 38)

typedef struct {
	GLint location;
	uint16_t size;
	uint16_t type;
	GLboolean valid;
	uint8_t data[VGL_OPT_UNIFORM_DATA_SIZE];
} vgl_uniform_cache_entry;

#ifdef HAVE_RUNTIME_OPTIMIZER_STATS
typedef struct {
	uintptr_t vertex_program;
	uintptr_t fragment_program;
	uint32_t stamp;
	GLboolean valid;
} vgl_pipeline_cache_entry;
#endif

typedef struct {
	SceGxmStencilFunc func;
	SceGxmStencilOp stencil_fail;
	SceGxmStencilOp depth_fail;
	SceGxmStencilOp depth_pass;
	unsigned char compare_mask;
	unsigned char write_mask;
} vgl_stencil_state;

typedef struct {
	int factor;
	int units;
} vgl_depth_bias_state;

typedef struct {
	SceGxmRegionClipMode mode;
	unsigned int x_min;
	unsigned int y_min;
	unsigned int x_max;
	unsigned int y_max;
} vgl_region_clip_state;

typedef struct {
	SceGxmContext *context;
	uint64_t valid;
	uint32_t vertex_stream_valid;
	uint32_t fragment_texture_valid;
	uint32_t vertex_texture_valid;
	uint32_t vertex_uniform_valid;
	uint32_t fragment_uniform_valid;
	const SceGxmVertexProgram *vertex_program;
	const SceGxmFragmentProgram *fragment_program;
	const SceGxmPrecomputedVertexState *precomputed_vertex_state;
	const SceGxmPrecomputedFragmentState *precomputed_fragment_state;
	const SceGxmVertexProgram *prepared_vertex_program;
	const void *prepared_vertex_stream;
	uint32_t prepared_vertex_stream_count;
	const void *vertex_stream[VGL_OPT_MAX_STREAMS];
	SceGxmTexture fragment_texture[VGL_OPT_MAX_TEXTURES];
	SceGxmTexture vertex_texture[VGL_OPT_MAX_TEXTURES];
	const void *vertex_uniform[VGL_OPT_MAX_UNIFORM_BUFFERS];
	const void *fragment_uniform[VGL_OPT_MAX_UNIFORM_BUFFERS];
	const void *vertex_default_uniform;
	const void *fragment_default_uniform;
	float viewport[6];
	vgl_region_clip_state region_clip;
	SceGxmCullMode cull_mode;
	SceGxmDepthFunc front_depth_func;
	SceGxmDepthFunc back_depth_func;
	SceGxmDepthWriteMode front_depth_write;
	SceGxmDepthWriteMode back_depth_write;
	SceGxmPolygonMode front_polygon_mode;
	SceGxmPolygonMode back_polygon_mode;
	SceGxmTwoSidedMode two_sided;
	SceGxmFragmentProgramMode front_fragment_enable;
	SceGxmFragmentProgramMode back_fragment_enable;
	SceGxmLineFillLastPixelMode front_line_fill;
	SceGxmLineFillLastPixelMode back_line_fill;
	unsigned int front_point_line_width;
	unsigned int back_point_line_width;
	vgl_depth_bias_state front_depth_bias;
	vgl_depth_bias_state back_depth_bias;
	float w_clamp_value;
	SceGxmWClampMode w_clamp_enable;
	SceGxmViewportMode viewport_enable;
	SceGxmWBufferMode w_buffer_enable;
	unsigned int front_visibility_index;
	unsigned int back_visibility_index;
	SceGxmVisibilityTestOp front_visibility_op;
	SceGxmVisibilityTestOp back_visibility_op;
	SceGxmVisibilityTestMode front_visibility_enable;
	SceGxmVisibilityTestMode back_visibility_enable;
	unsigned int front_stencil_ref;
	unsigned int back_stencil_ref;
	vgl_stencil_state front_stencil_func;
	vgl_stencil_state back_stencil_func;
} vgl_gxm_state_cache;

static vgl_gxm_state_cache gxm_state_cache;
static vgl_uniform_cache_entry uniform_cache[VGL_OPT_UNIFORM_CACHE_SIZE];
#ifdef HAVE_RUNTIME_OPTIMIZER_STATS
static vgl_pipeline_cache_entry pipeline_cache[VGL_OPT_PIPELINE_CACHE_SIZE];
#endif
static vglPerfCounters current_counters;
static vglPerfCounters last_counters;
static GLuint uniform_program = 0;
#ifdef HAVE_RUNTIME_OPTIMIZER_STATS
static uint32_t pipeline_stamp = 0;
#endif
#ifdef HAVE_RUNTIME_OPTIMIZER
GLboolean optimizer_enabled = GL_TRUE;
#else
GLboolean optimizer_enabled = GL_FALSE;
#endif
#if defined(HAVE_RUNTIME_OPTIMIZER) && !defined(HAVE_RUNTIME_OPTIMIZER_STATS)
vgl_fast_gxm_state vgl_fast_state;
#endif

#ifdef HAVE_RUNTIME_OPTIMIZER_STATS
#define VGL_COUNTER_INC(field) (current_counters.field++)
#define VGL_COUNTER_ADD(field, value) (current_counters.field += (value))
#define VGL_COUNTER_SET(field, value) (current_counters.field = (value))
#else
#define VGL_COUNTER_INC(field) ((void)0)
#define VGL_COUNTER_ADD(field, value) ((void)0)
#define VGL_COUNTER_SET(field, value) ((void)0)
#endif

static inline __attribute__((always_inline)) int compare_data(const void *a, const void *b, uint32_t size) {
#ifdef DISABLE_CLIB
	return memcmp(a, b, size);
#else
	return sceClibMemcmp(a, b, size);
#endif
}

#ifdef HAVE_RUNTIME_OPTIMIZER_STATS
static inline __attribute__((always_inline)) uint32_t hash_ptr_pair(uintptr_t a, uintptr_t b) {
	uint32_t x = (uint32_t)a;
	uint32_t y = (uint32_t)b;
	x ^= x >> 17;
	x *= 0xed5ad4bb;
	y ^= y >> 15;
	y *= 0xac4c1b51;
	return x ^ y;
}

static inline __attribute__((always_inline)) void pipeline_touch(void) {
	if (!gxm_state_cache.vertex_program || !gxm_state_cache.fragment_program)
		return;

	uintptr_t vp = (uintptr_t)gxm_state_cache.vertex_program;
	uintptr_t fp = (uintptr_t)gxm_state_cache.fragment_program;
	uint32_t slot = hash_ptr_pair(vp, fp) & (VGL_OPT_PIPELINE_CACHE_SIZE - 1);
	vgl_pipeline_cache_entry *entry = &pipeline_cache[slot];
	pipeline_stamp++;
	if (entry->valid && entry->vertex_program == vp && entry->fragment_program == fp) {
		entry->stamp = pipeline_stamp;
		VGL_COUNTER_INC(pipeline_cache_hits);
	} else {
		entry->valid = GL_TRUE;
		entry->vertex_program = vp;
		entry->fragment_program = fp;
		entry->stamp = pipeline_stamp;
		VGL_COUNTER_INC(pipeline_cache_misses);
	}
}
#else
#define pipeline_touch() ((void)0)
#endif

static void __attribute__((noinline, cold)) cache_context_changed(SceGxmContext *context) {
	if (gxm_state_cache.context && (gxm_state_cache.valid & (VGL_OPT_VALID_PRECOMPUTED_VERTEX_STATE | VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE)))
		vgl_optimizer_leave_precomputed_state(gxm_state_cache.context);
	vgl_memset(&gxm_state_cache, 0, sizeof(vgl_gxm_state_cache));
	gxm_state_cache.context = context;
}

static inline __attribute__((always_inline)) void cache_context(SceGxmContext *context) {
	if (__builtin_expect(gxm_state_cache.context != context, 0))
		cache_context_changed(context);
}

static inline __attribute__((always_inline)) GLboolean cache_pointer(const void **cache, uint32_t *valid_mask, uint32_t index, const void *value) {
	uint32_t bit = 1U << index;
	if ((*valid_mask & bit) && cache[index] == value)
		return GL_TRUE;
	cache[index] = value;
	*valid_mask |= bit;
	return GL_FALSE;
}

#ifdef HAVE_RUNTIME_OPTIMIZER_STATS
static inline __attribute__((always_inline)) vglDrawPath select_draw_path(SceGxmPrimitiveType primitive_type, SceGxmIndexFormat index_format, const void *index_data) {
	if (((uintptr_t)index_data & 1) != 0)
		return VGL_DRAW_PATH_STRICT;

	if (index_format != SCE_GXM_INDEX_FORMAT_U16 && index_format != SCE_GXM_INDEX_FORMAT_U32)
		return VGL_DRAW_PATH_CONVERTED;

	switch (primitive_type) {
	case SCE_GXM_PRIMITIVE_TRIANGLES:
	case SCE_GXM_PRIMITIVE_TRIANGLE_STRIP:
	case SCE_GXM_PRIMITIVE_TRIANGLE_FAN:
		return VGL_DRAW_PATH_NATIVE;
	default:
		return VGL_DRAW_PATH_STRICT;
	}
}

static inline __attribute__((always_inline)) void count_draw_path(SceGxmPrimitiveType primitive_type, SceGxmIndexFormat index_format, const void *index_data) {
	switch (select_draw_path(primitive_type, index_format, index_data)) {
	case VGL_DRAW_PATH_NATIVE:
		VGL_COUNTER_INC(native_draws);
		break;
	case VGL_DRAW_PATH_CONVERTED:
		VGL_COUNTER_INC(converted_draws);
		break;
	default:
		VGL_COUNTER_INC(strict_draws);
		break;
	}
}
#else
#define count_draw_path(primitive_type, index_format, index_data) ((void)0)
#endif

void vglSetRuntimeOptimizer(GLboolean usage) {
#ifdef HAVE_RUNTIME_OPTIMIZER
	optimizer_enabled = usage;
	vgl_optimizer_scene_reset();
	vgl_optimizer_program_changed(0);
#else
	optimizer_enabled = GL_FALSE;
#endif
}

GLboolean vglGetRuntimeOptimizer(void) {
	return optimizer_enabled;
}

void vglGetPerfCounters(vglPerfCounters *stats) {
	if (!stats)
		return;
	*stats = last_counters.frame ? last_counters : current_counters;
}

void vglResetPerfCounters(void) {
	vgl_memset(&current_counters, 0, sizeof(vglPerfCounters));
	vgl_memset(&last_counters, 0, sizeof(vglPerfCounters));
}

void vglInvalidateRuntimeOptimizerState(void) {
	vgl_optimizer_scene_reset();
}

void vgl_optimizer_leave_precomputed_state(SceGxmContext *context) {
	if (!context)
		return;
#if defined(HAVE_RUNTIME_OPTIMIZER) && !defined(HAVE_RUNTIME_OPTIMIZER_STATS)
	if (vgl_fast_state.context != context)
		return;
	vgl_fast_state.valid &= ~VGL_FAST_PREPARED_VERTEX_BINDING_VALID;
	vgl_fast_state.prepared_vertex_program = NULL;
	vgl_fast_state.prepared_vertex_stream = NULL;
	vgl_fast_state.prepared_vertex_stream_count = 0;
	if (vgl_fast_state.valid & VGL_FAST_PRECOMPUTED_VERTEX_STATE_VALID) {
		sceGxmSetPrecomputedVertexState(context, NULL);
		vgl_fast_state.valid &= ~(VGL_FAST_PRECOMPUTED_VERTEX_STATE_VALID | VGL_FAST_VERTEX_PROGRAM_VALID | VGL_FAST_PREPARED_VERTEX_BINDING_VALID);
		vgl_fast_state.precomputed_vertex_state = NULL;
		vgl_fast_state.vertex_stream_valid = 0;
	}
	if (vgl_fast_state.valid & VGL_FAST_PRECOMPUTED_FRAGMENT_STATE_VALID) {
		sceGxmSetPrecomputedFragmentState(context, NULL);
		vgl_fast_state.valid &= ~(VGL_FAST_PRECOMPUTED_FRAGMENT_STATE_VALID | VGL_FAST_FRAGMENT_PROGRAM_VALID);
		vgl_fast_state.precomputed_fragment_state = NULL;
		vgl_fast_state.fragment_texture_valid = 0;
	}
#else
	if (gxm_state_cache.context != context)
		return;
	gxm_state_cache.valid &= ~VGL_OPT_VALID_PREPARED_VERTEX_BINDING;
	gxm_state_cache.prepared_vertex_program = NULL;
	gxm_state_cache.prepared_vertex_stream = NULL;
	gxm_state_cache.prepared_vertex_stream_count = 0;
	if (gxm_state_cache.valid & VGL_OPT_VALID_PRECOMPUTED_VERTEX_STATE) {
		sceGxmSetPrecomputedVertexState(context, NULL);
		gxm_state_cache.valid &= ~(VGL_OPT_VALID_PRECOMPUTED_VERTEX_STATE | VGL_OPT_VALID_VERTEX_PROGRAM | VGL_OPT_VALID_PREPARED_VERTEX_BINDING);
		gxm_state_cache.precomputed_vertex_state = NULL;
		gxm_state_cache.vertex_stream_valid = 0;
		VGL_COUNTER_INC(state_changes);
	}
	if (gxm_state_cache.valid & VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE) {
		sceGxmSetPrecomputedFragmentState(context, NULL);
		gxm_state_cache.valid &= ~(VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE | VGL_OPT_VALID_FRAGMENT_PROGRAM);
		gxm_state_cache.precomputed_fragment_state = NULL;
		gxm_state_cache.fragment_texture_valid = 0;
		VGL_COUNTER_INC(state_changes);
	}
#endif
}

void vgl_optimizer_scene_reset(void) {
#if defined(HAVE_RUNTIME_OPTIMIZER) && !defined(HAVE_RUNTIME_OPTIMIZER_STATS)
	vgl_optimizer_leave_precomputed_state(vgl_fast_state.context);
#else
	vgl_optimizer_leave_precomputed_state(gxm_state_cache.context);
#endif
	vgl_memset(&gxm_state_cache, 0, sizeof(vgl_gxm_state_cache));
#if defined(HAVE_RUNTIME_OPTIMIZER) && !defined(HAVE_RUNTIME_OPTIMIZER_STATS)
	vgl_memset(&vgl_fast_state, 0, sizeof(vgl_fast_gxm_state));
#endif
}

void vgl_optimizer_frame_end(uint32_t frame) {
#ifdef HAVE_RUNTIME_OPTIMIZER_STATS
	VGL_COUNTER_SET(frame, frame);
	last_counters = current_counters;
	vgl_memset(&current_counters, 0, sizeof(vglPerfCounters));
#endif
}

void vgl_optimizer_program_changed(GLuint prog) {
	if (uniform_program == prog)
		return;
	uniform_program = prog;
	vgl_memset(uniform_cache, 0, sizeof(uniform_cache));
}

void vgl_optimizer_program_storage_reset(GLuint prog) {
	if (uniform_program == prog)
		vgl_memset(uniform_cache, 0, sizeof(uniform_cache));
}

GLboolean vgl_optimizer_uniform_changed(GLint location, const void *data, uint32_t components, uint32_t count, GLenum type) {
	uint64_t total_size = (uint64_t)components * count * sizeof(uint32_t);
	if (!optimizer_enabled || !data || total_size == 0 || total_size > VGL_OPT_UNIFORM_DATA_SIZE)
		return GL_TRUE;

	uint32_t size = (uint32_t)total_size;
	uint32_t slot = ((uint32_t)location * 2654435761U) & (VGL_OPT_UNIFORM_CACHE_SIZE - 1);
	vgl_uniform_cache_entry *entry = &uniform_cache[slot];
	if (entry->valid && entry->location == location && entry->size == size && entry->type == type && !compare_data(entry->data, data, size)) {
		VGL_COUNTER_INC(uniform_uploads_skipped);
		return GL_FALSE;
	}

	entry->valid = GL_TRUE;
	entry->location = location;
	entry->size = size;
	entry->type = type;
	vgl_memcpy(entry->data, data, size);
	VGL_COUNTER_INC(uniform_uploads);
	return GL_TRUE;
}

void vgl_optimizer_buffer_upload(uint32_t size) {
	VGL_COUNTER_INC(buffer_uploads);
	VGL_COUNTER_ADD(buffer_upload_bytes, size);
	VGL_COUNTER_ADD(bytes_uploaded, size);
}

static uint32_t get_texture_bpp(GLenum format, GLenum type) {
	uint32_t components;
	switch (format) {
	case GL_RED:
	case GL_ALPHA:
	case GL_LUMINANCE:
		components = 1;
		break;
	case GL_RG:
	case GL_LUMINANCE_ALPHA:
		components = 2;
		break;
	case GL_RGB:
		components = 3;
		break;
	default:
		components = 4;
		break;
	}

	switch (type) {
	case GL_UNSIGNED_SHORT:
	case GL_SHORT:
	case GL_HALF_FLOAT:
		return components * 2;
	case GL_FLOAT:
	case GL_UNSIGNED_INT:
	case GL_INT:
		return components * 4;
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_5_5_5_1:
		return 2;
	default:
		return components;
	}
}

void vgl_optimizer_texture_upload(GLsizei width, GLsizei height, GLenum format, GLenum type) {
	if (width <= 0 || height <= 0)
		return;
	uint64_t size = (uint64_t)width * (uint64_t)height * get_texture_bpp(format, type);
	VGL_COUNTER_INC(texture_uploads);
	VGL_COUNTER_ADD(texture_upload_bytes, size);
	VGL_COUNTER_ADD(bytes_uploaded, size);
}

void vgl_optimizer_texture_upload_bytes(uint32_t size) {
	if (!size)
		return;
	VGL_COUNTER_INC(texture_uploads);
	VGL_COUNTER_ADD(texture_upload_bytes, size);
	VGL_COUNTER_ADD(bytes_uploaded, size);
}

void vgl_optimizer_render_task(void) {
	VGL_COUNTER_INC(render_tasks);
}

void vgl_optimizer_transient_alloc(uint32_t size) {
	VGL_COUNTER_ADD(transient_bytes, size);
}

void vgl_optimizer_index_range(GLboolean hit) {
	if (hit)
		VGL_COUNTER_INC(index_range_cache_hits);
	else
		VGL_COUNTER_INC(index_range_cache_misses);
}

void vgl_optimizer_index_range_partial(void) {
	VGL_COUNTER_INC(index_range_partial_scans);
}

void vgl_optimizer_gl_state(GLboolean skipped) {
	VGL_COUNTER_INC(gl_state_calls);
	if (skipped)
		VGL_COUNTER_INC(gl_state_calls_skipped);
}

void vgl_optimizer_sampler_state(GLboolean hit) {
	if (hit)
		VGL_COUNTER_INC(sampler_state_cache_hits);
	else
		VGL_COUNTER_INC(sampler_state_cache_misses);
}

void vgl_optimizer_vertex_program(GLboolean hit) {
	if (hit)
		VGL_COUNTER_INC(vertex_program_cache_hits);
	else
		VGL_COUNTER_INC(vertex_program_cache_misses);
}

void vgl_optimizer_fragment_program(GLboolean hit) {
	if (hit)
		VGL_COUNTER_INC(fragment_program_cache_hits);
	else
		VGL_COUNTER_INC(fragment_program_cache_misses);
}

void vgl_optimizer_prepared_draw(GLboolean hit) {
	if (hit)
		VGL_COUNTER_INC(prepared_draw_cache_hits);
	else
		VGL_COUNTER_INC(prepared_draw_cache_misses);
}

void vgl_optimizer_precomputed_submission(GLboolean submitted) {
	if (submitted)
		VGL_COUNTER_INC(gxm_precomputed_draws);
	else
		VGL_COUNTER_INC(gxm_precomputed_fallbacks);
}

void vgl_optimizer_precomputed_fragment_state(GLboolean submitted) {
	if (submitted)
		VGL_COUNTER_INC(gxm_precomputed_fragment_states);
	else
		VGL_COUNTER_INC(gxm_precomputed_fragment_fallbacks);
}

void vgl_optimizer_precomputed_vertex_state(GLboolean submitted) {
	if (submitted)
		VGL_COUNTER_INC(gxm_precomputed_vertex_states);
	else
		VGL_COUNTER_INC(gxm_precomputed_vertex_fallbacks);
}

void vgl_optimizer_prepared_vertex_binding(GLboolean hit) {
	if (hit)
		VGL_COUNTER_INC(prepared_vertex_binding_hits);
	else
		VGL_COUNTER_INC(prepared_vertex_binding_misses);
}

void vgl_optimizer_apply_prepared_draw(SceGxmContext *context, const SceGxmFragmentProgram *fragment_program, const SceGxmTexture *fragment_texture, const SceGxmVertexProgram *vertex_program, const void *stream_data, uint32_t stream_count) {
#if defined(HAVE_RUNTIME_OPTIMIZER) && !defined(HAVE_RUNTIME_OPTIMIZER_STATS)
	vgl_fast_cache_context(context);
	if (vgl_fast_state.valid & (VGL_FAST_PRECOMPUTED_VERTEX_STATE_VALID | VGL_FAST_PRECOMPUTED_FRAGMENT_STATE_VALID))
		vgl_optimizer_leave_precomputed_state(context);
	vgl_fast_state.valid &= ~VGL_FAST_PREPARED_VERTEX_BINDING_VALID;
	if (!optimizer_enabled || !(vgl_fast_state.valid & VGL_FAST_FRAGMENT_PROGRAM_VALID) || vgl_fast_state.fragment_program != fragment_program) {
		vgl_fast_state.fragment_program = fragment_program;
		vgl_fast_state.valid |= VGL_FAST_FRAGMENT_PROGRAM_VALID;
		sceGxmSetFragmentProgram(context, fragment_program);
	}
	if (fragment_texture) {
		uint32_t bit = 1U;
		if (!optimizer_enabled || !(vgl_fast_state.fragment_texture_valid & bit) || !vgl_fast_texture_matches(&vgl_fast_state.fragment_texture[0], fragment_texture)) {
			vgl_fast_state.fragment_texture[0] = *fragment_texture;
			vgl_fast_state.fragment_texture_valid |= bit;
			sceGxmSetFragmentTexture(context, 0, fragment_texture);
		}
	}
	if (!optimizer_enabled || !(vgl_fast_state.valid & VGL_FAST_VERTEX_PROGRAM_VALID) || vgl_fast_state.vertex_program != vertex_program) {
		vgl_fast_state.vertex_program = vertex_program;
		vgl_fast_state.valid |= VGL_FAST_VERTEX_PROGRAM_VALID;
		sceGxmSetVertexProgram(context, vertex_program);
	}
	for (uint32_t i = 0; i < stream_count; i++) {
		uint32_t bit = 1U << i;
		if (!optimizer_enabled || !(vgl_fast_state.vertex_stream_valid & bit) || vgl_fast_state.vertex_stream[i] != stream_data) {
			vgl_fast_state.vertex_stream[i] = stream_data;
			vgl_fast_state.vertex_stream_valid |= bit;
			sceGxmSetVertexStream(context, i, stream_data);
		}
	}
#else
	vgl_optimizer_leave_precomputed_state(context);
	gxm_state_cache.valid &= ~VGL_OPT_VALID_PREPARED_VERTEX_BINDING;
	vgl_optimizer_set_fragment_program(context, fragment_program);
	if (fragment_texture)
		vgl_optimizer_set_fragment_texture(context, 0, fragment_texture);
	vgl_optimizer_set_vertex_program(context, vertex_program);
	for (uint32_t i = 0; i < stream_count; i++)
		vgl_optimizer_set_vertex_stream(context, i, stream_data);
#endif
}

static void __attribute__((noinline, cold)) bind_prepared_vertex_slow(SceGxmContext *context, const SceGxmVertexProgram *vertex_program, const void *stream_data, uint32_t stream_count) {
#if defined(HAVE_RUNTIME_OPTIMIZER) && !defined(HAVE_RUNTIME_OPTIMIZER_STATS)
	if (vgl_fast_state.valid & VGL_FAST_PRECOMPUTED_VERTEX_STATE_VALID) {
		sceGxmSetPrecomputedVertexState(context, NULL);
		vgl_fast_state.precomputed_vertex_state = NULL;
		vgl_fast_state.valid &= ~(VGL_FAST_PRECOMPUTED_VERTEX_STATE_VALID | VGL_FAST_VERTEX_PROGRAM_VALID);
		vgl_fast_state.vertex_stream_valid = 0;
	}
	if (!optimizer_enabled || !(vgl_fast_state.valid & VGL_FAST_VERTEX_PROGRAM_VALID) || vgl_fast_state.vertex_program != vertex_program) {
		vgl_fast_state.vertex_program = vertex_program;
		vgl_fast_state.valid |= VGL_FAST_VERTEX_PROGRAM_VALID;
		sceGxmSetVertexProgram(context, vertex_program);
	}
	for (uint32_t i = 0; i < stream_count; i++) {
		uint32_t bit = 1U << i;
		if (!optimizer_enabled || !(vgl_fast_state.vertex_stream_valid & bit) || vgl_fast_state.vertex_stream[i] != stream_data) {
			vgl_fast_state.vertex_stream[i] = stream_data;
			vgl_fast_state.vertex_stream_valid |= bit;
			sceGxmSetVertexStream(context, i, stream_data);
		}
	}
	vgl_fast_state.prepared_vertex_program = vertex_program;
	vgl_fast_state.prepared_vertex_stream = stream_data;
	vgl_fast_state.prepared_vertex_stream_count = stream_count;
	vgl_fast_state.valid |= VGL_FAST_PREPARED_VERTEX_BINDING_VALID;
	vgl_optimizer_prepared_vertex_binding(GL_FALSE);
#else
	vgl_optimizer_set_vertex_program(context, vertex_program);
	for (uint32_t i = 0; i < stream_count; i++)
		vgl_optimizer_set_vertex_stream(context, i, stream_data);
	gxm_state_cache.prepared_vertex_program = vertex_program;
	gxm_state_cache.prepared_vertex_stream = stream_data;
	gxm_state_cache.prepared_vertex_stream_count = stream_count;
	gxm_state_cache.valid |= VGL_OPT_VALID_PREPARED_VERTEX_BINDING;
	vgl_optimizer_prepared_vertex_binding(GL_FALSE);
#endif
}

void __attribute__((noinline, hot)) vgl_optimizer_apply_precomputed_prepared_draw2(SceGxmContext *context, const SceGxmPrecomputedFragmentState *fragment_state, const SceGxmVertexProgram *vertex_program, const void *stream_data) {
#if defined(HAVE_RUNTIME_OPTIMIZER) && !defined(HAVE_RUNTIME_OPTIMIZER_STATS)
	vgl_fast_cache_context(context);
	if (!optimizer_enabled || !(vgl_fast_state.valid & VGL_FAST_PRECOMPUTED_FRAGMENT_STATE_VALID) || vgl_fast_state.precomputed_fragment_state != fragment_state) {
		sceGxmSetPrecomputedFragmentState(context, fragment_state);
		vgl_fast_state.precomputed_fragment_state = fragment_state;
		vgl_fast_state.valid |= VGL_FAST_PRECOMPUTED_FRAGMENT_STATE_VALID;
		vgl_fast_state.valid &= ~VGL_FAST_FRAGMENT_PROGRAM_VALID;
		vgl_fast_state.fragment_texture_valid = 0;
	}
	if (optimizer_enabled && (vgl_fast_state.valid & VGL_FAST_PREPARED_VERTEX_BINDING_VALID) && vgl_fast_state.prepared_vertex_program == vertex_program && vgl_fast_state.prepared_vertex_stream == stream_data) {
		vgl_optimizer_prepared_vertex_binding(GL_TRUE);
		return;
	}
#else
	cache_context(context);
	if (!optimizer_enabled || !(gxm_state_cache.valid & VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE) || gxm_state_cache.precomputed_fragment_state != fragment_state) {
		sceGxmSetPrecomputedFragmentState(context, fragment_state);
		gxm_state_cache.precomputed_fragment_state = fragment_state;
		gxm_state_cache.valid |= VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE;
		gxm_state_cache.valid &= ~VGL_OPT_VALID_FRAGMENT_PROGRAM;
		gxm_state_cache.fragment_texture_valid = 0;
		VGL_COUNTER_INC(state_changes);
	} else {
		VGL_COUNTER_INC(state_changes_skipped);
	}
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_PREPARED_VERTEX_BINDING) && gxm_state_cache.prepared_vertex_program == vertex_program && gxm_state_cache.prepared_vertex_stream == stream_data) {
		vgl_optimizer_prepared_vertex_binding(GL_TRUE);
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
#endif
	bind_prepared_vertex_slow(context, vertex_program, stream_data, 2);
}

void __attribute__((noinline, hot)) vgl_optimizer_apply_precomputed_prepared_draw(SceGxmContext *context, const SceGxmPrecomputedFragmentState *fragment_state, const SceGxmVertexProgram *vertex_program, const void *stream_data, uint32_t stream_count) {
#if defined(HAVE_RUNTIME_OPTIMIZER) && !defined(HAVE_RUNTIME_OPTIMIZER_STATS)
	vgl_fast_cache_context(context);
	if (!optimizer_enabled || !(vgl_fast_state.valid & VGL_FAST_PRECOMPUTED_FRAGMENT_STATE_VALID) || vgl_fast_state.precomputed_fragment_state != fragment_state) {
		sceGxmSetPrecomputedFragmentState(context, fragment_state);
		vgl_fast_state.precomputed_fragment_state = fragment_state;
		vgl_fast_state.valid |= VGL_FAST_PRECOMPUTED_FRAGMENT_STATE_VALID;
		vgl_fast_state.valid &= ~VGL_FAST_FRAGMENT_PROGRAM_VALID;
		vgl_fast_state.fragment_texture_valid = 0;
	}
	if (optimizer_enabled && (vgl_fast_state.valid & VGL_FAST_PREPARED_VERTEX_BINDING_VALID) && vgl_fast_state.prepared_vertex_program == vertex_program && vgl_fast_state.prepared_vertex_stream == stream_data && vgl_fast_state.prepared_vertex_stream_count == stream_count) {
		vgl_optimizer_prepared_vertex_binding(GL_TRUE);
		return;
	}
#else
	cache_context(context);
	if (!optimizer_enabled || !(gxm_state_cache.valid & VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE) || gxm_state_cache.precomputed_fragment_state != fragment_state) {
		sceGxmSetPrecomputedFragmentState(context, fragment_state);
		gxm_state_cache.precomputed_fragment_state = fragment_state;
		gxm_state_cache.valid |= VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE;
		gxm_state_cache.valid &= ~VGL_OPT_VALID_FRAGMENT_PROGRAM;
		gxm_state_cache.fragment_texture_valid = 0;
		VGL_COUNTER_INC(state_changes);
	} else {
		VGL_COUNTER_INC(state_changes_skipped);
	}
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_PREPARED_VERTEX_BINDING) && gxm_state_cache.prepared_vertex_program == vertex_program && gxm_state_cache.prepared_vertex_stream == stream_data && gxm_state_cache.prepared_vertex_stream_count == stream_count) {
		vgl_optimizer_prepared_vertex_binding(GL_TRUE);
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
#endif
	bind_prepared_vertex_slow(context, vertex_program, stream_data, stream_count);
}

void *vglReserveTransientData(GLsizeiptr size, GLuint alignment) {
	if (size <= 0)
		return NULL;
	if (alignment < 4)
		alignment = 4;
	if (alignment & (alignment - 1))
		return NULL;

	uint64_t requested_size = (uint64_t)size + alignment - 1;
	if (requested_size > UINT32_MAX)
		return NULL;
	uint32_t total_size = (uint32_t)requested_size;
	void *base = gpu_alloc_mapped_temp(total_size);
	if (!base)
		return NULL;
	uintptr_t aligned = ((uintptr_t)base + alignment - 1) & ~(uintptr_t)(alignment - 1);
	return (void *)aligned;
}

uint64_t vglBuildShaderCacheKey(const void *source, GLsizeiptr size, const char *defines) {
	const uint8_t *ptr = (const uint8_t *)source;
	uint64_t hash = 1469598103934665603ULL;
	for (GLsizeiptr i = 0; ptr && i < size; i++) {
		hash ^= ptr[i];
		hash *= 1099511628211ULL;
	}
	for (const uint8_t *p = (const uint8_t *)defines; p && *p; p++) {
		hash ^= *p;
		hash *= 1099511628211ULL;
	}
	static const char cache_version[] = "vitaGL-runtime-optimizer-v1:" VGL_GIT_HASH;
	for (const uint8_t *p = (const uint8_t *)cache_version; *p; p++) {
		hash ^= *p;
		hash *= 1099511628211ULL;
	}
	return hash;
}

void vgl_optimizer_set_vertex_program(SceGxmContext *context, const SceGxmVertexProgram *program) {
	cache_context(context);
	if (gxm_state_cache.valid & VGL_OPT_VALID_PRECOMPUTED_VERTEX_STATE) {
		sceGxmSetPrecomputedVertexState(context, NULL);
		gxm_state_cache.valid &= ~(VGL_OPT_VALID_PRECOMPUTED_VERTEX_STATE | VGL_OPT_VALID_VERTEX_PROGRAM | VGL_OPT_VALID_PREPARED_VERTEX_BINDING);
		gxm_state_cache.precomputed_vertex_state = NULL;
		gxm_state_cache.vertex_stream_valid = 0;
		VGL_COUNTER_INC(state_changes);
	}
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_VERTEX_PROGRAM) && gxm_state_cache.vertex_program == program) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	gxm_state_cache.valid &= ~VGL_OPT_VALID_PREPARED_VERTEX_BINDING;
	gxm_state_cache.vertex_program = program;
	gxm_state_cache.valid |= VGL_OPT_VALID_VERTEX_PROGRAM;
	VGL_COUNTER_INC(state_changes);
	sceGxmSetVertexProgram(context, program);
}

void vgl_optimizer_set_fragment_program(SceGxmContext *context, const SceGxmFragmentProgram *program) {
	cache_context(context);
	if (gxm_state_cache.valid & VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE) {
		sceGxmSetPrecomputedFragmentState(context, NULL);
		gxm_state_cache.valid &= ~(VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE | VGL_OPT_VALID_FRAGMENT_PROGRAM);
		gxm_state_cache.precomputed_fragment_state = NULL;
		gxm_state_cache.fragment_texture_valid = 0;
		VGL_COUNTER_INC(state_changes);
	}
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_FRAGMENT_PROGRAM) && gxm_state_cache.fragment_program == program) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	gxm_state_cache.fragment_program = program;
	gxm_state_cache.valid |= VGL_OPT_VALID_FRAGMENT_PROGRAM;
	VGL_COUNTER_INC(state_changes);
	sceGxmSetFragmentProgram(context, program);
}

int vgl_optimizer_set_vertex_stream(SceGxmContext *context, unsigned int stream_index, const void *stream_data) {
	cache_context(context);
	if (stream_index < VGL_OPT_MAX_STREAMS && optimizer_enabled && cache_pointer(gxm_state_cache.vertex_stream, &gxm_state_cache.vertex_stream_valid, stream_index, stream_data)) {
		VGL_COUNTER_INC(state_changes_skipped);
		return SCE_OK;
	}
	gxm_state_cache.valid &= ~VGL_OPT_VALID_PREPARED_VERTEX_BINDING;
	if (stream_index < VGL_OPT_MAX_STREAMS && !optimizer_enabled)
		cache_pointer(gxm_state_cache.vertex_stream, &gxm_state_cache.vertex_stream_valid, stream_index, stream_data);
	VGL_COUNTER_INC(state_changes);
	return sceGxmSetVertexStream(context, stream_index, stream_data);
}

int vgl_optimizer_set_fragment_texture(SceGxmContext *context, unsigned int texture_index, const SceGxmTexture *texture) {
	cache_context(context);
	if (texture_index < VGL_OPT_MAX_TEXTURES) {
		uint32_t bit = 1U << texture_index;
		if (optimizer_enabled && texture && (gxm_state_cache.fragment_texture_valid & bit) && !compare_data(&gxm_state_cache.fragment_texture[texture_index], texture, sizeof(SceGxmTexture))) {
			VGL_COUNTER_INC(state_changes_skipped);
			return SCE_OK;
		}
		if (texture) {
			gxm_state_cache.fragment_texture[texture_index] = *texture;
			gxm_state_cache.fragment_texture_valid |= bit;
		} else {
			gxm_state_cache.fragment_texture_valid &= ~bit;
		}
	}
	VGL_COUNTER_INC(state_changes);
	return sceGxmSetFragmentTexture(context, texture_index, texture);
}

int vgl_optimizer_set_vertex_texture(SceGxmContext *context, unsigned int texture_index, const SceGxmTexture *texture) {
	cache_context(context);
	if (texture_index < VGL_OPT_MAX_TEXTURES) {
		uint32_t bit = 1U << texture_index;
		if (optimizer_enabled && texture && (gxm_state_cache.vertex_texture_valid & bit) && !compare_data(&gxm_state_cache.vertex_texture[texture_index], texture, sizeof(SceGxmTexture))) {
			VGL_COUNTER_INC(state_changes_skipped);
			return SCE_OK;
		}
		if (texture) {
			gxm_state_cache.vertex_texture[texture_index] = *texture;
			gxm_state_cache.vertex_texture_valid |= bit;
		} else {
			gxm_state_cache.vertex_texture_valid &= ~bit;
		}
	}
	VGL_COUNTER_INC(state_changes);
	return sceGxmSetVertexTexture(context, texture_index, texture);
}

int vgl_optimizer_set_vertex_uniform_buffer(SceGxmContext *context, unsigned int buffer_index, const void *buffer_data) {
	cache_context(context);
	if (buffer_index < VGL_OPT_MAX_UNIFORM_BUFFERS && optimizer_enabled && cache_pointer(gxm_state_cache.vertex_uniform, &gxm_state_cache.vertex_uniform_valid, buffer_index, buffer_data)) {
		VGL_COUNTER_INC(state_changes_skipped);
		return SCE_OK;
	}
	VGL_COUNTER_INC(state_changes);
	return sceGxmSetVertexUniformBuffer(context, buffer_index, buffer_data);
}

int vgl_optimizer_set_fragment_uniform_buffer(SceGxmContext *context, unsigned int buffer_index, const void *buffer_data) {
	cache_context(context);
	if (buffer_index < VGL_OPT_MAX_UNIFORM_BUFFERS && optimizer_enabled && cache_pointer(gxm_state_cache.fragment_uniform, &gxm_state_cache.fragment_uniform_valid, buffer_index, buffer_data)) {
		VGL_COUNTER_INC(state_changes_skipped);
		return SCE_OK;
	}
	VGL_COUNTER_INC(state_changes);
	return sceGxmSetFragmentUniformBuffer(context, buffer_index, buffer_data);
}

int vgl_optimizer_set_vertex_default_uniform_buffer(SceGxmContext *context, const void *buffer_data) {
	cache_context(context);
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_VERTEX_DEFAULT_BUFFER) && gxm_state_cache.vertex_default_uniform == buffer_data) {
		VGL_COUNTER_INC(state_changes_skipped);
		return SCE_OK;
	}
	gxm_state_cache.vertex_default_uniform = buffer_data;
	gxm_state_cache.valid |= VGL_OPT_VALID_VERTEX_DEFAULT_BUFFER;
	VGL_COUNTER_INC(state_changes);
	return sceGxmSetVertexDefaultUniformBuffer(context, buffer_data);
}

int vgl_optimizer_set_fragment_default_uniform_buffer(SceGxmContext *context, const void *buffer_data) {
	cache_context(context);
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_FRAGMENT_DEFAULT_BUFFER) && gxm_state_cache.fragment_default_uniform == buffer_data) {
		VGL_COUNTER_INC(state_changes_skipped);
		return SCE_OK;
	}
	gxm_state_cache.fragment_default_uniform = buffer_data;
	gxm_state_cache.valid |= VGL_OPT_VALID_FRAGMENT_DEFAULT_BUFFER;
	VGL_COUNTER_INC(state_changes);
	return sceGxmSetFragmentDefaultUniformBuffer(context, buffer_data);
}


void vgl_optimizer_set_viewport(SceGxmContext *context, float x_offset, float x_scale, float y_offset, float y_scale, float z_offset, float z_scale) {
	cache_context(context);
	float viewport[6] = {x_offset, x_scale, y_offset, y_scale, z_offset, z_scale};
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_VIEWPORT) && !compare_data(gxm_state_cache.viewport, viewport, sizeof(viewport))) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	vgl_memcpy(gxm_state_cache.viewport, viewport, sizeof(viewport));
	gxm_state_cache.valid |= VGL_OPT_VALID_VIEWPORT;
	VGL_COUNTER_INC(state_changes);
#ifdef HAVE_SOFTFP_ABI
	sceGxmSetViewport_sfp(context, x_offset, x_scale, y_offset, y_scale, z_offset, z_scale);
#else
	sceGxmSetViewport(context, x_offset, x_scale, y_offset, y_scale, z_offset, z_scale);
#endif
}

void vgl_optimizer_set_region_clip(SceGxmContext *context, SceGxmRegionClipMode mode, unsigned int x_min, unsigned int y_min, unsigned int x_max, unsigned int y_max) {
	cache_context(context);
	vgl_region_clip_state state = {mode, x_min, y_min, x_max, y_max};
	vgl_region_clip_state *cached = &gxm_state_cache.region_clip;
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_REGION_CLIP) && cached->mode == mode && cached->x_min == x_min && cached->y_min == y_min && cached->x_max == x_max && cached->y_max == y_max) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	gxm_state_cache.region_clip = state;
	gxm_state_cache.valid |= VGL_OPT_VALID_REGION_CLIP;
	VGL_COUNTER_INC(state_changes);
	sceGxmSetRegionClip(context, mode, x_min, y_min, x_max, y_max);
}

#define vgl_cached_enum_state(func_name, gxm_func, valid_bit, field, type) \
	void func_name(SceGxmContext *context, type value) { \
		cache_context(context); \
		if (optimizer_enabled && (gxm_state_cache.valid & valid_bit) && gxm_state_cache.field == value) { \
			VGL_COUNTER_INC(state_changes_skipped); \
			return; \
		} \
		gxm_state_cache.field = value; \
		gxm_state_cache.valid |= valid_bit; \
		VGL_COUNTER_INC(state_changes); \
		gxm_func(context, value); \
	}

vgl_cached_enum_state(vgl_optimizer_set_cull_mode, sceGxmSetCullMode, VGL_OPT_VALID_CULL_MODE, cull_mode, SceGxmCullMode)
vgl_cached_enum_state(vgl_optimizer_set_front_depth_func, sceGxmSetFrontDepthFunc, VGL_OPT_VALID_FRONT_DEPTH_FUNC, front_depth_func, SceGxmDepthFunc)
vgl_cached_enum_state(vgl_optimizer_set_back_depth_func, sceGxmSetBackDepthFunc, VGL_OPT_VALID_BACK_DEPTH_FUNC, back_depth_func, SceGxmDepthFunc)
vgl_cached_enum_state(vgl_optimizer_set_front_depth_write, sceGxmSetFrontDepthWriteEnable, VGL_OPT_VALID_FRONT_DEPTH_WRITE, front_depth_write, SceGxmDepthWriteMode)
vgl_cached_enum_state(vgl_optimizer_set_back_depth_write, sceGxmSetBackDepthWriteEnable, VGL_OPT_VALID_BACK_DEPTH_WRITE, back_depth_write, SceGxmDepthWriteMode)
vgl_cached_enum_state(vgl_optimizer_set_front_polygon_mode, sceGxmSetFrontPolygonMode, VGL_OPT_VALID_FRONT_POLYGON_MODE, front_polygon_mode, SceGxmPolygonMode)
vgl_cached_enum_state(vgl_optimizer_set_back_polygon_mode, sceGxmSetBackPolygonMode, VGL_OPT_VALID_BACK_POLYGON_MODE, back_polygon_mode, SceGxmPolygonMode)
vgl_cached_enum_state(vgl_optimizer_set_two_sided, sceGxmSetTwoSidedEnable, VGL_OPT_VALID_TWO_SIDED, two_sided, SceGxmTwoSidedMode)

vgl_cached_enum_state(vgl_optimizer_set_front_fragment_program_enable, sceGxmSetFrontFragmentProgramEnable, VGL_OPT_VALID_FRONT_FRAGMENT_ENABLE, front_fragment_enable, SceGxmFragmentProgramMode)
vgl_cached_enum_state(vgl_optimizer_set_back_fragment_program_enable, sceGxmSetBackFragmentProgramEnable, VGL_OPT_VALID_BACK_FRAGMENT_ENABLE, back_fragment_enable, SceGxmFragmentProgramMode)
vgl_cached_enum_state(vgl_optimizer_set_front_line_fill_last_pixel, sceGxmSetFrontLineFillLastPixelEnable, VGL_OPT_VALID_FRONT_LINE_FILL, front_line_fill, SceGxmLineFillLastPixelMode)
vgl_cached_enum_state(vgl_optimizer_set_back_line_fill_last_pixel, sceGxmSetBackLineFillLastPixelEnable, VGL_OPT_VALID_BACK_LINE_FILL, back_line_fill, SceGxmLineFillLastPixelMode)
vgl_cached_enum_state(vgl_optimizer_set_w_clamp_enable, sceGxmSetWClampEnable, VGL_OPT_VALID_W_CLAMP_ENABLE, w_clamp_enable, SceGxmWClampMode)
vgl_cached_enum_state(vgl_optimizer_set_viewport_enable, sceGxmSetViewportEnable, VGL_OPT_VALID_VIEWPORT_ENABLE, viewport_enable, SceGxmViewportMode)
vgl_cached_enum_state(vgl_optimizer_set_w_buffer_enable, sceGxmSetWBufferEnable, VGL_OPT_VALID_W_BUFFER_ENABLE, w_buffer_enable, SceGxmWBufferMode)
vgl_cached_enum_state(vgl_optimizer_set_front_visibility_op, sceGxmSetFrontVisibilityTestOp, VGL_OPT_VALID_FRONT_VISIBILITY_OP, front_visibility_op, SceGxmVisibilityTestOp)
vgl_cached_enum_state(vgl_optimizer_set_back_visibility_op, sceGxmSetBackVisibilityTestOp, VGL_OPT_VALID_BACK_VISIBILITY_OP, back_visibility_op, SceGxmVisibilityTestOp)
vgl_cached_enum_state(vgl_optimizer_set_front_visibility_enable, sceGxmSetFrontVisibilityTestEnable, VGL_OPT_VALID_FRONT_VISIBILITY_ENABLE, front_visibility_enable, SceGxmVisibilityTestMode)
vgl_cached_enum_state(vgl_optimizer_set_back_visibility_enable, sceGxmSetBackVisibilityTestEnable, VGL_OPT_VALID_BACK_VISIBILITY_ENABLE, back_visibility_enable, SceGxmVisibilityTestMode)

#undef vgl_cached_enum_state

#define vgl_cached_uint_state(func_name, gxm_func, valid_bit, field) \
	void func_name(SceGxmContext *context, unsigned int value) { \
		cache_context(context); \
		if (optimizer_enabled && (gxm_state_cache.valid & valid_bit) && gxm_state_cache.field == value) { \
			VGL_COUNTER_INC(state_changes_skipped); \
			return; \
		} \
		gxm_state_cache.field = value; \
		gxm_state_cache.valid |= valid_bit; \
		VGL_COUNTER_INC(state_changes); \
		gxm_func(context, value); \
	}

vgl_cached_uint_state(vgl_optimizer_set_front_point_line_width, sceGxmSetFrontPointLineWidth, VGL_OPT_VALID_FRONT_POINT_LINE_WIDTH, front_point_line_width)
vgl_cached_uint_state(vgl_optimizer_set_back_point_line_width, sceGxmSetBackPointLineWidth, VGL_OPT_VALID_BACK_POINT_LINE_WIDTH, back_point_line_width)
vgl_cached_uint_state(vgl_optimizer_set_front_visibility_index, sceGxmSetFrontVisibilityTestIndex, VGL_OPT_VALID_FRONT_VISIBILITY_INDEX, front_visibility_index)
vgl_cached_uint_state(vgl_optimizer_set_back_visibility_index, sceGxmSetBackVisibilityTestIndex, VGL_OPT_VALID_BACK_VISIBILITY_INDEX, back_visibility_index)

#undef vgl_cached_uint_state

static inline __attribute__((always_inline)) void set_depth_bias(SceGxmContext *context, int factor, int units, uint64_t valid_bit, vgl_depth_bias_state *cached, void (*setter)(SceGxmContext *, int, int)) {
	cache_context(context);
	vgl_depth_bias_state state = {factor, units};
	if (optimizer_enabled && (gxm_state_cache.valid & valid_bit) && cached->factor == factor && cached->units == units) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	*cached = state;
	gxm_state_cache.valid |= valid_bit;
	VGL_COUNTER_INC(state_changes);
	setter(context, factor, units);
}

void vgl_optimizer_set_front_depth_bias(SceGxmContext *context, int factor, int units) {
	set_depth_bias(context, factor, units, VGL_OPT_VALID_FRONT_DEPTH_BIAS, &gxm_state_cache.front_depth_bias, sceGxmSetFrontDepthBias);
}

void vgl_optimizer_set_back_depth_bias(SceGxmContext *context, int factor, int units) {
	set_depth_bias(context, factor, units, VGL_OPT_VALID_BACK_DEPTH_BIAS, &gxm_state_cache.back_depth_bias, sceGxmSetBackDepthBias);
}

void vgl_optimizer_set_w_clamp_value(SceGxmContext *context, float value) {
	cache_context(context);
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_W_CLAMP_VALUE) && !compare_data(&gxm_state_cache.w_clamp_value, &value, sizeof(value))) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	gxm_state_cache.w_clamp_value = value;
	gxm_state_cache.valid |= VGL_OPT_VALID_W_CLAMP_VALUE;
	VGL_COUNTER_INC(state_changes);
	sceGxmSetWClampValue(context, value);
}

void vgl_optimizer_set_default_region_clip_and_viewport(SceGxmContext *context, unsigned int x_max, unsigned int y_max) {
	cache_context(context);
	gxm_state_cache.valid &= ~(VGL_OPT_VALID_VIEWPORT | VGL_OPT_VALID_REGION_CLIP | VGL_OPT_VALID_VIEWPORT_ENABLE);
	VGL_COUNTER_INC(state_changes);
	sceGxmSetDefaultRegionClipAndViewport(context, x_max, y_max);
}

void vgl_optimizer_set_front_stencil_ref(SceGxmContext *context, unsigned int ref) {
	cache_context(context);
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_FRONT_STENCIL_REF) && gxm_state_cache.front_stencil_ref == ref) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	gxm_state_cache.front_stencil_ref = ref;
	gxm_state_cache.valid |= VGL_OPT_VALID_FRONT_STENCIL_REF;
	VGL_COUNTER_INC(state_changes);
	sceGxmSetFrontStencilRef(context, ref);
}

void vgl_optimizer_set_back_stencil_ref(SceGxmContext *context, unsigned int ref) {
	cache_context(context);
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_BACK_STENCIL_REF) && gxm_state_cache.back_stencil_ref == ref) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	gxm_state_cache.back_stencil_ref = ref;
	gxm_state_cache.valid |= VGL_OPT_VALID_BACK_STENCIL_REF;
	VGL_COUNTER_INC(state_changes);
	sceGxmSetBackStencilRef(context, ref);
}

static inline __attribute__((always_inline)) GLboolean stencil_state_matches(const vgl_stencil_state *a, const vgl_stencil_state *b) {
	return a->func == b->func && a->stencil_fail == b->stencil_fail && a->depth_fail == b->depth_fail && a->depth_pass == b->depth_pass && a->compare_mask == b->compare_mask && a->write_mask == b->write_mask;
}

void vgl_optimizer_set_front_stencil_func(SceGxmContext *context, SceGxmStencilFunc func, SceGxmStencilOp stencil_fail, SceGxmStencilOp depth_fail, SceGxmStencilOp depth_pass, unsigned char compare_mask, unsigned char write_mask) {
	cache_context(context);
	vgl_stencil_state state = {func, stencil_fail, depth_fail, depth_pass, compare_mask, write_mask};
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_FRONT_STENCIL_FUNC) && stencil_state_matches(&gxm_state_cache.front_stencil_func, &state)) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	gxm_state_cache.front_stencil_func = state;
	gxm_state_cache.valid |= VGL_OPT_VALID_FRONT_STENCIL_FUNC;
	VGL_COUNTER_INC(state_changes);
	sceGxmSetFrontStencilFunc(context, func, stencil_fail, depth_fail, depth_pass, compare_mask, write_mask);
}

void vgl_optimizer_set_back_stencil_func(SceGxmContext *context, SceGxmStencilFunc func, SceGxmStencilOp stencil_fail, SceGxmStencilOp depth_fail, SceGxmStencilOp depth_pass, unsigned char compare_mask, unsigned char write_mask) {
	cache_context(context);
	vgl_stencil_state state = {func, stencil_fail, depth_fail, depth_pass, compare_mask, write_mask};
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_BACK_STENCIL_FUNC) && stencil_state_matches(&gxm_state_cache.back_stencil_func, &state)) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	gxm_state_cache.back_stencil_func = state;
	gxm_state_cache.valid |= VGL_OPT_VALID_BACK_STENCIL_FUNC;
	VGL_COUNTER_INC(state_changes);
	sceGxmSetBackStencilFunc(context, func, stencil_fail, depth_fail, depth_pass, compare_mask, write_mask);
}

void vgl_optimizer_set_precomputed_vertex_state(SceGxmContext *context, const SceGxmPrecomputedVertexState *state) {
	cache_context(context);
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_PRECOMPUTED_VERTEX_STATE) && gxm_state_cache.precomputed_vertex_state == state) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	gxm_state_cache.precomputed_vertex_state = state;
	if (state)
		gxm_state_cache.valid |= VGL_OPT_VALID_PRECOMPUTED_VERTEX_STATE;
	else
		gxm_state_cache.valid &= ~VGL_OPT_VALID_PRECOMPUTED_VERTEX_STATE;
	gxm_state_cache.valid &= ~(VGL_OPT_VALID_VERTEX_PROGRAM | VGL_OPT_VALID_PREPARED_VERTEX_BINDING);
	gxm_state_cache.vertex_stream_valid = 0;
	VGL_COUNTER_INC(state_changes);
	sceGxmSetPrecomputedVertexState(context, state);
}

void vgl_optimizer_set_precomputed_fragment_state(SceGxmContext *context, const SceGxmPrecomputedFragmentState *state) {
	cache_context(context);
	if (optimizer_enabled && (gxm_state_cache.valid & VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE) && gxm_state_cache.precomputed_fragment_state == state) {
		VGL_COUNTER_INC(state_changes_skipped);
		return;
	}
	gxm_state_cache.precomputed_fragment_state = state;
	if (state)
		gxm_state_cache.valid |= VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE;
	else
		gxm_state_cache.valid &= ~VGL_OPT_VALID_PRECOMPUTED_FRAGMENT_STATE;
	gxm_state_cache.valid &= ~VGL_OPT_VALID_FRAGMENT_PROGRAM;
	gxm_state_cache.fragment_texture_valid = 0;
	VGL_COUNTER_INC(state_changes);
	sceGxmSetPrecomputedFragmentState(context, state);
}

int vgl_optimizer_draw(SceGxmContext *context, SceGxmPrimitiveType primitive_type, SceGxmIndexFormat index_format, const void *index_data, unsigned int index_count) {
	pipeline_touch();
	VGL_COUNTER_INC(draw_calls);
	VGL_COUNTER_INC(indexed_draws);
	count_draw_path(primitive_type, index_format, index_data);
	return sceGxmDraw(context, primitive_type, index_format, index_data, index_count);
}

int vgl_optimizer_draw_instanced(SceGxmContext *context, SceGxmPrimitiveType primitive_type, SceGxmIndexFormat index_format, const void *index_data, unsigned int index_count, unsigned int instance_count) {
	pipeline_touch();
	VGL_COUNTER_INC(draw_calls);
	VGL_COUNTER_INC(indexed_draws);
	VGL_COUNTER_INC(instanced_draws);
	count_draw_path(primitive_type, index_format, index_data);
	return sceGxmDrawInstanced(context, primitive_type, index_format, index_data, index_count, instance_count);
}

int vgl_optimizer_draw_precomputed(SceGxmContext *context, const SceGxmPrecomputedDraw *draw) {
	int ret = sceGxmDrawPrecomputed(context, draw);
	vgl_optimizer_precomputed_submission(ret == SCE_OK);
	if (ret == SCE_OK) {
		VGL_COUNTER_INC(draw_calls);
		VGL_COUNTER_INC(indexed_draws);
		VGL_COUNTER_INC(native_draws);
	}
	return ret;
}

int vgl_optimizer_begin_command_list(SceGxmContext *context) {
	vgl_optimizer_scene_reset();
	return sceGxmBeginCommandList(context);
}

int vgl_optimizer_end_command_list(SceGxmContext *context, SceGxmCommandList *list) {
	int ret = sceGxmEndCommandList(context, list);
	vgl_optimizer_scene_reset();
	return ret;
}

int vgl_optimizer_execute_command_list(SceGxmContext *context, SceGxmCommandList *list) {
	vgl_optimizer_scene_reset();
	int ret = sceGxmExecuteCommandList(context, list);
	vgl_optimizer_scene_reset();
	return ret;
}
