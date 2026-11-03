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
 * vgl_optimizer.h:
 * Internal runtime optimizer declarations and GXM wrappers
 */

#ifndef _VGL_OPTIMIZER_H_
#define _VGL_OPTIMIZER_H_

#include <stdint.h>
#include <psp2/gxm.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VGL_OPT_MAX_STREAMS 16
#define VGL_OPT_MAX_TEXTURES 16
#define VGL_OPT_MAX_UNIFORM_BUFFERS 15
#define VGL_OPT_UNIFORM_CACHE_SIZE 128
#define VGL_OPT_UNIFORM_DATA_SIZE 256
#define VGL_OPT_PIPELINE_CACHE_SIZE 128

typedef enum {
	VGL_DRAW_PATH_NATIVE,
	VGL_DRAW_PATH_CONVERTED,
	VGL_DRAW_PATH_STRICT
} vglDrawPath;

void vgl_optimizer_scene_reset(void);
void vgl_optimizer_frame_end(uint32_t frame);
void vgl_optimizer_program_changed(GLuint prog);
void vgl_optimizer_program_storage_reset(GLuint prog);
GLboolean vgl_optimizer_uniform_changed(GLint location, const void *data, uint32_t components, uint32_t count, GLenum type);
void vgl_optimizer_buffer_upload(uint32_t size);
void vgl_optimizer_texture_upload(GLsizei width, GLsizei height, GLenum format, GLenum type);
void vgl_optimizer_texture_upload_bytes(uint32_t size);
void vgl_optimizer_render_task(void);
void vgl_optimizer_transient_alloc(uint32_t size);
void vgl_optimizer_index_range(GLboolean hit);
void vgl_optimizer_index_range_partial(void);
void vgl_optimizer_gl_state(GLboolean skipped);
void vgl_optimizer_sampler_state(GLboolean hit);
void vgl_optimizer_vertex_program(GLboolean hit);
void vgl_optimizer_fragment_program(GLboolean hit);
void vgl_optimizer_prepared_draw(GLboolean hit);
void vgl_optimizer_precomputed_submission(GLboolean submitted);
void vgl_optimizer_precomputed_fragment_state(GLboolean submitted);
void vgl_optimizer_precomputed_vertex_state(GLboolean submitted);
void vgl_optimizer_prepared_vertex_binding(GLboolean hit);
void vgl_optimizer_leave_precomputed_state(SceGxmContext *context);
void vgl_optimizer_apply_prepared_draw(SceGxmContext *context, const SceGxmFragmentProgram *fragment_program, const SceGxmTexture *fragment_texture, const SceGxmVertexProgram *vertex_program, const void *stream_data, uint32_t stream_count);
void vgl_optimizer_apply_precomputed_prepared_draw2(SceGxmContext *context, const SceGxmPrecomputedFragmentState *fragment_state, const SceGxmVertexProgram *vertex_program, const void *stream_data);
void vgl_optimizer_apply_precomputed_prepared_draw(SceGxmContext *context, const SceGxmPrecomputedFragmentState *fragment_state, const SceGxmVertexProgram *vertex_program, const void *stream_data, uint32_t stream_count);

#if defined(HAVE_RUNTIME_OPTIMIZER) && !defined(HAVE_RUNTIME_OPTIMIZER_STATS)
typedef struct {
	SceGxmContext *context;
	uint32_t valid;
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
} vgl_fast_gxm_state;

extern vgl_fast_gxm_state vgl_fast_state;
extern GLboolean optimizer_enabled;

#define VGL_FAST_VERTEX_PROGRAM_VALID (1U << 0)
#define VGL_FAST_FRAGMENT_PROGRAM_VALID (1U << 1)
#define VGL_FAST_VERTEX_DEFAULT_BUFFER_VALID (1U << 2)
#define VGL_FAST_FRAGMENT_DEFAULT_BUFFER_VALID (1U << 3)
#define VGL_FAST_PRECOMPUTED_VERTEX_STATE_VALID (1U << 4)
#define VGL_FAST_PRECOMPUTED_FRAGMENT_STATE_VALID (1U << 5)
#define VGL_FAST_PREPARED_VERTEX_BINDING_VALID (1U << 6)

static inline __attribute__((always_inline)) void vgl_fast_cache_context(SceGxmContext *context) {
	if (__builtin_expect(vgl_fast_state.context != context, 0)) {
		vgl_optimizer_scene_reset();
		vgl_fast_state.context = context;
	}
}

static inline __attribute__((always_inline)) GLboolean vgl_fast_cache_pointer(const void **cache, uint32_t *valid_mask, uint32_t index, const void *value) {
	uint32_t bit = 1U << index;
	if ((*valid_mask & bit) && cache[index] == value)
		return GL_TRUE;
	cache[index] = value;
	*valid_mask |= bit;
	return GL_FALSE;
}

typedef uint32_t vgl_texture_word __attribute__((may_alias));

static inline __attribute__((always_inline)) GLboolean vgl_fast_texture_matches(const SceGxmTexture *a, const SceGxmTexture *b) {
	const vgl_texture_word *left = (const vgl_texture_word *)a;
	const vgl_texture_word *right = (const vgl_texture_word *)b;
	return left[0] == right[0] && left[1] == right[1] && left[2] == right[2] && left[3] == right[3];
}

static inline __attribute__((always_inline)) void vgl_fast_set_vertex_program(SceGxmContext *context, const SceGxmVertexProgram *program) {
	vgl_fast_cache_context(context);
	if (__builtin_expect(vgl_fast_state.valid & VGL_FAST_PRECOMPUTED_VERTEX_STATE_VALID, 0)) {
		sceGxmSetPrecomputedVertexState(context, NULL);
		vgl_fast_state.valid &= ~(VGL_FAST_PRECOMPUTED_VERTEX_STATE_VALID | VGL_FAST_VERTEX_PROGRAM_VALID | VGL_FAST_PREPARED_VERTEX_BINDING_VALID);
		vgl_fast_state.precomputed_vertex_state = NULL;
		vgl_fast_state.vertex_stream_valid = 0;
	}
	if (optimizer_enabled && (vgl_fast_state.valid & VGL_FAST_VERTEX_PROGRAM_VALID) && vgl_fast_state.vertex_program == program)
		return;
	vgl_fast_state.valid &= ~VGL_FAST_PREPARED_VERTEX_BINDING_VALID;
	vgl_fast_state.vertex_program = program;
	vgl_fast_state.valid |= VGL_FAST_VERTEX_PROGRAM_VALID;
	sceGxmSetVertexProgram(context, program);
}

static inline __attribute__((always_inline)) void vgl_fast_set_fragment_program(SceGxmContext *context, const SceGxmFragmentProgram *program) {
	vgl_fast_cache_context(context);
	if (__builtin_expect(vgl_fast_state.valid & VGL_FAST_PRECOMPUTED_FRAGMENT_STATE_VALID, 0)) {
		sceGxmSetPrecomputedFragmentState(context, NULL);
		vgl_fast_state.valid &= ~(VGL_FAST_PRECOMPUTED_FRAGMENT_STATE_VALID | VGL_FAST_FRAGMENT_PROGRAM_VALID);
		vgl_fast_state.precomputed_fragment_state = NULL;
		vgl_fast_state.fragment_texture_valid = 0;
	}
	if (optimizer_enabled && (vgl_fast_state.valid & VGL_FAST_FRAGMENT_PROGRAM_VALID) && vgl_fast_state.fragment_program == program)
		return;
	vgl_fast_state.fragment_program = program;
	vgl_fast_state.valid |= VGL_FAST_FRAGMENT_PROGRAM_VALID;
	sceGxmSetFragmentProgram(context, program);
}

static inline __attribute__((always_inline)) int vgl_fast_set_vertex_stream(SceGxmContext *context, unsigned int stream_index, const void *stream_data) {
	vgl_fast_cache_context(context);
	if (stream_index < VGL_OPT_MAX_STREAMS && optimizer_enabled && vgl_fast_cache_pointer(vgl_fast_state.vertex_stream, &vgl_fast_state.vertex_stream_valid, stream_index, stream_data))
		return SCE_OK;
	vgl_fast_state.valid &= ~VGL_FAST_PREPARED_VERTEX_BINDING_VALID;
	if (stream_index < VGL_OPT_MAX_STREAMS && !optimizer_enabled)
		vgl_fast_cache_pointer(vgl_fast_state.vertex_stream, &vgl_fast_state.vertex_stream_valid, stream_index, stream_data);
	return sceGxmSetVertexStream(context, stream_index, stream_data);
}

static inline __attribute__((always_inline)) int vgl_fast_set_fragment_texture(SceGxmContext *context, unsigned int texture_index, const SceGxmTexture *texture) {
	vgl_fast_cache_context(context);
	if (texture_index < VGL_OPT_MAX_TEXTURES) {
		uint32_t bit = 1U << texture_index;
		if (optimizer_enabled && texture && (vgl_fast_state.fragment_texture_valid & bit) && vgl_fast_texture_matches(&vgl_fast_state.fragment_texture[texture_index], texture))
			return SCE_OK;
		if (texture) {
			vgl_fast_state.fragment_texture[texture_index] = *texture;
			vgl_fast_state.fragment_texture_valid |= bit;
		} else {
			vgl_fast_state.fragment_texture_valid &= ~bit;
		}
	}
	return sceGxmSetFragmentTexture(context, texture_index, texture);
}

static inline __attribute__((always_inline)) int vgl_fast_set_vertex_texture(SceGxmContext *context, unsigned int texture_index, const SceGxmTexture *texture) {
	vgl_fast_cache_context(context);
	if (texture_index < VGL_OPT_MAX_TEXTURES) {
		uint32_t bit = 1U << texture_index;
		if (optimizer_enabled && texture && (vgl_fast_state.vertex_texture_valid & bit) && vgl_fast_texture_matches(&vgl_fast_state.vertex_texture[texture_index], texture))
			return SCE_OK;
		if (texture) {
			vgl_fast_state.vertex_texture[texture_index] = *texture;
			vgl_fast_state.vertex_texture_valid |= bit;
		} else {
			vgl_fast_state.vertex_texture_valid &= ~bit;
		}
	}
	return sceGxmSetVertexTexture(context, texture_index, texture);
}

static inline __attribute__((always_inline)) int vgl_fast_set_vertex_uniform_buffer(SceGxmContext *context, unsigned int buffer_index, const void *buffer_data) {
	vgl_fast_cache_context(context);
	if (buffer_index < VGL_OPT_MAX_UNIFORM_BUFFERS && optimizer_enabled && vgl_fast_cache_pointer(vgl_fast_state.vertex_uniform, &vgl_fast_state.vertex_uniform_valid, buffer_index, buffer_data))
		return SCE_OK;
	return sceGxmSetVertexUniformBuffer(context, buffer_index, buffer_data);
}

static inline __attribute__((always_inline)) int vgl_fast_set_fragment_uniform_buffer(SceGxmContext *context, unsigned int buffer_index, const void *buffer_data) {
	vgl_fast_cache_context(context);
	if (buffer_index < VGL_OPT_MAX_UNIFORM_BUFFERS && optimizer_enabled && vgl_fast_cache_pointer(vgl_fast_state.fragment_uniform, &vgl_fast_state.fragment_uniform_valid, buffer_index, buffer_data))
		return SCE_OK;
	return sceGxmSetFragmentUniformBuffer(context, buffer_index, buffer_data);
}

static inline __attribute__((always_inline)) int vgl_fast_set_vertex_default_uniform_buffer(SceGxmContext *context, const void *buffer_data) {
	vgl_fast_cache_context(context);
	if (optimizer_enabled && (vgl_fast_state.valid & VGL_FAST_VERTEX_DEFAULT_BUFFER_VALID) && vgl_fast_state.vertex_default_uniform == buffer_data)
		return SCE_OK;
	vgl_fast_state.vertex_default_uniform = buffer_data;
	vgl_fast_state.valid |= VGL_FAST_VERTEX_DEFAULT_BUFFER_VALID;
	return sceGxmSetVertexDefaultUniformBuffer(context, buffer_data);
}

static inline __attribute__((always_inline)) int vgl_fast_set_fragment_default_uniform_buffer(SceGxmContext *context, const void *buffer_data) {
	vgl_fast_cache_context(context);
	if (optimizer_enabled && (vgl_fast_state.valid & VGL_FAST_FRAGMENT_DEFAULT_BUFFER_VALID) && vgl_fast_state.fragment_default_uniform == buffer_data)
		return SCE_OK;
	vgl_fast_state.fragment_default_uniform = buffer_data;
	vgl_fast_state.valid |= VGL_FAST_FRAGMENT_DEFAULT_BUFFER_VALID;
	return sceGxmSetFragmentDefaultUniformBuffer(context, buffer_data);
}
#endif

void vgl_optimizer_set_vertex_program(SceGxmContext *context, const SceGxmVertexProgram *program);
void vgl_optimizer_set_fragment_program(SceGxmContext *context, const SceGxmFragmentProgram *program);
int vgl_optimizer_set_vertex_stream(SceGxmContext *context, unsigned int stream_index, const void *stream_data);
int vgl_optimizer_set_fragment_texture(SceGxmContext *context, unsigned int texture_index, const SceGxmTexture *texture);
int vgl_optimizer_set_vertex_texture(SceGxmContext *context, unsigned int texture_index, const SceGxmTexture *texture);
int vgl_optimizer_set_vertex_uniform_buffer(SceGxmContext *context, unsigned int buffer_index, const void *buffer_data);
int vgl_optimizer_set_fragment_uniform_buffer(SceGxmContext *context, unsigned int buffer_index, const void *buffer_data);
int vgl_optimizer_set_vertex_default_uniform_buffer(SceGxmContext *context, const void *buffer_data);
int vgl_optimizer_set_fragment_default_uniform_buffer(SceGxmContext *context, const void *buffer_data);

void vgl_optimizer_set_viewport(SceGxmContext *context, float x_offset, float x_scale, float y_offset, float y_scale, float z_offset, float z_scale);
void vgl_optimizer_set_region_clip(SceGxmContext *context, SceGxmRegionClipMode mode, unsigned int x_min, unsigned int y_min, unsigned int x_max, unsigned int y_max);
void vgl_optimizer_set_cull_mode(SceGxmContext *context, SceGxmCullMode mode);
void vgl_optimizer_set_front_depth_func(SceGxmContext *context, SceGxmDepthFunc func);
void vgl_optimizer_set_back_depth_func(SceGxmContext *context, SceGxmDepthFunc func);
void vgl_optimizer_set_front_depth_write(SceGxmContext *context, SceGxmDepthWriteMode enable);
void vgl_optimizer_set_back_depth_write(SceGxmContext *context, SceGxmDepthWriteMode enable);
void vgl_optimizer_set_front_polygon_mode(SceGxmContext *context, SceGxmPolygonMode mode);
void vgl_optimizer_set_back_polygon_mode(SceGxmContext *context, SceGxmPolygonMode mode);
void vgl_optimizer_set_two_sided(SceGxmContext *context, SceGxmTwoSidedMode enable);
void vgl_optimizer_set_front_fragment_program_enable(SceGxmContext *context, SceGxmFragmentProgramMode enable);
void vgl_optimizer_set_back_fragment_program_enable(SceGxmContext *context, SceGxmFragmentProgramMode enable);
void vgl_optimizer_set_front_line_fill_last_pixel(SceGxmContext *context, SceGxmLineFillLastPixelMode enable);
void vgl_optimizer_set_back_line_fill_last_pixel(SceGxmContext *context, SceGxmLineFillLastPixelMode enable);
void vgl_optimizer_set_front_point_line_width(SceGxmContext *context, unsigned int width);
void vgl_optimizer_set_back_point_line_width(SceGxmContext *context, unsigned int width);
void vgl_optimizer_set_front_depth_bias(SceGxmContext *context, int factor, int units);
void vgl_optimizer_set_back_depth_bias(SceGxmContext *context, int factor, int units);
void vgl_optimizer_set_w_clamp_value(SceGxmContext *context, float value);
void vgl_optimizer_set_w_clamp_enable(SceGxmContext *context, SceGxmWClampMode enable);
void vgl_optimizer_set_viewport_enable(SceGxmContext *context, SceGxmViewportMode enable);
void vgl_optimizer_set_w_buffer_enable(SceGxmContext *context, SceGxmWBufferMode enable);
void vgl_optimizer_set_front_visibility_index(SceGxmContext *context, unsigned int index);
void vgl_optimizer_set_back_visibility_index(SceGxmContext *context, unsigned int index);
void vgl_optimizer_set_front_visibility_op(SceGxmContext *context, SceGxmVisibilityTestOp op);
void vgl_optimizer_set_back_visibility_op(SceGxmContext *context, SceGxmVisibilityTestOp op);
void vgl_optimizer_set_front_visibility_enable(SceGxmContext *context, SceGxmVisibilityTestMode enable);
void vgl_optimizer_set_back_visibility_enable(SceGxmContext *context, SceGxmVisibilityTestMode enable);
void vgl_optimizer_set_default_region_clip_and_viewport(SceGxmContext *context, unsigned int x_max, unsigned int y_max);
void vgl_optimizer_set_front_stencil_ref(SceGxmContext *context, unsigned int ref);
void vgl_optimizer_set_back_stencil_ref(SceGxmContext *context, unsigned int ref);
void vgl_optimizer_set_front_stencil_func(SceGxmContext *context, SceGxmStencilFunc func, SceGxmStencilOp stencil_fail, SceGxmStencilOp depth_fail, SceGxmStencilOp depth_pass, unsigned char compare_mask, unsigned char write_mask);
void vgl_optimizer_set_back_stencil_func(SceGxmContext *context, SceGxmStencilFunc func, SceGxmStencilOp stencil_fail, SceGxmStencilOp depth_fail, SceGxmStencilOp depth_pass, unsigned char compare_mask, unsigned char write_mask);
void vgl_optimizer_set_precomputed_vertex_state(SceGxmContext *context, const SceGxmPrecomputedVertexState *state);
void vgl_optimizer_set_precomputed_fragment_state(SceGxmContext *context, const SceGxmPrecomputedFragmentState *state);
int vgl_optimizer_draw(SceGxmContext *context, SceGxmPrimitiveType primitive_type, SceGxmIndexFormat index_format, const void *index_data, unsigned int index_count);
int vgl_optimizer_draw_instanced(SceGxmContext *context, SceGxmPrimitiveType primitive_type, SceGxmIndexFormat index_format, const void *index_data, unsigned int index_count, unsigned int instance_count);
int vgl_optimizer_draw_precomputed(SceGxmContext *context, const SceGxmPrecomputedDraw *draw);
int vgl_optimizer_begin_command_list(SceGxmContext *context);
int vgl_optimizer_end_command_list(SceGxmContext *context, SceGxmCommandList *list);
int vgl_optimizer_execute_command_list(SceGxmContext *context, SceGxmCommandList *list);

#if !defined(HAVE_RUNTIME_OPTIMIZER) && !defined(VGL_OPTIMIZER_IMPLEMENTATION)
#define vgl_optimizer_scene_reset() ((void)0)
#define vgl_optimizer_program_changed(prog) ((void)0)
#define vgl_optimizer_program_storage_reset(prog) ((void)0)
#define vgl_optimizer_uniform_changed(location, data, components, count, type) GL_TRUE
#endif

#if !defined(HAVE_RUNTIME_OPTIMIZER_STATS) && !defined(VGL_OPTIMIZER_IMPLEMENTATION)
#define vgl_optimizer_frame_end(frame) ((void)0)
#define vgl_optimizer_buffer_upload(size) ((void)0)
#define vgl_optimizer_texture_upload(width, height, format, type) ((void)0)
#define vgl_optimizer_texture_upload_bytes(size) ((void)0)
#define vgl_optimizer_render_task() ((void)0)
#define vgl_optimizer_transient_alloc(size) ((void)0)
#define vgl_optimizer_index_range(hit) ((void)0)
#define vgl_optimizer_index_range_partial() ((void)0)
#define vgl_optimizer_gl_state(skipped) ((void)0)
#define vgl_optimizer_sampler_state(hit) ((void)0)
#define vgl_optimizer_vertex_program(hit) ((void)0)
#define vgl_optimizer_fragment_program(hit) ((void)0)
#define vgl_optimizer_prepared_draw(hit) ((void)0)
#define vgl_optimizer_precomputed_submission(submitted) ((void)0)
#define vgl_optimizer_precomputed_fragment_state(submitted) ((void)0)
#define vgl_optimizer_precomputed_vertex_state(submitted) ((void)0)
#define vgl_optimizer_prepared_vertex_binding(hit) ((void)0)
#endif

#if defined(HAVE_RUNTIME_OPTIMIZER) && !defined(VGL_OPTIMIZER_IMPLEMENTATION)
#ifdef HAVE_RUNTIME_OPTIMIZER_STATS
#define sceGxmSetVertexProgram(context, program) vgl_optimizer_set_vertex_program(context, program)
#define sceGxmSetFragmentProgram(context, program) vgl_optimizer_set_fragment_program(context, program)
#define sceGxmSetVertexStream(context, stream_index, stream_data) vgl_optimizer_set_vertex_stream(context, stream_index, stream_data)
#define sceGxmSetFragmentTexture(context, texture_index, texture) vgl_optimizer_set_fragment_texture(context, texture_index, texture)
#define sceGxmSetVertexTexture(context, texture_index, texture) vgl_optimizer_set_vertex_texture(context, texture_index, texture)
#define sceGxmSetVertexUniformBuffer(context, buffer_index, buffer_data) vgl_optimizer_set_vertex_uniform_buffer(context, buffer_index, buffer_data)
#define sceGxmSetFragmentUniformBuffer(context, buffer_index, buffer_data) vgl_optimizer_set_fragment_uniform_buffer(context, buffer_index, buffer_data)
#define sceGxmSetVertexDefaultUniformBuffer(context, buffer_data) vgl_optimizer_set_vertex_default_uniform_buffer(context, buffer_data)
#define sceGxmSetFragmentDefaultUniformBuffer(context, buffer_data) vgl_optimizer_set_fragment_default_uniform_buffer(context, buffer_data)
#else
#define sceGxmSetVertexProgram(context, program) vgl_fast_set_vertex_program(context, program)
#define sceGxmSetFragmentProgram(context, program) vgl_fast_set_fragment_program(context, program)
#define sceGxmSetVertexStream(context, stream_index, stream_data) vgl_fast_set_vertex_stream(context, stream_index, stream_data)
#define sceGxmSetFragmentTexture(context, texture_index, texture) vgl_fast_set_fragment_texture(context, texture_index, texture)
#define sceGxmSetVertexTexture(context, texture_index, texture) vgl_fast_set_vertex_texture(context, texture_index, texture)
#define sceGxmSetVertexUniformBuffer(context, buffer_index, buffer_data) vgl_fast_set_vertex_uniform_buffer(context, buffer_index, buffer_data)
#define sceGxmSetFragmentUniformBuffer(context, buffer_index, buffer_data) vgl_fast_set_fragment_uniform_buffer(context, buffer_index, buffer_data)
#define sceGxmSetVertexDefaultUniformBuffer(context, buffer_data) vgl_fast_set_vertex_default_uniform_buffer(context, buffer_data)
#define sceGxmSetFragmentDefaultUniformBuffer(context, buffer_data) vgl_fast_set_fragment_default_uniform_buffer(context, buffer_data)
#endif

#define sceGxmSetViewport(context, x_offset, x_scale, y_offset, y_scale, z_offset, z_scale) vgl_optimizer_set_viewport(context, x_offset, x_scale, y_offset, y_scale, z_offset, z_scale)
#ifdef HAVE_SOFTFP_ABI
#undef vglSetViewport
#define vglSetViewport vgl_optimizer_set_viewport
#endif
#define sceGxmSetRegionClip(context, mode, x_min, y_min, x_max, y_max) vgl_optimizer_set_region_clip(context, mode, x_min, y_min, x_max, y_max)
#define sceGxmSetCullMode(context, mode) vgl_optimizer_set_cull_mode(context, mode)
#define sceGxmSetFrontDepthFunc(context, func) vgl_optimizer_set_front_depth_func(context, func)
#define sceGxmSetBackDepthFunc(context, func) vgl_optimizer_set_back_depth_func(context, func)
#define sceGxmSetFrontDepthWriteEnable(context, enable) vgl_optimizer_set_front_depth_write(context, enable)
#define sceGxmSetBackDepthWriteEnable(context, enable) vgl_optimizer_set_back_depth_write(context, enable)
#define sceGxmSetFrontPolygonMode(context, mode) vgl_optimizer_set_front_polygon_mode(context, mode)
#define sceGxmSetBackPolygonMode(context, mode) vgl_optimizer_set_back_polygon_mode(context, mode)
#define sceGxmSetTwoSidedEnable(context, enable) vgl_optimizer_set_two_sided(context, enable)
#define sceGxmSetFrontFragmentProgramEnable(context, enable) vgl_optimizer_set_front_fragment_program_enable(context, enable)
#define sceGxmSetBackFragmentProgramEnable(context, enable) vgl_optimizer_set_back_fragment_program_enable(context, enable)
#define sceGxmSetFrontLineFillLastPixelEnable(context, enable) vgl_optimizer_set_front_line_fill_last_pixel(context, enable)
#define sceGxmSetBackLineFillLastPixelEnable(context, enable) vgl_optimizer_set_back_line_fill_last_pixel(context, enable)
#define sceGxmSetFrontPointLineWidth(context, width) vgl_optimizer_set_front_point_line_width(context, width)
#define sceGxmSetBackPointLineWidth(context, width) vgl_optimizer_set_back_point_line_width(context, width)
#define sceGxmSetFrontDepthBias(context, factor, units) vgl_optimizer_set_front_depth_bias(context, factor, units)
#define sceGxmSetBackDepthBias(context, factor, units) vgl_optimizer_set_back_depth_bias(context, factor, units)
#define sceGxmSetWClampValue(context, value) vgl_optimizer_set_w_clamp_value(context, value)
#define sceGxmSetWClampEnable(context, enable) vgl_optimizer_set_w_clamp_enable(context, enable)
#define sceGxmSetViewportEnable(context, enable) vgl_optimizer_set_viewport_enable(context, enable)
#define sceGxmSetWBufferEnable(context, enable) vgl_optimizer_set_w_buffer_enable(context, enable)
#define sceGxmSetFrontVisibilityTestIndex(context, index) vgl_optimizer_set_front_visibility_index(context, index)
#define sceGxmSetBackVisibilityTestIndex(context, index) vgl_optimizer_set_back_visibility_index(context, index)
#define sceGxmSetFrontVisibilityTestOp(context, op) vgl_optimizer_set_front_visibility_op(context, op)
#define sceGxmSetBackVisibilityTestOp(context, op) vgl_optimizer_set_back_visibility_op(context, op)
#define sceGxmSetFrontVisibilityTestEnable(context, enable) vgl_optimizer_set_front_visibility_enable(context, enable)
#define sceGxmSetBackVisibilityTestEnable(context, enable) vgl_optimizer_set_back_visibility_enable(context, enable)
#define sceGxmSetDefaultRegionClipAndViewport(context, x_max, y_max) vgl_optimizer_set_default_region_clip_and_viewport(context, x_max, y_max)
#define sceGxmSetFrontStencilRef(context, ref) vgl_optimizer_set_front_stencil_ref(context, ref)
#define sceGxmSetBackStencilRef(context, ref) vgl_optimizer_set_back_stencil_ref(context, ref)
#define sceGxmSetFrontStencilFunc(context, func, stencil_fail, depth_fail, depth_pass, compare_mask, write_mask) vgl_optimizer_set_front_stencil_func(context, func, stencil_fail, depth_fail, depth_pass, compare_mask, write_mask)
#define sceGxmSetBackStencilFunc(context, func, stencil_fail, depth_fail, depth_pass, compare_mask, write_mask) vgl_optimizer_set_back_stencil_func(context, func, stencil_fail, depth_fail, depth_pass, compare_mask, write_mask)
#define sceGxmSetPrecomputedVertexState(context, state) vgl_optimizer_set_precomputed_vertex_state(context, state)
#define sceGxmSetPrecomputedFragmentState(context, state) vgl_optimizer_set_precomputed_fragment_state(context, state)
#ifdef HAVE_RUNTIME_OPTIMIZER_STATS
#define sceGxmDraw(context, primitive_type, index_format, index_data, index_count) vgl_optimizer_draw(context, primitive_type, index_format, index_data, index_count)
#define sceGxmDrawInstanced(context, primitive_type, index_format, index_data, index_count, instance_count) vgl_optimizer_draw_instanced(context, primitive_type, index_format, index_data, index_count, instance_count)
#define sceGxmDrawPrecomputed(context, draw) vgl_optimizer_draw_precomputed(context, draw)
#endif
#define sceGxmBeginCommandList(context) vgl_optimizer_begin_command_list(context)
#define sceGxmEndCommandList(context, list) vgl_optimizer_end_command_list(context, list)
#define sceGxmExecuteCommandList(context, list) vgl_optimizer_execute_command_list(context, list)
#endif

#ifdef __cplusplus
}
#endif

#endif
