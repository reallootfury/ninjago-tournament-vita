/*
 * This file is part of vitaGL
 * Copyright 2017-2023 Rinnegatamante
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* 
 * custom_shaders.c:
 * Implementation for custom shaders feature
 */

#define _GNU_SOURCE
#include <string.h>
#include "shared.h"
#include "utils/glsl_utils.h"
#include "utils/shacccg_paramquery.h"
#if defined(HAVE_SHADER_CACHE) || defined(HAVE_TEX_CACHE)
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#define XXH_NAMESPACE VITAGL_
#define XXH_memcpy vgl_fast_memcpy
#define XXH_memset sceClibMemset
#include "utils/xxhash_utils.h"
#ifdef HAVE_SHADER_CACHE
char vgl_shader_cache_path[256];
#endif
#ifdef HAVE_TEX_CACHE
char vgl_file_cache_path[256];
#endif
#endif

#define MAX_CUSTOM_SHADERS 2048 // Maximum number of linkable custom shaders
#define MAX_CUSTOM_PROGRAMS 1024 // Maximum number of linkable custom programs

#if defined(HAVE_PREPARED_VERTEX_CACHE) && defined(HAVE_RUNTIME_OPTIMIZER) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)
#define HAVE_PREPARED_PACKED_VERTEX_CACHE
#endif

#if defined(HAVE_PREPARED_DRAW_CACHE) && defined(HAVE_PREPARED_PACKED_VERTEX_CACHE) && defined(HAVE_SAMPLER_STATE_CACHE) && !defined(HAVE_FFP_SHADER_SUPPORT) && !defined(HAVE_GLSL_TEXTURE_SIZE) && !defined(HAVE_TEX_CACHE) && !defined(SAMPLERS_SPEEDHACK)
#define HAVE_EXACT_PREPARED_DRAW_CACHE
#endif

#define set_default_attrib_binding() \
	uint32_t cnt = sceGxmProgramGetParameterCount(p->vshader->prog); \
	uint32_t *ptr = vglProgramGetParameterBase(p->vshader->prog); \
	for (int i = 0; i < cnt; i++) { \
		SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr; \
		SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(param); \
		if (cat == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) { \
			p->attr[p->attr_highest_idx++].regIndex = sceGxmProgramParameterGetResourceIndex(param); \
		} \
		ptr += 4; \
	}

#define disable_draw_attrib(i) \
	orig_stride[i] = streams[i].stride; \
	orig_fmt[i] = attributes[i].format; \
	orig_size[i] = attributes[i].componentCount; \
	streams[i].stride = 0; \
	attributes[i].offset = 0; \
	attributes[i].componentCount = cur_vao->vertex_attrib_size[attr_idx]; \
	attributes[i].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;

#ifdef SAFER_DRAW_SPEEDHACK
#define handle_unpacked_attrib(first, count) \
	if (cur_vao->vertex_attrib_state & (1 << attr_idx)) { \
		if (cur_vao->vertex_attrib_vbo[attr_idx]) { \
			vbo *gpu_buf = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx]; \
			ptrs[i] = (uint8_t *)gpu_buf->ptr + cur_vao->vertex_attrib_offsets[attr_idx] + first * streams[i].stride; \
			gpu_buf->last_frame = vgl_framecount; \
			attributes[i].offset = 0; \
		} else { \
			if (count * streams[i].stride > SAFE_DRAW_SIZE_THRESHOLD) { \
				ptrs[i] = (void *)cur_vao->vertex_attrib_offsets[attr_idx] + first * streams[i].stride; \
			} else { \
				ptrs[i] = gpu_alloc_mapped_temp(count * streams[i].stride); \
				vgl_fast_memcpy(ptrs[i], (void *)cur_vao->vertex_attrib_offsets[attr_idx] + first * streams[i].stride, count * streams[i].stride); \
				attributes[i].offset = 0; \
			} \
		} \
	} else { \
		disable_draw_attrib(i) \
	}
#else
#define handle_unpacked_attrib(first, count) \
	if (cur_vao->vertex_attrib_state & (1 << attr_idx)) { \
		if (cur_vao->vertex_attrib_vbo[attr_idx]) { \
			vbo *gpu_buf = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx]; \
			ptrs[i] = (uint8_t *)gpu_buf->ptr + cur_vao->vertex_attrib_offsets[attr_idx] + first * streams[i].stride; \
			gpu_buf->last_frame = vgl_framecount; \
			attributes[i].offset = 0; \
		} else { \
			ptrs[i] = gpu_alloc_mapped_temp(count * streams[i].stride); \
			vgl_fast_memcpy(ptrs[i], (void *)cur_vao->vertex_attrib_offsets[attr_idx] + first * streams[i].stride, count * streams[i].stride); \
			attributes[i].offset = 0; \
		} \
	} else { \
		disable_draw_attrib(i) \
	}
#endif

#define handle_speedhack_attrib() \
	for (int i = 0; i < p->attr_num; i++) { \
		uint8_t attr_idx = p->attr_map[i]; \
		attributes[i].regIndex = p->attr[attr_idx].regIndex; \
		if (cur_vao->vertex_attrib_state & (1 << attr_idx)) { \
			if (cur_vao->vertex_attrib_vbo[attr_idx]) { \
				vbo *gpu_buf = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx]; \
				ptrs[i] = (uint8_t *)gpu_buf->ptr + cur_vao->vertex_attrib_offsets[attr_idx]; \
				gpu_buf->last_frame = vgl_framecount; \
				attributes[i].offset = 0; \
			} else { \
				ptrs[i] = (void *)cur_vao->vertex_attrib_offsets[attr_idx]; \
				attributes[i].offset = 0; \
			} \
		} else { \
			disable_draw_attrib(i) \
		} \
	}

#define handle_packed_vbo_attrib() \
	if (cur_vao->vertex_attrib_state & (1 << attr_idx)) { \
		attributes[i].offset = cur_vao->vertex_attrib_offsets[attr_idx]; \
	} else { \
		disable_draw_attrib(i) \
	}	

#define handle_packed_attrib() \
	if (cur_vao->vertex_attrib_state & (1 << attr_idx)) { \
		attributes[i].offset = cur_vao->vertex_attrib_offsets[attr_idx] - cur_vao->vertex_attrib_offsets[p->attr_map[0]]; \
	} else { \
		disable_draw_attrib(i) \
	}

#ifndef HAVE_FFP_SHADER_SUPPORT
#define upload_uniforms() \
	if (p->vert_uniforms && dirty_shader_vert_unifs) { \
		void *buffer = vglReserveVertexUniformBuffer(p->vshader->unif_buf_size); \
		vgl_fast_memcpy(buffer, p->unif_vbuffer, p->vshader->unif_buf_size); \
		dirty_shader_vert_unifs = GL_FALSE; \
	} \
	if (p->frag_uniforms && dirty_shader_frag_unifs) { \
		void *buffer = vglReserveFragmentUniformBuffer(p->fshader->unif_buf_size); \
		vgl_fast_memcpy(buffer, p->unif_fbuffer, p->fshader->unif_buf_size); \
		dirty_shader_frag_unifs = GL_FALSE; \
	} \
	if (p->vert_ubos) { \
		ubo *u = p->vert_ubos; \
		while (u) { \
			ubo *b = u->alias ? u->alias : u; \
			sceGxmSetVertexUniformBuffer(gxm_context, b->idx, (uint8_t *)ubo_buf[b->bind]->ptr + ubo_offset[b->bind]); \
			ubo_buf[b->bind]->last_frame = vgl_framecount; \
			u = (ubo *)u->chain; \
		} \
	} \
	if (p->frag_ubos) { \
		ubo *u = p->frag_ubos; \
		while (u) { \
			sceGxmSetFragmentUniformBuffer(gxm_context, u->idx, (uint8_t *)ubo_buf[u->bind]->ptr + ubo_offset[u->bind]); \
			ubo_buf[u->bind]->last_frame = vgl_framecount; \
			u = (ubo *)u->chain; \
		} \
	}
#else
#define upload_uniforms() \
	if (p->vert_uniforms && (dirty_vert_unifs || dirty_shader_vert_unifs)) { \
		void *buffer = vglReserveVertexUniformBuffer(p->vshader->unif_buf_size); \
		if (dirty_vert_unifs) { \
			dirty_vert_unifs = GL_FALSE; \
			if (p->ffp_binds[FFP_MVP_MATRIX]) { \
				if (mvp_modified) { \
					matrix4x4_multiply(vgl_mvp_matrix, projection_matrix, modelview_matrix); \
					recalculate_normal_matrix(); \
					mvp_modified = GL_FALSE; \
				} \
				vglSetUniformData(p->ffp_binds[FFP_MVP_MATRIX]->vptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 4, 4, (const float *)vgl_mvp_matrix, SCE_GXM_PARAMETER_TYPE_F32); \
			} \
			if (p->ffp_binds[FFP_MV_MATRIX]) { \
				vglSetUniformData(p->ffp_binds[FFP_MV_MATRIX]->vptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 4, 4, (const float *)modelview_matrix, SCE_GXM_PARAMETER_TYPE_F32); \
			} \
			if (p->ffp_binds[FFP_NORMAL_MATRIX]) { \
				if (mvp_modified) { \
					matrix4x4_multiply(vgl_mvp_matrix, projection_matrix, modelview_matrix); \
					recalculate_normal_matrix(); \
					mvp_modified = GL_FALSE; \
				} \
				vglSetUniformData(p->ffp_binds[FFP_NORMAL_MATRIX]->vptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 3, 3, (const float *)normal_matrix, SCE_GXM_PARAMETER_TYPE_F32); \
			} \
		} \
		vgl_fast_memcpy(buffer, p->unif_vbuffer, p->vshader->unif_buf_size); \
		dirty_shader_vert_unifs = GL_FALSE; \
	} \
	if (p->frag_uniforms && dirty_shader_frag_unifs) { \
		void *buffer = vglReserveFragmentUniformBuffer(p->fshader->unif_buf_size); \
		vgl_fast_memcpy(buffer, p->unif_fbuffer, p->fshader->unif_buf_size); \
		dirty_shader_frag_unifs = GL_FALSE; \
	} \
	if (p->vert_ubos) { \
		ubo *u = p->vert_ubos; \
		while (u) { \
			ubo *b = u->alias ? u->alias : u; \
			sceGxmSetVertexUniformBuffer(gxm_context, b->idx, (uint8_t *)ubo_buf[b->bind]->ptr + ubo_offset[b->bind]); \
			ubo_buf[b->bind]->last_frame = vgl_framecount; \
			u = (ubo *)u->chain; \
		} \
	} \
	if (p->frag_ubos) { \
		ubo *u = p->frag_ubos; \
		while (u) { \
			sceGxmSetFragmentUniformBuffer(gxm_context, u->idx, (uint8_t *)ubo_buf[u->bind]->ptr + ubo_offset[u->bind]); \
			ubo_buf[u->bind]->last_frame = vgl_framecount; \
			u = (ubo *)u->chain; \
		} \
	}
#endif
	
#ifdef HAVE_PROGRAM_VARIANT_CACHE
#define setup_frag_program() \
	if ((p->blend_info.raw != blend_info.raw) || (is_fbo_float != p->is_fbo_float)) { \
		p->is_fbo_float = is_fbo_float; \
		p->blend_info.raw = blend_info.raw; \
		get_fragment_program_variant(p, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, &p->fprog); \
	} \
	sceGxmSetFragmentProgram(gxm_context, p->fprog);
#else
#define setup_frag_program() \
	if ((p->blend_info.raw != blend_info.raw) || (is_fbo_float != p->is_fbo_float)) { \
		p->is_fbo_float = is_fbo_float; \
		p->blend_info.raw = blend_info.raw; \
		rebuild_frag_shader(p->fshader->id, &p->fprog, (SceGxmProgram *)p->vshader->prog, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4); \
	} \
	sceGxmSetFragmentProgram(gxm_context, p->fprog);
#endif
	
#define align_attributes(attributes, streams) \
	if (p->has_unaligned_attrs) { \
		attributes = temp_attributes; \
		streams = temp_streams; \
		for (int i = 0; i < p->attr_num; i++) { \
			uint8_t attr_idx = p->attr_map[i]; \
			temp_attributes[i] = cur_vao->vertex_attrib_config[attr_idx]; \
			temp_streams[i] = cur_vao->vertex_stream_config[attr_idx]; \
			attributes[i].streamIndex = i; \
		} \
	} else { \
		attributes = cur_vao->vertex_attrib_config; \
		streams = cur_vao->vertex_stream_config; \
	}
	
#ifdef HAVE_FFP_SHADER_SUPPORT
const char *ffp_bind_names[FFP_BINDS_NUM] = {
	"gl_ModelViewProjectionMatrix",
	"gl_ModelViewMatrix",
	"gl_NormalMatrix",
};
#endif

// Internal stuffs
GLboolean is_shark_online = GL_FALSE; // Current vitaShaRK status
static SceGxmVertexAttribute temp_attributes[VERTEX_ATTRIBS_NUM];
static SceGxmVertexStream temp_streams[VERTEX_ATTRIBS_NUM];
static unsigned short orig_stride[VERTEX_ATTRIBS_NUM];
static SceGxmAttributeFormat orig_fmt[VERTEX_ATTRIBS_NUM];
static unsigned char orig_size[VERTEX_ATTRIBS_NUM];
static vbo *ubo_buf[UBOS_NUM];
static uint32_t ubo_offset[UBOS_NUM];
static uint8_t tex2d_override = 0;
GLboolean dirty_shader_frag_unifs = GL_TRUE;
GLboolean dirty_shader_vert_unifs = GL_TRUE;

typedef struct {
	GLuint idx;
	char name[64];
} attr_mapping;

#ifdef STRICT_UNIFORMS_COMPLIANCE
typedef union {
	struct {
		uint32_t offset : 12;
		uint32_t program_idx : 10;
		uint32_t uniform_idx : 8;
		uint32_t is_vertex : 1;
		uint32_t zero : 1;
	};
	int raw;
} uniform_location;
#endif

// Internal runtime shader compiler settings
int32_t compiler_fastmath = GL_TRUE;
#ifdef HAVE_UNSAFE_SHADER_FAST_PRECISION
int32_t compiler_fastprecision = GL_TRUE;
#else
int32_t compiler_fastprecision = GL_FALSE;
#endif
int32_t compiler_fastint = GL_TRUE;
#ifdef HAVE_UNSAFE_SHADER_OPTIMIZER
shark_opt compiler_opts = SHARK_OPT_UNSAFE;
#else
shark_opt compiler_opts = SHARK_OPT_FAST;
#endif

#ifdef HAVE_SHADER_CACHE
typedef struct {
	uint32_t version;
	uint32_t type;
	uint32_t semantic_mode;
	uint32_t low_precision;
	int32_t fastmath;
	int32_t fastprecision;
	int32_t fastint;
	uint32_t optimization;
} shader_cache_config;

static uint64_t get_shader_cache_hash(shader *s, shader *pair) {
	shader_cache_config config;
	config.version = 3;
	config.type = s->type;
	config.semantic_mode = glsl_sema_mode;
	config.low_precision = glsl_precision_low;
	config.fastmath = compiler_fastmath;
	config.fastprecision = compiler_fastprecision;
	config.fastint = compiler_fastint;
	config.optimization = compiler_opts;

	uint64_t seed = XXH3_64bits(&config, sizeof(shader_cache_config));
	seed ^= XXH3_64bits(VGL_GIT_HASH, strlen(VGL_GIT_HASH));
	uint64_t hash = XXH3_64bits_withSeed(s->source, s->size, seed);
	if (pair && pair->source)
		hash = XXH3_64bits_withSeed(pair->source, pair->size, hash);
	for (int i = 0; i < glsl_custom_bindings_num; i++) {
		glsl_sema_bind *binding = &glsl_custom_bindings[i];
		hash = XXH3_64bits_withSeed(binding->name, strlen(binding->name), hash);
		uint32_t binding_info[3] = {binding->idx, binding->ref_idx, binding->type};
		hash = XXH3_64bits_withSeed(binding_info, sizeof(binding_info), hash);
	}
	return hash;
}
#endif

GLuint cur_program = 0; // Current in use custom program (0 = No custom program)

typedef enum {
	UNIFORM_DATA,
	UNIFORM_SAMPLER,
	UNIFORM_CUBE_SAMPLER
} uniform_type;

// Uniform struct
typedef struct {
	const SceGxmProgramParameter *ptr;
#ifdef HAVE_GLSL_TEXTURE_SIZE
	glsl_samplers_info *sampler;
#endif
	void *fptr;
	void *vptr;
	uniform_type type;
	uint8_t sampler_index;
} uniform;

// Program status enum
typedef enum {
	PROG_INVALID,
	PROG_UNLINKED,
	PROG_LINKED
} prog_status;

// Uniform buffer object struct
typedef struct {
	const SceGxmProgramParameter *ptr;
	uint32_t bind;
	uint32_t idx;
	void *alias;
	void *chain;
} ubo;

#ifdef ENABLE_LEGACY_PIPELINE
typedef enum {
	VGL_ATTRIB_REGULAR,
	VGL_ATTRIB_PACKED,
	VGL_ATTRIB_UNPACKED
} attrib_mode;
#endif

#ifdef HAVE_PROGRAM_VARIANT_CACHE
#define VERTEX_VARIANT_FAST_CACHE_SIZE 64
#define FRAGMENT_VARIANT_FAST_CACHE_SIZE 128

typedef struct vertex_program_variant {
	SceGxmVertexProgram *gxm_program;
	uint8_t attribute_count;
	uint8_t stream_count;
	SceGxmVertexAttribute attributes[VERTEX_ATTRIBS_NUM];
	SceGxmVertexStream streams[VERTEX_ATTRIBS_NUM];
	struct vertex_program_variant *next;
} vertex_program_variant;

typedef struct fragment_program_variant {
	SceGxmFragmentProgram *gxm_program;
	SceGxmOutputRegisterFormat output_format;
	SceGxmMultisampleMode multisample_mode;
	uint32_t blend_raw;
	struct fragment_program_variant *next;
} fragment_program_variant;

typedef struct {
	void *owner;
	vao *vertex_array;
	uint32_t generation;
	uint32_t stream_key;
	uint8_t attribute_count;
	uint8_t stream_count;
	SceGxmVertexProgram *gxm_program;
} vertex_program_fast_cache_entry;

typedef struct {
	void *owner;
	fragment_program_variant *variant;
} fragment_program_fast_cache_entry;

typedef struct {
	void *owner;
	SceGxmFragmentProgram *gxm_program;
	uint32_t blend_raw;
	uint16_t mode_key;
} fragment_program_recent_cache_entry;
#endif

// Program struct holding vertex/fragment shader info
typedef struct {
	shader *vshader;
	shader *fshader;
	uint8_t status;
	uint8_t max_frag_texunit_idx;
	uint8_t max_vert_texunit_idx;
	uniform *vert_texunits[TEXTURE_IMAGE_UNITS_NUM];
	uniform *frag_texunits[TEXTURE_IMAGE_UNITS_NUM];
	SceGxmVertexAttribute attr[VERTEX_ATTRIBS_NUM];
	SceGxmVertexStream stream[VERTEX_ATTRIBS_NUM];
	uint8_t attr_map[VERTEX_ATTRIBS_NUM];
	SceGxmVertexProgram *vprog;
	SceGxmFragmentProgram *fprog;
	blend_config blend_info;
	GLuint attr_num;
	GLuint attr_idx;
#ifdef ENABLE_LEGACY_PIPELINE
	attrib_mode attr_mode;
#endif
#ifdef HAVE_FFP_SHADER_SUPPORT
	uniform *ffp_binds[FFP_BINDS_NUM];
#endif
#ifdef HAVE_GLSL_TEXTURE_SIZE
	float **samplers_ptr;
#endif
	uniform *vert_uniforms;
	uniform *frag_uniforms;
	uint32_t vert_uniforms_num;
	uint32_t frag_uniforms_num;
	ubo *vert_ubos;
	ubo *frag_ubos;
	GLuint attr_highest_idx;
	GLboolean has_unaligned_attrs;
	GLboolean is_fbo_float;
	uint8_t num_glsl_attr;
	attr_mapping *glsl_attr_map;
	void *unif_fbuffer;
	void *unif_vbuffer;
#ifdef HAVE_PROGRAM_VARIANT_CACHE
	vertex_program_variant *vertex_variants;
	fragment_program_variant *fragment_variants;
#endif
} program;

// Internal shaders and array
static shader shaders[MAX_CUSTOM_SHADERS];
static program progs[MAX_CUSTOM_PROGRAMS];

#ifdef HAVE_PROGRAM_VARIANT_CACHE
static vertex_program_fast_cache_entry vertex_variant_fast_cache[VERTEX_VARIANT_FAST_CACHE_SIZE];
static fragment_program_fast_cache_entry fragment_variant_fast_cache[FRAGMENT_VARIANT_FAST_CACHE_SIZE];
static vertex_program_fast_cache_entry vertex_variant_recent[2];
static fragment_program_recent_cache_entry fragment_variant_recent[2];

#ifdef HAVE_PREPARED_PACKED_VERTEX_CACHE
#define PREPARED_VERTEX_FAST_CACHE_SIZE 64

typedef struct {
	program *owner;
	vao *vertex_array;
	vbo *vertex_buffer;
	SceGxmVertexProgram *gxm_program;
	uint32_t vao_generation;
	uint32_t buffer_generation;
	uint16_t stride;
	uint8_t stream_count;
#ifdef HAVE_GXM_PRECOMPUTED_DRAW_CACHE
	SceGxmPrecomputedDraw precomputed_draw;
	void *precomputed_mem;
	const void *precomputed_stream;
	uint32_t precomputed_count;
	GLboolean precomputed_ready;
#endif
} prepared_vertex_fast_cache_entry;

static prepared_vertex_fast_cache_entry prepared_vertex_fast_cache[PREPARED_VERTEX_FAST_CACHE_SIZE];
static prepared_vertex_fast_cache_entry prepared_vertex_recent[2];

#ifdef HAVE_EXACT_PREPARED_DRAW_CACHE
#ifdef HAVE_HASHED_PREPARED_DRAW_CACHE
#define PREPARED_DRAW_FAST_CACHE_SIZE 64
#else
#define PREPARED_DRAW_FAST_CACHE_SIZE 2
#endif

typedef struct {
	program *owner;
	texture *fragment_texture;
	sampler *fragment_sampler;
	SceGxmFragmentProgram *fragment_program;
	SceGxmTexture fragment_descriptor;
	uint32_t sampler_generation;
	uint32_t blend_raw;
	uint16_t mode_key;
	uint8_t texture_count;
	GLboolean texture_overridden;
#ifdef HAVE_GXM_PRECOMPUTED_FRAGMENT_STATE_CACHE
	SceGxmPrecomputedFragmentState precomputed_fragment_state;
	void *precomputed_fragment_mem;
	GLboolean precomputed_fragment_ready;
#endif
} prepared_draw_cache_entry;

static prepared_draw_cache_entry prepared_draw_fast_cache[PREPARED_DRAW_FAST_CACHE_SIZE];
#endif
#endif

#ifdef HAVE_GXM_PRECOMPUTED_FRAGMENT_STATE_CACHE
static void release_precomputed_fragment_entry(prepared_draw_cache_entry *entry) {
	if (!entry->precomputed_fragment_mem)
		return;
	vgl_optimizer_leave_precomputed_state(gxm_context);
	vglFree(entry->precomputed_fragment_mem);
	entry->precomputed_fragment_mem = NULL;
	entry->precomputed_fragment_ready = GL_FALSE;
}

static void release_precomputed_fragment_cache(void) {
	for (int i = 0; i < PREPARED_DRAW_FAST_CACHE_SIZE; i++)
		release_precomputed_fragment_entry(&prepared_draw_fast_cache[i]);
}
#endif

#ifdef HAVE_GXM_PRECOMPUTED_DRAW_CACHE
static void release_precomputed_draw(prepared_vertex_fast_cache_entry *entry) {
	if (!entry->precomputed_mem)
		return;

	vgl_optimizer_leave_precomputed_state(gxm_context);
	for (int i = 0; i < 2; i++) {
		if (prepared_vertex_recent[i].precomputed_mem == entry->precomputed_mem)
			vgl_memset(&prepared_vertex_recent[i], 0, sizeof(prepared_vertex_recent[i]));
	}
	vglFree(entry->precomputed_mem);
	entry->precomputed_mem = NULL;
	entry->precomputed_ready = GL_FALSE;
}

static void release_precomputed_draw_cache(void) {
	for (int i = 0; i < PREPARED_VERTEX_FAST_CACHE_SIZE; i++)
		release_precomputed_draw(&prepared_vertex_fast_cache[i]);
}
#endif

void invalidate_program_variant_fast_cache(void) {
#ifdef HAVE_GXM_PRECOMPUTED_DRAW_CACHE
	release_precomputed_draw_cache();
#endif
#ifdef HAVE_GXM_PRECOMPUTED_FRAGMENT_STATE_CACHE
	release_precomputed_fragment_cache();
#endif
	vgl_memset(vertex_variant_fast_cache, 0, sizeof(vertex_variant_fast_cache));
	vgl_memset(fragment_variant_fast_cache, 0, sizeof(fragment_variant_fast_cache));
	vgl_memset(vertex_variant_recent, 0, sizeof(vertex_variant_recent));
	vgl_memset(fragment_variant_recent, 0, sizeof(fragment_variant_recent));
#ifdef HAVE_PREPARED_PACKED_VERTEX_CACHE
	vgl_memset(prepared_vertex_fast_cache, 0, sizeof(prepared_vertex_fast_cache));
	vgl_memset(prepared_vertex_recent, 0, sizeof(prepared_vertex_recent));
#ifdef HAVE_EXACT_PREPARED_DRAW_CACHE
	vgl_memset(prepared_draw_fast_cache, 0, sizeof(prepared_draw_fast_cache));
#endif
#endif
}

#ifdef HAVE_PREPARED_PACKED_VERTEX_CACHE
static inline __attribute__((always_inline)) uint32_t get_prepared_vertex_slot(program *p, vao *vertex_array, vbo *vertex_buffer) {
	uint32_t hash = ((uintptr_t)p >> 4) ^ ((uintptr_t)vertex_array >> 5) ^ ((uintptr_t)vertex_buffer >> 6);
	hash ^= vertex_array->generation * 0x9E3779B1U;
	hash ^= vertex_buffer->generation * 0x85EBCA6BU;
	return hash & (PREPARED_VERTEX_FAST_CACHE_SIZE - 1);
}

static inline __attribute__((always_inline)) GLboolean prepared_vertex_entry_matches(prepared_vertex_fast_cache_entry *entry, program *p, vbo *vertex_buffer) {
	return entry->gxm_program && entry->owner == p && entry->vertex_array == cur_vao && entry->vertex_buffer == vertex_buffer && entry->vao_generation == cur_vao->generation && entry->buffer_generation == vertex_buffer->generation;
}

static inline __attribute__((always_inline)) void remember_prepared_vertex_entry(prepared_vertex_fast_cache_entry *entry) {
	prepared_vertex_recent[1] = prepared_vertex_recent[0];
	prepared_vertex_recent[0] = *entry;
}

static inline __attribute__((always_inline)) prepared_vertex_fast_cache_entry *get_prepared_vertex(program *p, GLboolean instanced) {
	if (instanced || p->has_unaligned_attrs || p->attr_num < 2)
		return NULL;

	vbo *vertex_buffer = (vbo *)cur_vao->vertex_attrib_vbo[p->attr_map[0]];
	if (!vertex_buffer)
		return NULL;

	for (int i = 0; i < 2; i++) {
		if (prepared_vertex_entry_matches(&prepared_vertex_recent[i], p, vertex_buffer)) {
			if (i) {
				prepared_vertex_fast_cache_entry hit = prepared_vertex_recent[i];
				prepared_vertex_recent[1] = prepared_vertex_recent[0];
				prepared_vertex_recent[0] = hit;
			}
			return &prepared_vertex_recent[0];
		}
	}

	uint32_t slot = get_prepared_vertex_slot(p, cur_vao, vertex_buffer);
	prepared_vertex_fast_cache_entry *entry = &prepared_vertex_fast_cache[slot];
	if (!prepared_vertex_entry_matches(entry, p, vertex_buffer))
		return NULL;

	remember_prepared_vertex_entry(entry);
	return &prepared_vertex_recent[0];
}

static inline __attribute__((always_inline)) void cache_prepared_vertex(program *p, vbo *vertex_buffer, GLboolean is_packed, GLboolean instanced, SceGxmVertexProgram *gxm_program, SceGxmVertexStream *streams) {
	if (instanced || p->has_unaligned_attrs || p->attr_num < 2 || !is_packed || !vertex_buffer || !gxm_program)
		return;

	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		if (!(cur_vao->vertex_attrib_state & (1 << attr_idx)) || cur_vao->vertex_attrib_vbo[attr_idx] != (uint32_t)vertex_buffer)
			return;
	}

	uint32_t slot = get_prepared_vertex_slot(p, cur_vao, vertex_buffer);
	prepared_vertex_fast_cache_entry *entry = &prepared_vertex_fast_cache[slot];
#ifdef HAVE_GXM_PRECOMPUTED_DRAW_CACHE
	release_precomputed_draw(entry);
#endif
	entry->owner = p;
	entry->vertex_array = cur_vao;
	entry->vertex_buffer = vertex_buffer;
	entry->gxm_program = gxm_program;
	entry->vao_generation = cur_vao->generation;
	entry->buffer_generation = vertex_buffer->generation;
	entry->stride = streams[0].stride;
	entry->stream_count = p->attr_num;
#ifdef HAVE_GXM_PRECOMPUTED_DRAW_CACHE
	uint32_t precomputed_size = sceGxmGetPrecomputedDrawSize(gxm_program);
	entry->precomputed_mem = precomputed_size ? vglMemalign(16, precomputed_size) : NULL;
	if (entry->precomputed_mem && sceGxmPrecomputedDrawInit(&entry->precomputed_draw, gxm_program, entry->precomputed_mem) == SCE_OK)
		entry->precomputed_ready = GL_TRUE;
	else
		release_precomputed_draw(entry);
#endif
	remember_prepared_vertex_entry(entry);
}

#ifdef HAVE_EXACT_PREPARED_DRAW_CACHE
typedef uint32_t prepared_texture_word __attribute__((may_alias));

static inline __attribute__((always_inline)) GLboolean prepared_texture_matches(const SceGxmTexture *a, const SceGxmTexture *b) {
	const prepared_texture_word *left = (const prepared_texture_word *)a;
	const prepared_texture_word *right = (const prepared_texture_word *)b;
	return left[0] == right[0] && left[1] == right[1] && left[2] == right[2] && left[3] == right[3];
}

static inline __attribute__((always_inline)) GLboolean resolve_prepared_fragment_texture(program *p, texture **tex, sampler **smp) {
	*tex = NULL;
	*smp = NULL;
	if (!p->max_frag_texunit_idx)
		return GL_TRUE;
	if (p->max_frag_texunit_idx != 1 || !p->frag_texunits[0])
		return GL_FALSE;

	uniform *binding = p->frag_texunits[0];
	uint8_t unit = binding->sampler_index;
	texture_unit *tex_unit = &texture_units[unit];
	uint8_t tex_type = binding->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
	*tex = &texture_slots[tex_unit->tex_id[tex_type]];
	*smp = samplers[unit];
	return GL_TRUE;
}

static inline __attribute__((always_inline)) uint32_t get_prepared_draw_slot(program *p, texture *tex, sampler *smp, uint32_t blend_raw, uint16_t mode_key) {
#ifdef HAVE_HASHED_PREPARED_DRAW_CACHE
	uint32_t hash = ((uintptr_t)p >> 4) ^ ((uintptr_t)tex >> 5) ^ ((uintptr_t)smp >> 6);
	hash ^= blend_raw;
	hash ^= (uint32_t)mode_key * 0x9E3779B1U;
	hash ^= hash >> 16;
	hash *= 0x7FEB352DU;
	hash ^= hash >> 15;
	return hash & (PREPARED_DRAW_FAST_CACHE_SIZE - 1);
#else
	(void)p;
	(void)tex;
	(void)smp;
	(void)blend_raw;
	(void)mode_key;
	return 0;
#endif
}

static inline __attribute__((always_inline)) GLboolean prepared_draw_entry_matches(prepared_draw_cache_entry *entry, program *p, texture *tex, sampler *smp, uint16_t mode_key) {
	if (entry->owner != p || entry->blend_raw != blend_info.raw || entry->mode_key != mode_key)
		return GL_FALSE;
	if (entry->texture_count != p->max_frag_texunit_idx || entry->fragment_texture != tex || entry->fragment_sampler != smp)
		return GL_FALSE;
	return !tex || (entry->sampler_generation == (smp ? smp->generation : 0) && entry->texture_overridden == tex->overridden && prepared_texture_matches(&entry->fragment_descriptor, &tex->gxm_tex));
}

static inline __attribute__((always_inline)) GLboolean prepared_program_is_eligible(program *p) {
	if (p->max_vert_texunit_idx || p->vert_ubos || p->frag_ubos || p->max_frag_texunit_idx > 1)
		return GL_FALSE;
	for (int i = 0; i < p->vert_uniforms_num; i++) {
		if (p->vert_uniforms[i].type == UNIFORM_DATA)
			return GL_FALSE;
	}
	for (int i = 0; i < p->frag_uniforms_num; i++) {
		if (p->frag_uniforms[i].type == UNIFORM_DATA)
			return GL_FALSE;
	}
	return GL_TRUE;
}

typedef enum {
	PREPARED_DRAW_MISS,
	PREPARED_DRAW_READY,
	PREPARED_DRAW_SUBMITTED
} prepared_draw_result;

#ifdef HAVE_GXM_PRECOMPUTED_DRAW_CACHE
static inline __attribute__((always_inline)) GLboolean submit_precomputed_draw(prepared_vertex_fast_cache_entry *vertex, const void *stream_data, GLsizei count) {
	if (!vertex->precomputed_ready) {
		vgl_optimizer_precomputed_submission(GL_FALSE);
		return GL_FALSE;
	}

	if (vertex->precomputed_stream != stream_data) {
		for (int i = 0; i < vertex->stream_count; i++) {
			if (sceGxmPrecomputedDrawSetVertexStream(&vertex->precomputed_draw, i, stream_data) != SCE_OK) {
				vgl_optimizer_precomputed_submission(GL_FALSE);
				return GL_FALSE;
			}
		}
		vertex->precomputed_stream = stream_data;
	}
	if (vertex->precomputed_count != (uint32_t)count) {
		sceGxmPrecomputedDrawSetParams(&vertex->precomputed_draw, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, default_idx_ptr, count);
		vertex->precomputed_count = count;
	}

	return sceGxmDrawPrecomputed(gxm_context, &vertex->precomputed_draw) == SCE_OK;
}
#endif

static prepared_draw_result __attribute__((noinline, hot)) try_prepared_draw(program *p, prepared_vertex_fast_cache_entry *vertex, GLint first, GLsizei count, GLboolean allow_precomputed) {
	uint16_t mode_key = (uint16_t)(is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4) | ((uint16_t)msaa_mode << 8);
	texture *tex;
	sampler *smp;
	if (!resolve_prepared_fragment_texture(p, &tex, &smp)) {
		vgl_optimizer_prepared_draw(GL_FALSE);
		return PREPARED_DRAW_MISS;
	}
	uint32_t slot = get_prepared_draw_slot(p, tex, smp, blend_info.raw, mode_key);
	prepared_draw_cache_entry *entry = &prepared_draw_fast_cache[slot];
	if (prepared_draw_entry_matches(entry, p, tex, smp, mode_key)) {

		p->blend_info.raw = blend_info.raw;
		p->is_fbo_float = is_fbo_float;
		p->fprog = entry->fragment_program;
		vertex->vertex_buffer->last_frame = vgl_framecount;
		void *stream_data = (uint8_t *)vertex->vertex_buffer->ptr + first * vertex->stride;
#ifndef TEXTURES_SPEEDHACK
		if (tex)
			tex->last_frame = vgl_framecount;
#endif
		vgl_optimizer_prepared_draw(GL_TRUE);
#ifdef HAVE_GXM_PRECOMPUTED_FRAGMENT_STATE_CACHE
		if (allow_precomputed) {
			if (entry->precomputed_fragment_ready) {
				vgl_optimizer_precomputed_fragment_state(GL_TRUE);
				if (vertex->stream_count == 2)
					vgl_optimizer_apply_precomputed_prepared_draw2(gxm_context, &entry->precomputed_fragment_state, vertex->gxm_program, stream_data);
				else
					vgl_optimizer_apply_precomputed_prepared_draw(gxm_context, &entry->precomputed_fragment_state, vertex->gxm_program, stream_data, vertex->stream_count);
#ifdef HAVE_GXM_PRECOMPUTED_DRAW_CACHE
				if (submit_precomputed_draw(vertex, stream_data, count))
					return PREPARED_DRAW_SUBMITTED;
#endif
				vgl_optimizer_apply_prepared_draw(gxm_context, entry->fragment_program, tex ? &tex->gxm_tex : NULL, vertex->gxm_program, stream_data, vertex->stream_count);
				return PREPARED_DRAW_READY;
			}
			vgl_optimizer_precomputed_fragment_state(GL_FALSE);
		}
#endif
		vgl_optimizer_apply_prepared_draw(gxm_context, entry->fragment_program, tex ? &tex->gxm_tex : NULL, vertex->gxm_program, stream_data, vertex->stream_count);
#if defined(HAVE_GXM_PRECOMPUTED_DRAW_CACHE) && !defined(HAVE_GXM_PRECOMPUTED_FRAGMENT_STATE_CACHE)
		if (allow_precomputed && submit_precomputed_draw(vertex, stream_data, count))
			return PREPARED_DRAW_SUBMITTED;
#endif
		return PREPARED_DRAW_READY;
	}
	vgl_optimizer_prepared_draw(GL_FALSE);
	return PREPARED_DRAW_MISS;
}

static void __attribute__((noinline, cold)) cache_prepared_draw(program *p) {
	if (!prepared_program_is_eligible(p))
		return;

	texture *tex;
	sampler *smp;
	if (!resolve_prepared_fragment_texture(p, &tex, &smp))
		return;

	uint16_t mode_key = (uint16_t)(is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4) | ((uint16_t)msaa_mode << 8);
	uint32_t slot = get_prepared_draw_slot(p, tex, smp, blend_info.raw, mode_key);
	prepared_draw_cache_entry *entry = &prepared_draw_fast_cache[slot];
#ifdef HAVE_GXM_PRECOMPUTED_FRAGMENT_STATE_CACHE
	release_precomputed_fragment_entry(entry);
#endif
	vgl_memset(entry, 0, sizeof(*entry));
	entry->owner = p;
	entry->fragment_texture = tex;
	entry->fragment_sampler = smp;
	entry->fragment_program = p->fprog;
	entry->sampler_generation = smp ? smp->generation : 0;
	entry->blend_raw = blend_info.raw;
	entry->mode_key = mode_key;
	entry->texture_count = p->max_frag_texunit_idx;
	entry->texture_overridden = tex ? tex->overridden : GL_FALSE;
	if (tex)
		entry->fragment_descriptor = tex->gxm_tex;
#ifdef HAVE_GXM_PRECOMPUTED_FRAGMENT_STATE_CACHE
	uint32_t state_size = sceGxmGetPrecomputedFragmentStateSize(entry->fragment_program);
	entry->precomputed_fragment_mem = state_size ? vglMemalign(16, state_size) : NULL;
	if (entry->precomputed_fragment_mem && sceGxmPrecomputedFragmentStateInit(&entry->precomputed_fragment_state, entry->fragment_program, entry->precomputed_fragment_mem) == SCE_OK) {
		if (!tex || sceGxmPrecomputedFragmentStateSetTexture(&entry->precomputed_fragment_state, 0, &entry->fragment_descriptor) == SCE_OK)
			entry->precomputed_fragment_ready = GL_TRUE;
	}
	if (!entry->precomputed_fragment_ready)
		release_precomputed_fragment_entry(entry);
#endif
}
#endif
#endif

static inline __attribute__((always_inline)) GLboolean program_data_matches(const void *a, const void *b, size_t size) {
#ifdef DISABLE_CLIB
	return !memcmp(a, b, size);
#else
	return !sceClibMemcmp(a, b, size);
#endif
}

static inline __attribute__((always_inline)) uint32_t get_fragment_variant_slot(program *p, SceGxmOutputRegisterFormat output_format) {
	uint32_t hash = blend_info.raw ^ (blend_info.raw >> 16);
	hash ^= ((uint32_t)output_format << 8) | (uint32_t)msaa_mode;
	hash ^= (uintptr_t)p >> 4;
	hash *= 0x7FEB352D;
	return hash & (FRAGMENT_VARIANT_FAST_CACHE_SIZE - 1);
}

static inline __attribute__((always_inline)) uint32_t get_vertex_stream_key(const SceGxmVertexStream *streams, uint8_t stream_count) {
	uint32_t key = 0;
	for (int i = 0; i < stream_count; i++)
		key = (key * 33) ^ (uint32_t)streams[i].indexSource;
	return key;
}

static inline __attribute__((always_inline)) GLboolean vertex_fast_entry_matches(vertex_program_fast_cache_entry *entry, program *p, uint32_t stream_key, uint8_t attribute_count, uint8_t stream_count) {
	return entry->gxm_program && entry->owner == p && entry->vertex_array == cur_vao && entry->generation == cur_vao->generation && entry->stream_key == stream_key && entry->attribute_count == attribute_count && entry->stream_count == stream_count;
}

static inline __attribute__((always_inline)) void remember_vertex_fast_entry(vertex_program_fast_cache_entry *entry) {
	vertex_variant_recent[1] = vertex_variant_recent[0];
	vertex_variant_recent[0] = *entry;
}

static inline __attribute__((always_inline)) GLboolean fragment_fast_entry_matches(fragment_program_fast_cache_entry *entry, program *p, SceGxmOutputRegisterFormat output_format) {
	fragment_program_variant *variant = entry->variant;
	return entry->owner == p && variant && variant->output_format == output_format && variant->multisample_mode == msaa_mode && variant->blend_raw == blend_info.raw;
}

static inline __attribute__((always_inline)) GLboolean fragment_recent_entry_matches(fragment_program_recent_cache_entry *entry, program *p, SceGxmOutputRegisterFormat output_format) {
	uint16_t mode_key = (uint16_t)output_format | ((uint16_t)msaa_mode << 8);
	return entry->owner == p && entry->mode_key == mode_key && entry->blend_raw == blend_info.raw;
}

static inline __attribute__((always_inline)) void set_fragment_fast_entry(fragment_program_fast_cache_entry *entry, program *p, fragment_program_variant *variant) {
	entry->variant = variant;
	entry->owner = p;
}

static inline __attribute__((always_inline)) void set_fragment_recent_entry(fragment_program_recent_cache_entry *entry, program *p, fragment_program_variant *variant) {
	entry->gxm_program = variant->gxm_program;
	entry->blend_raw = variant->blend_raw;
	entry->mode_key = (uint16_t)variant->output_format | ((uint16_t)variant->multisample_mode << 8);
	entry->owner = p;
}

static inline __attribute__((always_inline)) void remember_fragment_fast_entry(fragment_program_fast_cache_entry *entry) {
	fragment_variant_recent[1] = fragment_variant_recent[0];
	set_fragment_recent_entry(&fragment_variant_recent[0], entry->owner, entry->variant);
}

static int get_vertex_program_variant(program *p, const SceGxmVertexAttribute *attributes, uint8_t attribute_count, const SceGxmVertexStream *streams, uint8_t stream_count, SceGxmVertexProgram **out) {
	vertex_program_fast_cache_entry *fast_entry = NULL;
	if (attributes == cur_vao->vertex_attrib_config) {
		uint32_t stream_key = get_vertex_stream_key(streams, stream_count);
		for (int i = 0; i < 2; i++) {
			if (vertex_fast_entry_matches(&vertex_variant_recent[i], p, stream_key, attribute_count, stream_count)) {
				vertex_program_fast_cache_entry hit = vertex_variant_recent[i];
				if (i) {
					vertex_variant_recent[1] = vertex_variant_recent[0];
					vertex_variant_recent[0] = hit;
				}
				*out = hit.gxm_program;
				vgl_optimizer_vertex_program(GL_TRUE);
				return SCE_OK;
			}
		}
		uint32_t slot = (((uintptr_t)p >> 4) ^ ((uintptr_t)cur_vao >> 5) ^ cur_vao->generation ^ stream_key) & (VERTEX_VARIANT_FAST_CACHE_SIZE - 1);
		fast_entry = &vertex_variant_fast_cache[slot];
		if (vertex_fast_entry_matches(fast_entry, p, stream_key, attribute_count, stream_count)) {
			remember_vertex_fast_entry(fast_entry);
			*out = fast_entry->gxm_program;
			vgl_optimizer_vertex_program(GL_TRUE);
			return SCE_OK;
		}
		fast_entry->owner = p;
		fast_entry->vertex_array = cur_vao;
		fast_entry->generation = cur_vao->generation;
		fast_entry->stream_key = stream_key;
		fast_entry->attribute_count = attribute_count;
		fast_entry->stream_count = stream_count;
		fast_entry->gxm_program = NULL;
	}

	vertex_program_variant *variant = p->vertex_variants;
	vertex_program_variant *previous = NULL;
	size_t attributes_size = attribute_count * sizeof(SceGxmVertexAttribute);
	size_t streams_size = stream_count * sizeof(SceGxmVertexStream);

	while (variant) {
		if (variant->attribute_count == attribute_count && variant->stream_count == stream_count &&
			program_data_matches(variant->attributes, attributes, attributes_size) &&
			program_data_matches(variant->streams, streams, streams_size)) {
			if (previous) {
				previous->next = variant->next;
				variant->next = p->vertex_variants;
				p->vertex_variants = variant;
			}
			if (fast_entry) {
				fast_entry->gxm_program = variant->gxm_program;
				remember_vertex_fast_entry(fast_entry);
			}
			*out = variant->gxm_program;
			vgl_optimizer_vertex_program(GL_TRUE);
			return SCE_OK;
		}
		previous = variant;
		variant = variant->next;
	}

	variant = (vertex_program_variant *)vglMalloc(sizeof(vertex_program_variant));
	if (!variant) {
		vgl_optimizer_vertex_program(GL_FALSE);
		return sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher, p->vshader->id, attributes, attribute_count, streams, stream_count, out);
	}

	int ret = sceGxmShaderPatcherCreateVertexProgram(gxm_shader_patcher, p->vshader->id, attributes, attribute_count, streams, stream_count, &variant->gxm_program);
	if (ret) {
#ifdef LOG_ERRORS
		vgl_log("Vertex shader variant creation failed (%s) with %d attributes and %d streams.\n", get_gxm_error_literal(ret), attribute_count, stream_count);
#endif
		vgl_free(variant);
		return ret;
	}

	variant->attribute_count = attribute_count;
	variant->stream_count = stream_count;
	vgl_fast_memcpy(variant->attributes, attributes, attributes_size);
	vgl_fast_memcpy(variant->streams, streams, streams_size);
	variant->next = p->vertex_variants;
	p->vertex_variants = variant;
	if (fast_entry) {
		fast_entry->gxm_program = variant->gxm_program;
		remember_vertex_fast_entry(fast_entry);
	}
	*out = variant->gxm_program;
	vgl_optimizer_vertex_program(GL_FALSE);
	return SCE_OK;
}

static int __attribute__((noinline, cold)) get_fragment_program_variant_slow(program *p, SceGxmOutputRegisterFormat output_format, SceGxmFragmentProgram **out) {
	uint32_t slot = get_fragment_variant_slot(p, output_format);
	fragment_program_fast_cache_entry *fast_entry = &fragment_variant_fast_cache[slot];
	fragment_program_variant *variant = fast_entry->variant;
	if (fragment_fast_entry_matches(fast_entry, p, output_format)) {
		remember_fragment_fast_entry(fast_entry);
		*out = variant->gxm_program;
		vgl_optimizer_fragment_program(GL_TRUE);
		return SCE_OK;
	}

	variant = p->fragment_variants;
	fragment_program_variant *previous = NULL;

	while (variant) {
		if (variant->output_format == output_format && variant->multisample_mode == msaa_mode && variant->blend_raw == blend_info.raw) {
			if (previous) {
				previous->next = variant->next;
				variant->next = p->fragment_variants;
				p->fragment_variants = variant;
			}
			set_fragment_fast_entry(fast_entry, p, variant);
			remember_fragment_fast_entry(fast_entry);
			*out = variant->gxm_program;
			vgl_optimizer_fragment_program(GL_TRUE);
			return SCE_OK;
		}
		previous = variant;
		variant = variant->next;
	}

	variant = (fragment_program_variant *)vglMalloc(sizeof(fragment_program_variant));
	if (!variant) {
		vgl_optimizer_fragment_program(GL_FALSE);
		return sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher, p->fshader->id, output_format, msaa_mode, &blend_info.info, (SceGxmProgram *)p->vshader->prog, out);
	}

	int ret = sceGxmShaderPatcherCreateFragmentProgram(gxm_shader_patcher, p->fshader->id, output_format, msaa_mode, &blend_info.info, (SceGxmProgram *)p->vshader->prog, &variant->gxm_program);
	if (ret) {
#ifdef LOG_ERRORS
		vgl_log("Fragment shader variant creation failed (%s).\n", get_gxm_error_literal(ret));
#endif
		vgl_free(variant);
		return ret;
	}

	variant->output_format = output_format;
	variant->multisample_mode = msaa_mode;
	variant->blend_raw = blend_info.raw;
	variant->next = p->fragment_variants;
	p->fragment_variants = variant;
	set_fragment_fast_entry(fast_entry, p, variant);
	remember_fragment_fast_entry(fast_entry);
	*out = variant->gxm_program;
	vgl_optimizer_fragment_program(GL_FALSE);
	return SCE_OK;
}

#ifdef HAVE_COMPACT_FRAGMENT_VARIANT_CACHE
static int __attribute__((noinline, hot)) get_fragment_program_variant(program *p, SceGxmOutputRegisterFormat output_format, SceGxmFragmentProgram **out) {
	for (int i = 0; i < 2; i++) {
		if (fragment_recent_entry_matches(&fragment_variant_recent[i], p, output_format)) {
			*out = fragment_variant_recent[i].gxm_program;
			vgl_optimizer_fragment_program(GL_TRUE);
			return SCE_OK;
		}
	}
	return get_fragment_program_variant_slow(p, output_format, out);
}
#else
#define get_fragment_program_variant get_fragment_program_variant_slow
#endif

static void release_program_variants(program *p) {
	invalidate_program_variant_fast_cache();
	while (p->vertex_variants) {
		vertex_program_variant *variant = p->vertex_variants;
		p->vertex_variants = variant->next;
		sceGxmShaderPatcherReleaseVertexProgram(gxm_shader_patcher, variant->gxm_program);
		vgl_free(variant);
	}
	while (p->fragment_variants) {
		fragment_program_variant *variant = p->fragment_variants;
		p->fragment_variants = variant->next;
		sceGxmShaderPatcherReleaseFragmentProgram(gxm_shader_patcher, variant->gxm_program);
		vgl_free(variant);
	}
	p->vprog = NULL;
	p->fprog = NULL;
}
#endif

#ifdef HAVE_SAMPLER_STATE_CACHE
#define SAMPLER_STATE_CACHE_SIZE 256

typedef struct {
	texture *tex;
	uint32_t sampler_generation;
	SceGxmTexture descriptor;
	GLboolean use_lod_bias;
} sampler_state_cache_entry;

static sampler_state_cache_entry sampler_state_cache[SAMPLER_STATE_CACHE_SIZE];

typedef uint32_t sampler_texture_word __attribute__((may_alias));

static inline __attribute__((always_inline)) GLboolean sampler_descriptor_matches(sampler_state_cache_entry *cache, texture *tex) {
	const sampler_texture_word *left = (const sampler_texture_word *)&cache->descriptor;
	const sampler_texture_word *right = (const sampler_texture_word *)&tex->gxm_tex;
	return left[0] == right[0] && left[1] == right[1] && left[2] == right[2] && left[3] == right[3];
}

static inline __attribute__((always_inline)) sampler_state_cache_entry *get_sampler_state_cache(texture *tex) {
	return &sampler_state_cache[((uintptr_t)tex >> 4) & (SAMPLER_STATE_CACHE_SIZE - 1)];
}
#endif

static inline __attribute__((always_inline)) void apply_sampler_state(texture *tex, sampler *smp, GLboolean use_lod_bias) {
#ifdef HAVE_SAMPLER_STATE_CACHE
	sampler_state_cache_entry *cache = get_sampler_state_cache(tex);
	if (smp) {
		if (tex->overridden && cache->tex == tex && cache->sampler_generation == smp->generation && cache->use_lod_bias == use_lod_bias && sampler_descriptor_matches(cache, tex)) {
			vgl_optimizer_sampler_state(GL_TRUE);
			return;
		}
	} else if (!tex->overridden) {
		vgl_optimizer_sampler_state(GL_TRUE);
		return;
	}
	vgl_optimizer_sampler_state(GL_FALSE);
#endif

	if (smp) {
		vglSetTexMinFilter(&tex->gxm_tex, smp->min_filter);
		vglSetTexMipFilter(&tex->gxm_tex, smp->mip_filter);
		vglSetTexMagFilter(&tex->gxm_tex, smp->mag_filter);
		vglSetTexUMode(&tex->gxm_tex, smp->u_mode);
		vglSetTexVMode(&tex->gxm_tex, smp->v_mode);
		vglSetTexMipmapCount(&tex->gxm_tex, smp->use_mips ? tex->mip_count : 0);
		if (use_lod_bias)
			vglSetTexLodBias(&tex->gxm_tex, smp->lod_bias);
		tex->overridden = GL_TRUE;
#ifdef HAVE_SAMPLER_STATE_CACHE
		cache->tex = tex;
		cache->sampler_generation = smp->generation;
		vgl_memcpy(&cache->descriptor, &tex->gxm_tex, sizeof(SceGxmTexture));
		cache->use_lod_bias = use_lod_bias;
#endif
	} else if (tex->overridden) {
		vglSetTexMinFilter(&tex->gxm_tex, tex->min_filter);
		vglSetTexMipFilter(&tex->gxm_tex, tex->mip_filter);
		vglSetTexMagFilter(&tex->gxm_tex, tex->mag_filter);
		vglSetTexUMode(&tex->gxm_tex, tex->u_mode);
		vglSetTexVMode(&tex->gxm_tex, tex->v_mode);
		vglSetTexMipmapCount(&tex->gxm_tex, tex->use_mips ? tex->mip_count : 0);
		if (use_lod_bias)
			vglSetTexLodBias(&tex->gxm_tex, tex->lod_bias);
		tex->overridden = GL_FALSE;
#ifdef HAVE_SAMPLER_STATE_CACHE
		if (cache->tex == tex)
			cache->tex = NULL;
#endif
	}
}

#ifdef HAVE_SHARK_LOG
static char *shark_log = NULL;
#endif

#ifdef STRICT_UNIFORMS_COMPLIANCE
static inline __attribute__((always_inline)) uniform *get_uniform_from_ptr(GLint ptr, uint32_t *offset) {
	uniform_location u;
	u.raw = ptr;
	uniform *ret = u.is_vertex ? &progs[u.program_idx].vert_uniforms[u.uniform_idx] : &progs[u.program_idx].frag_uniforms[u.uniform_idx];
	*offset = u.offset;
	return ret;
}
#else
#define get_uniform_from_ptr(ptr, offs) (-ptr)
#endif

void release_shader(shader *s) {
	// Deallocating shader and unregistering it from sceGxmShaderPatcher
	if (s->valid) {
		if (s->prog) {
			sceGxmShaderPatcherForceUnregisterProgram(gxm_shader_patcher, s->id);
			vgl_free((void *)s->prog);
			while (s->mat) {
				matrix_uniform *m = (matrix_uniform *)s->mat->chain;
				vgl_free(s->mat);
				s->mat = m;
			}
			while (s->unif_blk) {
				block_uniform *b = (block_uniform *)s->unif_blk->chain;
				vgl_free(s->unif_blk);
				s->unif_blk = b;
			}
#ifdef HAVE_SHARK_LOG
			if (s->log) {
				vgl_free(s->log);
				s->log = NULL;
			}
#endif
		}
	}
	if (s->source) {
		vgl_free(s->source);
		s->source = NULL;
	}
	s->valid = GL_FALSE;
	s->dirty = GL_FALSE;
}

float *reserve_attrib_pool(uint8_t count) {
	float *res = cur_vao->vertex_attrib_pool_ptr;
	cur_vao->vertex_attrib_pool_ptr += count;
	if (cur_vao->vertex_attrib_pool_ptr > cur_vao->vertex_attrib_pool_limit) {
		cur_vao->vertex_attrib_pool_ptr = cur_vao->vertex_attrib_pool;
		return cur_vao->vertex_attrib_pool_ptr;
	}
	return res;
}

static inline __attribute__((always_inline)) GLenum gxm_vd_fmt_to_gl(SceGxmAttributeFormat fmt) {
	switch (fmt) {
	case SCE_GXM_ATTRIBUTE_FORMAT_F16:
		return GL_HALF_FLOAT;
	case SCE_GXM_ATTRIBUTE_FORMAT_F32:
		return GL_FLOAT;
	case SCE_GXM_ATTRIBUTE_FORMAT_S16:
	case SCE_GXM_ATTRIBUTE_FORMAT_S16N:
		return GL_SHORT;
	case SCE_GXM_ATTRIBUTE_FORMAT_U16:
	case SCE_GXM_ATTRIBUTE_FORMAT_U16N:
		return GL_UNSIGNED_SHORT;
	case SCE_GXM_ATTRIBUTE_FORMAT_S8:
	case SCE_GXM_ATTRIBUTE_FORMAT_S8N:
		return GL_BYTE;
	case SCE_GXM_ATTRIBUTE_FORMAT_U8:
	case SCE_GXM_ATTRIBUTE_FORMAT_U8N:
		return GL_UNSIGNED_BYTE;
	default:
		return GL_FLOAT;
	}
}

static inline __attribute__((always_inline)) GLenum gxm_attr_type_to_gl(uint8_t size, uint8_t num) {
	switch (size * num) {
	case 1:
		return GL_FLOAT;
	case 2:
		return GL_FLOAT_VEC2;
	case 3:
		return GL_FLOAT_VEC3;
	case 4:
		return GL_FLOAT_VEC4;
	case 9:
		return GL_FLOAT_MAT3;
	case 16:
		return GL_FLOAT_MAT4;
	default:
		return GL_FLOAT;
	}
}

static inline __attribute__((always_inline)) GLenum gxm_unif_type_to_gl(SceGxmParameterType type, uint8_t count, int *size) {
	switch (type) {
	case SCE_GXM_PARAMETER_TYPE_F32:
	case SCE_GXM_PARAMETER_TYPE_F16:
	case SCE_GXM_PARAMETER_TYPE_C10:
		switch (count) {
		case 1:
			return GL_FLOAT;
		case 2:
			return GL_FLOAT_VEC2;
		case 3:
			return GL_FLOAT_VEC3;
		case 4:
			return GL_FLOAT_VEC4;
		default:
			return GL_FLOAT;
		}
	case SCE_GXM_PARAMETER_TYPE_U32:
	case SCE_GXM_PARAMETER_TYPE_S32:
		switch (count) {
		case 1:
			return GL_INT;
		case 2:
			return GL_INT_VEC2;
		case 3:
			return GL_INT_VEC3;
		case 4:
			return GL_INT_VEC4;
		default:
			return GL_INT;
		}
	default:
		return GL_FLOAT;
	}
}

static inline __attribute__((always_inline)) void gxm_unif_to_mat(GLenum *type, int *size) {
	switch (*type) {
	case GL_FLOAT_VEC2:
		*type = GL_FLOAT_MAT2;
		*size = *size / 2;
		break;
	case GL_FLOAT_VEC3:
		*type = GL_FLOAT_MAT3;
		*size = *size / 3;
		break;
	case GL_FLOAT_VEC4:
		*type = GL_FLOAT_MAT4;
		*size = *size / 4;
		break;
	default:
		break;
	}
}

static inline __attribute__((always_inline)) size_t serialized_shader_size(shader *s, GLboolean have_bindings, uint32_t *matrix_uniforms_num) {
	*matrix_uniforms_num = 0;
	matrix_uniform *m = s->mat;
	while (m) {
		*matrix_uniforms_num = *matrix_uniforms_num + 1;
		m = (matrix_uniform *)m->chain;
	}
	size_t sz = (1 + *matrix_uniforms_num) * sizeof(uint32_t) + s->size;
	if (have_bindings)
		sz += sizeof(binds_map);
#ifdef HAVE_GLSL_TEXTURE_SIZE
	sz += sizeof(glsl_samplers_info) * s->sized_samplers_num + sizeof(uint8_t);
#endif
	return sz;
}

void *serialize_shader(void *out, size_t *sz, shader *s, GLboolean save_bindings) {
	uint32_t matrix_uniforms_num;
	*sz = serialized_shader_size(s, save_bindings, &matrix_uniforms_num);
	void *_out = out;
	if (!_out) {
		_out = vglMalloc(*sz);
	}
	uint8_t *buf = (uint8_t *)_out;
	vgl_fast_memcpy(buf, &matrix_uniforms_num, sizeof(uint32_t));
	buf += sizeof(uint32_t);
	matrix_uniform *m = s->mat;
	while (m) {
		uint32_t idx = sceGxmProgramParameterGetIndex(s->prog, m->ptr);
		vgl_fast_memcpy(buf, &idx, sizeof(uint32_t));
		buf += sizeof(uint32_t);
		m = (matrix_uniform *)m->chain;
	}
	if (save_bindings) {
		vgl_fast_memcpy(buf, &s->semantics, sizeof(binds_map));
		buf += sizeof(binds_map);
	}
#ifdef HAVE_GLSL_TEXTURE_SIZE
	*buf = s->sized_samplers_num;
	buf++;
	if (s->sized_samplers_num) {
		vgl_fast_memcpy(buf, s->sized_samplers, sizeof(glsl_samplers_info) * s->sized_samplers_num);
		buf += sizeof(glsl_samplers_info) * s->sized_samplers_num;
	}
#endif
	vgl_fast_memcpy(buf, s->prog, s->size);
	return _out;
}

void unserialize_shader(void *in, size_t sz, shader *s, GLboolean load_bindings) {
	uint8_t *buf = (uint8_t *)in;
	uint32_t matrix_uniforms_num;
	vgl_fast_memcpy(&matrix_uniforms_num, buf, sizeof(uint32_t));
	buf += sizeof(uint32_t) * (matrix_uniforms_num + 1);
	if (load_bindings) {
		vgl_fast_memcpy(&s->semantics, buf, sizeof(binds_map));
		buf += sizeof(binds_map);
	}
#ifdef HAVE_GLSL_TEXTURE_SIZE
	s->sized_samplers_num = *buf;
	buf++;
	if (s->sized_samplers_num) {
		vgl_fast_memcpy(s->sized_samplers, buf, sizeof(glsl_samplers_info) * s->sized_samplers_num);
		buf += sizeof(glsl_samplers_info) * s->sized_samplers_num;
	}
#endif
	s->size = sz - ((uintptr_t)buf - (uintptr_t)in);
	s->prog = (SceGxmProgram *)vglMalloc(s->size);
	vgl_fast_memcpy((SceGxmProgram *)s->prog, buf, s->size);
	sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, s->prog, &s->id);
	s->unif_buf_size = sceGxmProgramGetDefaultUniformBufferSize(s->prog);
	if (matrix_uniforms_num) {
		uint32_t *_m = (uint32_t *)in + 1;
		uint32_t *ptr = vglProgramGetParameterBase(s->prog);
		for (int i = 0; i < matrix_uniforms_num; i++) {
			matrix_uniform *m = vglMalloc(sizeof(matrix_uniform));
			m->chain = s->mat;
			m->ptr = (SceGxmProgramParameter *)(ptr + _m[i] * 4);
			s->mat = m;
		}
	}
}

#ifdef HAVE_SHADER_CACHE
#define vgl_compile_shader(shd, sv) compile_shader(shd, sv, fname)
static inline __attribute__((always_inline)) void compile_shader(shader *s, GLboolean save_bindings, const char *cache_fname) {
#else
#define vgl_compile_shader(shd, sv) compile_shader(shd, sv)
static inline __attribute__((always_inline)) void compile_shader(shader *s, GLboolean save_bindings) {
#endif
	// Compiling shader source
	const char *shader_source = (const char *)s->source;
	uint32_t shader_size = s->size;
#ifdef HAVE_UNSAFE_SHADER_LOW_PRECISION
	char *low_precision_source = NULL;
	if (!s->is_glsl && s->type == GL_FRAGMENT_SHADER) {
		static const char low_precision_preamble[] =
			"#define float half\n"
			"#define float2 half2\n"
			"#define float3 half3\n"
			"#define float4 half4\n"
			"#define float2x2 half2x2\n"
			"#define float3x3 half3x3\n"
			"#define float4x4 half4x4\n";
		uint32_t preamble_size = sizeof(low_precision_preamble) - 1;
		low_precision_source = (char *)vglMalloc(preamble_size + shader_size + 1);
		if (low_precision_source) {
			vgl_fast_memcpy(low_precision_source, low_precision_preamble, preamble_size);
			vgl_fast_memcpy(low_precision_source + preamble_size, shader_source, shader_size);
			low_precision_source[preamble_size + shader_size] = 0;
			shader_source = low_precision_source;
			shader_size += preamble_size;
		}
	}
#endif
	s->prog = shark_compile_shader_extended(shader_source, &shader_size, s->type == GL_FRAGMENT_SHADER ? SHARK_FRAGMENT_SHADER : SHARK_VERTEX_SHADER, compiler_opts, compiler_fastmath, compiler_fastprecision, compiler_fastint);
#ifdef HAVE_UNSAFE_SHADER_LOW_PRECISION
	if (low_precision_source)
		vgl_free(low_precision_source);
#endif
	s->size = shader_size;
	if (s->prog) {
		vgl_free(s->source);
		s->source = NULL;
		SceGxmProgram *res = (SceGxmProgram *)vglMalloc(s->size);
		vgl_fast_memcpy((void *)res, (void *)s->prog, s->size);
		int r = sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, res, &s->id);
#ifdef LOG_ERRORS
		if (r) {
			vgl_log("%s:%d %s: Program failed to register on sceGxm (%s).\n", __FILE__, __LINE__, __func__, get_gxm_error_literal(r));
		}
#endif
		s->unif_buf_size = sceGxmProgramGetDefaultUniformBufferSize(res);
		s->prog = res;
		SceShaccCgCompileOutput *cout = (SceShaccCgCompileOutput *)shark_get_internal_compile_output();
		SceShaccCgParameter param = sceShaccCgGetFirstParameter(cout);
		while (param) {
			if (sceShaccCgGetParameterClass(param) == SCE_SHACCCG_PARAMETERCLASS_MATRIX) {
				matrix_uniform *m = (matrix_uniform *)vglMalloc(sizeof(matrix_uniform));
				m->ptr = sceGxmProgramFindParameterByName(s->prog, sceShaccCgGetParameterName(param));
				m->chain = s->mat;
				s->mat = m;
			} else if (sceShaccCgGetParameterClass(param) == SCE_SHACCCG_PARAMETERCLASS_UNIFORMBLOCK) {
				block_uniform *b = (block_uniform *)vglMalloc(sizeof(block_uniform));
				b->idx = sceShaccCgGetParameterBufferIndex(param);
				strcpy(b->name, sceShaccCgGetParameterName(param));
				b->chain = s->unif_blk;
				s->unif_blk = b;
			}
			param = sceShaccCgGetNextParameter(param);
		}
	}
#ifdef HAVE_SHARK_LOG
	if (s->log)
		vgl_free(s->log);
	s->log = shark_log;
	shark_log = NULL;
#endif
	shark_clear_output();
#ifdef HAVE_SHADER_CACHE
	SceUID f = sceIoOpen(cache_fname, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
	size_t sz;
	void *buf = serialize_shader(NULL, &sz, s, save_bindings);
	sceIoWrite(f, buf, sz);
	sceIoClose(f);
	vgl_free(buf);
#endif
}

void reset_custom_shaders(void) {
#ifdef HAVE_PROGRAM_VARIANT_CACHE
	invalidate_program_variant_fast_cache();
#endif

	// Init custom shaders
	for (int i = 0; i < MAX_CUSTOM_SHADERS; i++) {
		shaders[i].valid = GL_FALSE;
#ifdef HAVE_SHARK_LOG
		shaders[i].log = NULL;
#endif
		shaders[i].source = NULL;
	}

	// Init custom programs
	for (int i = 0; i < MAX_CUSTOM_PROGRAMS; i++) {
		progs[i].status = PROG_INVALID;
	}
}

void _glMultiDrawArrays_CustomShadersIMPL(SceGxmPrimitiveType gxm_p, uint16_t *idx_ptr, const GLint *first, const GLsizei *count, GLint lowest, GLsizei highest, GLsizei drawcount) {
#ifdef HAVE_PROFILING
	uint32_t draw_start = sceKernelGetProcessTimeLow();
#endif
	program *p = &progs[cur_program - 1];

	// Check if a blend info rebuild is required and upload fragment program
	setup_frag_program();

	// Uploading fragment textures on relative texture units
	for (int i = 0; i < p->max_frag_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->frag_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->frag_texunits[i]->sampler_index];
			uint8_t tex_type = p->frag_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifdef HAVE_TEX_CACHE
			restore_tex_cache(tex);
#endif
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->frag_texunits[i]->sampler_index];
			apply_sampler_state(tex, smp, GL_TRUE);
			sceGxmSetFragmentTexture(gxm_context, i, &tex->gxm_tex);
#ifdef HAVE_GLSL_TEXTURE_SIZE
			glsl_samplers_info *info = p->frag_texunits[i]->sampler;
			if (info) {
				uint32_t sizes[2];
				vglGetTexSizes(&tex->gxm_tex, &sizes[0], &sizes[1]);
				float fsizes[2] = {sizes[0], sizes[1]};
				vglSetUniformData(p->frag_texunits[i]->fptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 2, fsizes, SCE_GXM_PARAMETER_TYPE_F32);
				dirty_shader_frag_unifs = GL_TRUE;
			}
#endif
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// Uploading vertex textures on relative texture units
	for (int i = 0; i < p->max_vert_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->vert_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->vert_texunits[i]->sampler_index];
			uint8_t tex_type = p->vert_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->vert_texunits[i]->sampler_index];
			apply_sampler_state(tex, smp, GL_FALSE);
			sceGxmSetVertexTexture(gxm_context, i, &tex->gxm_tex);
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// Aligning attributes
	SceGxmVertexAttribute *attributes;
	SceGxmVertexStream *streams;
	align_attributes(attributes, streams);

	void *ptrs[VERTEX_ATTRIBS_NUM];
	vbo *target_vbo = (vbo *)cur_vao->vertex_attrib_vbo[p->attr_map[0]];
#ifdef STRICT_DRAW_COMPLIANCE
	GLboolean is_packed[VERTEX_ATTRIBS_NUM];
	vgl_memset(is_packed, GL_TRUE, p->attr_num * sizeof(GLboolean));
#else
	GLboolean is_packed = p->attr_num > 1;
	if (is_packed) {
#endif
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			vbo *attr_vbo = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx];
			if (attr_vbo) {
#ifdef STRICT_DRAW_COMPLIANCE
				vgl_memset(is_packed, 0, p->attr_num * sizeof(GLboolean));
				break;
#else
				if (attr_vbo != target_vbo) {
					is_packed = GL_FALSE;
					break;
				}
#endif
#ifdef STRICT_DRAW_COMPLIANCE
			} else {
				if (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[attr_idx] && cur_vao->vertex_attrib_offsets[attr_idx] >= cur_vao->vertex_attrib_offsets[p->attr_map[0]])) {
					is_packed[attr_idx] = GL_FALSE;
				}
#endif
			}
		}
#ifndef STRICT_DRAW_COMPLIANCE
		if (is_packed && (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[p->attr_map[1]] && cur_vao->vertex_attrib_offsets[p->attr_map[1]] > cur_vao->vertex_attrib_offsets[p->attr_map[0]])))
			is_packed = GL_FALSE;
	}
#endif
#ifdef STRICT_DRAW_COMPLIANCE
	// Gathering real attribute data pointers
	if (is_packed[0]) {
		ptrs[0] = gpu_alloc_mapped_temp(highest * streams[0].stride);
		vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + lowest * streams[0].stride, highest * streams[0].stride);
	}
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		attributes[i].regIndex = p->attr[attr_idx].regIndex;
		if (is_packed[i]) {
			handle_packed_attrib();
		} else {
			handle_unpacked_attrib(lowest, highest);
		}
	}
#else
	// Gathering real attribute data pointers
	if (is_packed) {
		if (target_vbo) {
			ptrs[0] = (void *)target_vbo->ptr + lowest * streams[0].stride;
			target_vbo->last_frame = vgl_framecount;
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_vbo_attrib();
			}
		} else {
			ptrs[0] = gpu_alloc_mapped_temp((highest - lowest) * streams[0].stride);
			vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + lowest * streams[0].stride, (highest - lowest) * streams[0].stride);		
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_attrib();
			}
		}
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			attributes[i].regIndex = p->attr[attr_idx].regIndex;
			handle_unpacked_attrib(lowest, highest);
		}
	}
#endif

#ifndef INDICES_SPEEDHACK
	for (int i = 0; i < p->attr_num; i++) {
		streams[i].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	}
#endif

	// Uploading new vertex program
#ifdef HAVE_PROGRAM_VARIANT_CACHE
	get_vertex_program_variant(p, attributes, p->attr_num, streams, p->attr_num, &p->vprog);
#else
	patch_vertex_program(gxm_shader_patcher, p->vshader->id, attributes, p->attr_num, streams, p->attr_num, &p->vprog);
#endif
	sceGxmSetVertexProgram(gxm_context, p->vprog);

	// Uploading both fragment and vertex uniforms data
	upload_uniforms();
	
	for (int j = 0; j < drawcount; j++) {
		// Uploading vertex streams
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			GLboolean is_active = (cur_vao->vertex_attrib_state & (1 << attr_idx)) ? GL_TRUE : GL_FALSE;
			if (is_active) {
#ifdef STRICT_DRAW_COMPLIANCE
				if (is_packed[i])
					sceGxmSetVertexStream(gxm_context, i, ptrs[0] + (first[j] - lowest) * streams[0].stride);
				else
					sceGxmSetVertexStream(gxm_context, i, ptrs[i] + (first[j] - lowest) * streams[i].stride);
#else
				if (is_packed)
					sceGxmSetVertexStream(gxm_context, i, ptrs[0] + (first[j] - lowest) * streams[0].stride);
				else
					sceGxmSetVertexStream(gxm_context, i, ptrs[i] + (first[j] - lowest) * streams[i].stride);
#endif
			}
		}
		
		sceGxmDraw(gxm_context, gxm_p, SCE_GXM_INDEX_FORMAT_U16, idx_ptr, count[j]);
	}
#ifdef HAVE_PROFILING
	shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
	shaders_draw_cnt++;
#endif
}

GLboolean _glDrawArrays_CustomShadersIMPL(GLint first, GLsizei count, GLboolean instanced, GLboolean allow_precomputed) {
#ifdef HAVE_PROFILING
	uint32_t draw_start = sceKernelGetProcessTimeLow();
#endif
	program *p = &progs[cur_program - 1];

#ifdef HAVE_PREPARED_PACKED_VERTEX_CACHE
	prepared_vertex_fast_cache_entry *prepared_vertex = get_prepared_vertex(p, instanced);
#ifdef HAVE_EXACT_PREPARED_DRAW_CACHE
	if (prepared_vertex) {
		prepared_draw_result prepared_result = try_prepared_draw(p, prepared_vertex, first, count, allow_precomputed);
		if (prepared_result != PREPARED_DRAW_MISS)
			return prepared_result == PREPARED_DRAW_READY;
	}
#endif
#endif

	// Check if a blend info rebuild is required and upload fragment program
	setup_frag_program();

	// Uploading fragment textures on relative texture units
	for (int i = 0; i < p->max_frag_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->frag_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->frag_texunits[i]->sampler_index];
			uint8_t tex_type = p->frag_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifdef HAVE_TEX_CACHE
			restore_tex_cache(tex);
#endif
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->frag_texunits[i]->sampler_index];
			apply_sampler_state(tex, smp, GL_TRUE);
			sceGxmSetFragmentTexture(gxm_context, i, &tex->gxm_tex);
#ifdef HAVE_GLSL_TEXTURE_SIZE
			glsl_samplers_info *info = p->frag_texunits[i]->sampler;
			if (info) {
				uint32_t sizes[2];
				vglGetTexSizes(&tex->gxm_tex, &sizes[0], &sizes[1]);
				float fsizes[2] = {sizes[0], sizes[1]};
				vglSetUniformData(p->frag_texunits[i]->fptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 2, fsizes, SCE_GXM_PARAMETER_TYPE_F32);
				dirty_shader_frag_unifs = GL_TRUE;
			}
#endif
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// Uploading vertex textures on relative texture units
	for (int i = 0; i < p->max_vert_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->vert_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->vert_texunits[i]->sampler_index];
			uint8_t tex_type = p->vert_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->vert_texunits[i]->sampler_index];
			apply_sampler_state(tex, smp, GL_FALSE);
			sceGxmSetVertexTexture(gxm_context, i, &tex->gxm_tex);
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

#ifdef HAVE_PREPARED_PACKED_VERTEX_CACHE
	if (prepared_vertex) {
		prepared_vertex->vertex_buffer->last_frame = vgl_framecount;
		void *stream_data = (uint8_t *)prepared_vertex->vertex_buffer->ptr + first * prepared_vertex->stride;
		sceGxmSetVertexProgram(gxm_context, prepared_vertex->gxm_program);
		upload_uniforms();
		for (int i = 0; i < prepared_vertex->stream_count; i++)
			sceGxmSetVertexStream(gxm_context, i, stream_data);
#ifdef HAVE_EXACT_PREPARED_DRAW_CACHE
		cache_prepared_draw(p);
#endif
		return GL_TRUE;
	}
#endif

	// Aligning attributes
	SceGxmVertexAttribute *attributes;
	SceGxmVertexStream *streams;
	align_attributes(attributes, streams);

	void *ptrs[VERTEX_ATTRIBS_NUM];
#ifndef DRAW_SPEEDHACK
	vbo *target_vbo = (vbo *)cur_vao->vertex_attrib_vbo[p->attr_map[0]];
#ifdef STRICT_DRAW_COMPLIANCE
	GLboolean is_packed[VERTEX_ATTRIBS_NUM];
	vgl_memset(is_packed, GL_TRUE, p->attr_num * sizeof(GLboolean));
#else
	GLboolean is_packed = p->attr_num > 1;
	if (is_packed) {
#endif
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			vbo *attr_vbo = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx];
			if (attr_vbo) {
#ifdef STRICT_DRAW_COMPLIANCE
				vgl_memset(is_packed, 0, p->attr_num * sizeof(GLboolean));
				break;
#else
				if (attr_vbo != target_vbo) {
					is_packed = GL_FALSE;
					break;
				}
#endif
#ifdef STRICT_DRAW_COMPLIANCE
			} else {
				if (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[attr_idx] && cur_vao->vertex_attrib_offsets[attr_idx] >= cur_vao->vertex_attrib_offsets[p->attr_map[0]])) {
					is_packed[attr_idx] = GL_FALSE;
				}
#endif
			}
		}
#ifndef STRICT_DRAW_COMPLIANCE
		if (is_packed && (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[p->attr_map[1]] && cur_vao->vertex_attrib_offsets[p->attr_map[1]] > cur_vao->vertex_attrib_offsets[p->attr_map[0]])))
			is_packed = GL_FALSE;
	}
#endif
#ifdef STRICT_DRAW_COMPLIANCE
	// Gathering real attribute data pointers
	if (is_packed[0]) {
#ifdef SAFER_DRAW_SPEEDHACK
		if (count * streams[0].stride > SAFE_DRAW_SIZE_THRESHOLD) {
			ptrs[0] = (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + first * streams[0].stride;
		} else
#endif
		{
			ptrs[0] = gpu_alloc_mapped_temp(count * streams[0].stride);
			vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + first * streams[0].stride, count * streams[0].stride);
		}
	}
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		attributes[i].regIndex = p->attr[attr_idx].regIndex;
		if (is_packed[i]) {
			handle_packed_attrib();
		} else {
			handle_unpacked_attrib(first, count);
		}
	}
#else
	// Gathering real attribute data pointers
	if (is_packed) {
		if (target_vbo) {
			ptrs[0] = (void *)target_vbo->ptr + first * streams[0].stride;
			target_vbo->last_frame = vgl_framecount;
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_vbo_attrib();
			}
		} else {
#ifdef SAFER_DRAW_SPEEDHACK
			if (count * streams[0].stride > SAFE_DRAW_SIZE_THRESHOLD) {
				ptrs[0] = (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + first * streams[0].stride;
			} else
#endif
			{
				ptrs[0] = gpu_alloc_mapped_temp(count * streams[0].stride);
				vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]] + first * streams[0].stride, count * streams[0].stride);
			}
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_attrib();
			}
		}
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			attributes[i].regIndex = p->attr[attr_idx].regIndex;
			handle_unpacked_attrib(first, count);
		}
	}
#endif
#else // DRAW_SPEEDHACK
	handle_speedhack_attrib();
#endif

#ifndef INDICES_SPEEDHACK
	if (instanced) { // Instanced draw
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			streams[i].indexSource = (cur_vao->vertex_attrib_divisor & (1 << attr_idx)) ? SCE_GXM_INDEX_SOURCE_INSTANCE_16BIT : SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
		}		
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			streams[i].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
		}
	}
#endif

	// Uploading new vertex program
#ifdef HAVE_PROGRAM_VARIANT_CACHE
	get_vertex_program_variant(p, attributes, p->attr_num, streams, p->attr_num, &p->vprog);
#else
	patch_vertex_program(gxm_shader_patcher, p->vshader->id, attributes, p->attr_num, streams, p->attr_num, &p->vprog);
#endif
#ifdef HAVE_PREPARED_PACKED_VERTEX_CACHE
	cache_prepared_vertex(p, target_vbo, is_packed, instanced, p->vprog, streams);
#endif
	sceGxmSetVertexProgram(gxm_context, p->vprog);

	// Uploading both fragment and vertex uniforms data
	upload_uniforms();

	// Uploading vertex streams
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		GLboolean is_active = (cur_vao->vertex_attrib_state & (1 << attr_idx)) ? GL_TRUE : GL_FALSE;
		if (is_active) {
#ifdef DRAW_SPEEDHACK
			sceGxmSetVertexStream(gxm_context, i, ptrs[i]);
#else
#ifdef STRICT_DRAW_COMPLIANCE
			sceGxmSetVertexStream(gxm_context, i, is_packed[i] ? ptrs[0] : ptrs[i]);
#else
			sceGxmSetVertexStream(gxm_context, i, is_packed ? ptrs[0] : ptrs[i]);
#endif
#endif
		} else {
			sceGxmSetVertexStream(gxm_context, i, cur_vao->vertex_attrib_value[attr_idx]);
		}
		if (!p->has_unaligned_attrs) {
			attributes[i].regIndex = i;
			if (!is_active) {
				streams[i].stride = orig_stride[i];
				attributes[i].componentCount = orig_size[i];
				attributes[i].format = orig_fmt[i];
			}
		}
	}
#ifdef HAVE_PROFILING
	shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
	shaders_draw_cnt++;
#endif
	return GL_TRUE;
}

GLboolean _glDrawElements_CustomShadersIMPL(uint16_t *idx_buf, GLsizei count, uint32_t top_idx, uint32_t base_idx, SceGxmIndexSource index_type) {
#ifdef HAVE_PROFILING
	uint32_t draw_start = sceKernelGetProcessTimeLow();
#endif
	program *p = &progs[cur_program - 1];

	// Check if a blend info rebuild is required and upload fragment program
	setup_frag_program();

	// Uploading fragment textures on relative texture units
	for (int i = 0; i < p->max_frag_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->frag_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->frag_texunits[i]->sampler_index];
			uint8_t tex_type = p->frag_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifdef HAVE_TEX_CACHE
			restore_tex_cache(tex);
#endif
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->frag_texunits[i]->sampler_index];
			apply_sampler_state(tex, smp, GL_TRUE);
			sceGxmSetFragmentTexture(gxm_context, i, &tex->gxm_tex);
#ifdef HAVE_GLSL_TEXTURE_SIZE
			glsl_samplers_info *info = p->frag_texunits[i]->sampler;
			if (info) {
				uint32_t sizes[2];
				vglGetTexSizes(&tex->gxm_tex, &sizes[0], &sizes[1]);
				float fsizes[2] = {sizes[0], sizes[1]};
				vglSetUniformData(p->frag_texunits[i]->fptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 2, fsizes, SCE_GXM_PARAMETER_TYPE_F32);
				dirty_shader_frag_unifs = GL_TRUE;
			}
#endif
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// Uploading vertex textures on relative texture units
	for (int i = 0; i < p->max_vert_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->vert_texunits[i]) {
#endif
			texture_unit *tex_unit = &texture_units[(int)p->vert_texunits[i]->sampler_index];
			uint8_t tex_type = p->vert_texunits[i]->type == UNIFORM_CUBE_SAMPLER ? 2 : tex2d_override;
			texture *tex = &texture_slots[tex_unit->tex_id[tex_type]];
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sampler *smp = samplers[(int)p->vert_texunits[i]->sampler_index];
			apply_sampler_state(tex, smp, GL_FALSE);
			sceGxmSetVertexTexture(gxm_context, i, &tex->gxm_tex);
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// Aligning attributes
	SceGxmVertexAttribute *attributes;
	SceGxmVertexStream *streams;
	align_attributes(attributes, streams);

	void *ptrs[VERTEX_ATTRIBS_NUM];
#ifndef DRAW_SPEEDHACK
	vbo *target_vbo = (vbo *)cur_vao->vertex_attrib_vbo[p->attr_map[0]];
	GLboolean is_full_vbo = GL_TRUE;
#ifdef STRICT_DRAW_COMPLIANCE
	GLboolean is_packed[VERTEX_ATTRIBS_NUM];
	vgl_memset(is_packed, GL_TRUE, p->attr_num * sizeof(GLboolean));
#else
	GLboolean is_packed = p->attr_num > 1;
	if (is_packed) {
#endif
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			vbo *attr_vbo = (vbo *)cur_vao->vertex_attrib_vbo[attr_idx];
			if (attr_vbo) {
#ifdef STRICT_DRAW_COMPLIANCE
					vgl_memset(is_packed, 0, p->attr_num * sizeof(GLboolean));
					break;
#else
				if (attr_vbo != target_vbo) {
					is_packed = GL_FALSE;
					break;
				}
#endif
			} else {
#ifdef STRICT_DRAW_COMPLIANCE
				if (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[attr_idx] && cur_vao->vertex_attrib_offsets[attr_idx] >= cur_vao->vertex_attrib_offsets[p->attr_map[0]])) {
					is_packed[attr_idx] = GL_FALSE;
				}
#endif
				is_full_vbo = GL_FALSE;
			}
		}
#ifndef STRICT_DRAW_COMPLIANCE
		if (is_packed && (!(cur_vao->vertex_attrib_offsets[p->attr_map[0]] + streams[0].stride > cur_vao->vertex_attrib_offsets[p->attr_map[1]] && cur_vao->vertex_attrib_offsets[p->attr_map[1]] > cur_vao->vertex_attrib_offsets[p->attr_map[0]])))
			is_packed = GL_FALSE;
	} else if (!target_vbo)
		is_full_vbo = GL_FALSE;
#endif

	// Detecting highest index value
	if (!is_full_vbo && !top_idx) {
		uint32_t raw_top_idx = 0;
		uint32_t scan_start = 0;
#ifdef HAVE_RUNTIME_OPTIMIZER
		vbo *idx_vbo = (vbo *)cur_vao->index_array_unit;
		uintptr_t idx_addr = (uintptr_t)idx_buf;
		uintptr_t vbo_addr = idx_vbo ? (uintptr_t)idx_vbo->ptr : 0;
		uint32_t idx_offset = idx_addr >= vbo_addr ? (uint32_t)(idx_addr - vbo_addr) : 0;
		uint32_t idx_size = (index_type & 1) ? sizeof(uint32_t) : sizeof(uint16_t);
		uint64_t idx_bytes = (uint64_t)count * idx_size;
		GLboolean cacheable = idx_vbo && !idx_vbo->optimizer_external && idx_addr >= vbo_addr && idx_offset <= idx_vbo->size && idx_bytes <= idx_vbo->size - idx_offset;

		if (cacheable && idx_vbo->index_cache_generation == idx_vbo->generation && idx_vbo->index_cache_offset == idx_offset && idx_vbo->index_cache_count == count && idx_vbo->index_cache_type == (index_type & 1)) {
			raw_top_idx = idx_vbo->index_cache_top;
			vgl_optimizer_index_range(GL_TRUE);
		} else {
			GLboolean partial_scan = cacheable && idx_vbo->index_cache_generation == idx_vbo->generation && idx_vbo->index_cache_offset == idx_offset && idx_vbo->index_cache_type == (index_type & 1) && idx_vbo->index_cache_count < count;
			if (partial_scan) {
				scan_start = idx_vbo->index_cache_count;
				raw_top_idx = idx_vbo->index_cache_top ? idx_vbo->index_cache_top - 1 : 0;
			}
#endif
			if ((index_type & 1) == 0) {
				for (int i = scan_start; i < count; i++) {
					if (idx_buf[i] > raw_top_idx)
						raw_top_idx = idx_buf[i];
				}
			} else {
				uint32_t *_idx_buf = (uint32_t *)idx_buf;
				for (int i = scan_start; i < count; i++) {
					if (_idx_buf[i] > raw_top_idx)
						raw_top_idx = _idx_buf[i];
				}
			}
			raw_top_idx++;
#ifdef HAVE_RUNTIME_OPTIMIZER
			if (partial_scan)
				vgl_optimizer_index_range_partial();
			else
				vgl_optimizer_index_range(GL_FALSE);
			if (cacheable) {
				idx_vbo->index_cache_generation = idx_vbo->generation;
				idx_vbo->index_cache_offset = idx_offset;
				idx_vbo->index_cache_count = count;
				idx_vbo->index_cache_type = index_type & 1;
				idx_vbo->index_cache_top = raw_top_idx;
			}
		}
#endif
		top_idx = raw_top_idx + base_idx;
	}

#ifdef STRICT_DRAW_COMPLIANCE
	// Gathering real attribute data pointers
	if (is_packed[0]) {
#ifdef SAFER_DRAW_SPEEDHACK
		if (top_idx * streams[0].stride > SAFE_DRAW_SIZE_THRESHOLD) {
			ptrs[0] = (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]];
		} else
#endif
		{
			ptrs[0] = gpu_alloc_mapped_temp(top_idx * streams[0].stride);
			vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]], top_idx * streams[0].stride);
		}
	}
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		attributes[i].regIndex = p->attr[attr_idx].regIndex;
		if (is_packed[i]) {
			handle_packed_attrib();
		} else {
			handle_unpacked_attrib(0, top_idx);
		}
	}
#else
	// Gathering real attribute data pointers
	if (is_packed) {
		if (target_vbo) {
			ptrs[0] = (void *)target_vbo->ptr;
			target_vbo->last_frame = vgl_framecount;
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_vbo_attrib();
			}
		} else {
#ifdef SAFER_DRAW_SPEEDHACK
			if (top_idx * streams[0].stride > SAFE_DRAW_SIZE_THRESHOLD) {
				ptrs[0] = (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]];
			} else
#endif
			{
				ptrs[0] = gpu_alloc_mapped_temp(top_idx * streams[0].stride);
				vgl_fast_memcpy(ptrs[0], (void *)cur_vao->vertex_attrib_offsets[p->attr_map[0]], top_idx * streams[0].stride);
			}
			for (int i = 0; i < p->attr_num; i++) {
				uint8_t attr_idx = p->attr_map[i];
				attributes[i].regIndex = p->attr[attr_idx].regIndex;
				handle_packed_attrib();
			}
		}	
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			attributes[i].regIndex = p->attr[attr_idx].regIndex;
			handle_unpacked_attrib(0, top_idx);
		}
	}
#endif
#else // DRAW_SPEEDHACK
	handle_speedhack_attrib();
#endif

#ifndef INDICES_SPEEDHACK
	// Check if highest index is small enough for 16 bit usage and if so, downgrade to 16 bit vertex sources for faster emitted code
	if (top_idx && top_idx < 0xFFFF) {
		index_type &= ~1;
	}

	if (index_type & 2) { // Instanced draw
		for (int i = 0; i < p->attr_num; i++) {
			uint8_t attr_idx = p->attr_map[i];
			streams[i].indexSource = (cur_vao->vertex_attrib_divisor & (1 << attr_idx)) ? index_type : (index_type & 1);
		}		
	} else {
		for (int i = 0; i < p->attr_num; i++) {
			streams[i].indexSource = index_type;
		}
	}
#endif

	// Uploading new vertex program
#ifdef HAVE_PROGRAM_VARIANT_CACHE
	get_vertex_program_variant(p, attributes, p->attr_num, streams, p->attr_num, &p->vprog);
#else
	patch_vertex_program(gxm_shader_patcher, p->vshader->id, attributes, p->attr_num, streams, p->attr_num, &p->vprog);
#endif
	sceGxmSetVertexProgram(gxm_context, p->vprog);

	// Uploading both fragment and vertex uniforms data
	upload_uniforms();

	// Uploading vertex streams
	for (int i = 0; i < p->attr_num; i++) {
		uint8_t attr_idx = p->attr_map[i];
		GLboolean is_active = (cur_vao->vertex_attrib_state & (1 << attr_idx)) ? GL_TRUE : GL_FALSE;
		if (is_active) {
#ifdef DRAW_SPEEDHACK
			sceGxmSetVertexStream(gxm_context, i, ptrs[i]);
#else
#ifdef STRICT_DRAW_COMPLIANCE
			sceGxmSetVertexStream(gxm_context, i, is_packed[i] ? ptrs[0] : ptrs[i]);
#else
			sceGxmSetVertexStream(gxm_context, i, is_packed ? ptrs[0] : ptrs[i]);
#endif
#endif
		} else {
			sceGxmSetVertexStream(gxm_context, i, cur_vao->vertex_attrib_value[attr_idx]);
		}
		if (!p->has_unaligned_attrs) {
			attributes[i].regIndex = i;
			if (!is_active) {
				streams[i].stride = orig_stride[i];
				attributes[i].componentCount = orig_size[i];
				attributes[i].format = orig_fmt[i];
			}
		}
	}
#ifdef HAVE_PROFILING
	shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
	shaders_draw_cnt++;
#endif
	return GL_TRUE;
}

#ifdef ENABLE_LEGACY_PIPELINE
void _vglDrawObjects_CustomShadersIMPL() {
#ifdef HAVE_PROFILING
	uint32_t draw_start = sceKernelGetProcessTimeLow();
#endif
	program *p = &progs[cur_program - 1];

	// Check if a blend info rebuild is required
	setup_frag_program();

	// Setting up required vertex shader
	sceGxmSetVertexProgram(gxm_context, p->vprog);

	// Uploading textures on relative texture units
	for (int i = 0; i < p->max_frag_texunit_idx; i++) {
#ifndef SAMPLERS_SPEEDHACK
		if (p->frag_texunits[i]) {
#endif
			texture *tex = &texture_slots[texture_units[i].tex_id[0]];
#ifndef TEXTURES_SPEEDHACK
			tex->last_frame = vgl_framecount;
#endif
			sceGxmSetFragmentTexture(gxm_context, i, &tex->gxm_tex);
#ifdef HAVE_GLSL_TEXTURE_SIZE
			glsl_samplers_info *info = p->frag_texunits[i]->sampler;
			if (info) {
				uint32_t sizes[2];
				vglGetTexSizes(&tex->gxm_tex, &sizes[0], &sizes[1]);
				float fsizes[2] = {sizes[0], sizes[1]};
				vglSetUniformData(p->frag_texunits[i]->fptr, SCE_GXM_PARAMETER_TYPE_F32, 0, 1, 2, fsizes, SCE_GXM_PARAMETER_TYPE_F32);
				dirty_shader_frag_unifs = GL_TRUE;
			}
#endif
#ifndef SAMPLERS_SPEEDHACK
		}
#endif
	}

	// Uploading both fragment and vertex uniforms data
	upload_uniforms();

#ifdef HAVE_PROFILING
	shaders_draw_profiler_cnt += sceKernelGetProcessTimeLow() - draw_start;
	shaders_draw_cnt++;
#endif
}
#endif

#ifdef HAVE_SHARK_LOG
void shark_log_cb(const char *msg, shark_log_level msg_level, int line) {
	char newline[1024];
	GLboolean is_extra_line = shark_log ? GL_TRUE : GL_FALSE;
	switch (msg_level) {
	case SHARK_LOG_INFO:
		sprintf(newline, "%sI] %s on line %d.", is_extra_line ? "\n" : "", msg, line);
#ifdef LOG_ERRORS
		vgl_log("Shader Compiler: I] %s on line %d.\n", msg, line);
#endif
		break;
	case SHARK_LOG_WARNING:
		sprintf(newline, "%sW] %s on line %d.", is_extra_line ? "\n" : "", msg, line);
#ifdef LOG_ERRORS
		vgl_log("Shader Compiler: W] %s on line %d.\n", msg, line);
#endif
		break;
	case SHARK_LOG_ERROR:
		sprintf(newline, "%sE] %s on line %d.", is_extra_line ? "\n" : "", msg, line);
#ifdef LOG_ERRORS
		vgl_log("Shader Compiler: E] %s on line %d.\n", msg, line);
#endif
		break;
	}
	uint32_t size = (is_extra_line ? strlen(shark_log) : 0) + strlen(newline);
	shark_log = shark_log ? vglRealloc(shark_log, size + 1) : vglMalloc(size + 1);
	if (is_extra_line)
		strcat(shark_log, newline);
	else
		strcpy(shark_log, newline);
}
#elif defined(LOG_ERRORS)
void shark_log_cb(const char *msg, shark_log_level msg_level, int line) {
	switch (msg_level) {
	case SHARK_LOG_INFO:
		vgl_log("Shader Compiler: I] %s on line %d.\n", msg, line);
		break;
	case SHARK_LOG_WARNING:
		vgl_log("Shader Compiler: W] %s on line %d.\n", msg, line);
		break;
	case SHARK_LOG_ERROR:
		vgl_log("Shader Compiler: E] %s on line %d.\n", msg, line);
		break;
	}
}
#endif

static inline __attribute__((always_inline)) float *get_uniform_alias_data_ptr(uniform *src, uint32_t cnt, const char *name) {
	for (int i = 0; i < cnt; i++) {
		uniform *u = &src[i];
		if (!strcmp(name, sceGxmProgramParameterGetName(u->ptr))) {
			return u->fptr;
		}
	}
	return NULL;
}

static inline __attribute__((always_inline)) ubo *has_block_alias(ubo *u, const char *name) {
	while (u) {
		if (!strcmp(name, ((block_uniform*)u->ptr)->name)) {
			return u;
		}
		u = u->chain;
	}
	return NULL;
}

static inline __attribute__((always_inline)) block_uniform *get_block_details(block_uniform *b, uint8_t idx) {
	while (b) {
		if (b->idx == idx)
			return b;
		b = (block_uniform *)b->chain;
	}
	return NULL;
}

/*
 * ------------------------------
 * - IMPLEMENTATION STARTS HERE -
 * ------------------------------
 */
void vglSetupRuntimeShaderCompiler(shark_opt opt_level, int32_t use_fastmath, int32_t use_fastprecision, int32_t use_fastint) {
	compiler_opts = opt_level;
	compiler_fastmath = use_fastmath;
	compiler_fastprecision = use_fastprecision;
	compiler_fastint = use_fastint;
}

GLuint glCreateShader(GLenum shaderType) {
#ifndef SKIP_ERROR_HANDLING
	if (shaderType != GL_FRAGMENT_SHADER && shaderType != GL_VERTEX_SHADER && shaderType != GL_CG_FRAGMENT_SHADER_EXT && shaderType != GL_CG_VERTEX_SHADER_EXT) {
		SET_GL_ERROR_WITH_RET_AND_VALUE(GL_INVALID_ENUM, 0, shaderType)
	}
#endif

	// Looking for a free shader slot
	GLuint i, res = 0;
	for (i = 1; i <= MAX_CUSTOM_SHADERS; i++) {
		if (!(shaders[i - 1].valid)) {
			res = i;
			break;
		}
	}

#ifndef SKIP_ERROR_HANDLING
	// All shader slots are busy, exiting call
	if (res == 0) {
		vgl_log("%s:%d %s: Out of shaders handles. Consider increasing MAX_CUSTOM_SHADERS...\n", __FILE__, __LINE__, __func__);
		return res;
	}
#endif

	// Reserving and initializing shader slot
	switch (shaderType) {
	case GL_CG_VERTEX_SHADER_EXT:
		shaders[res - 1].type = GL_VERTEX_SHADER;
		shaders[res - 1].is_glsl = GL_FALSE;
		break;
	case GL_CG_FRAGMENT_SHADER_EXT:
		shaders[res - 1].type = GL_FRAGMENT_SHADER;
		shaders[res - 1].is_glsl = GL_FALSE;
		break;
	default:
		shaders[res - 1].type = shaderType;
		shaders[res - 1].is_glsl = GL_TRUE;
		break;
	}
	shaders[res - 1].mat = NULL;
	shaders[res - 1].unif_blk = NULL;
	shaders[res - 1].prog = NULL;
	shaders[res - 1].valid = GL_TRUE;
	shaders[res - 1].source = NULL;

	return res;
}

void glGetShaderiv(GLuint handle, GLenum pname, GLint *params) {
	THREAD_SAFE()

	// Grabbing passed shader
	shader *s = &shaders[handle - 1];
	switch (pname) {
	case GL_SHADER_TYPE:
		*params = s->type;
		break;
	case GL_COMPILE_STATUS:
		if (glsl_sema_mode == VGL_MODE_POSTPONED && s->is_glsl) {
			*params = GL_TRUE;
			break;
		}
		*params = s->prog ? GL_TRUE : GL_FALSE;
		break;
	case GL_DELETE_STATUS:
		*params = s->dirty ? GL_TRUE : GL_FALSE;
		break;
	case GL_INFO_LOG_LENGTH:
#ifdef HAVE_SHARK_LOG
		*params = s->log ? (strlen(s->log) + 1) : 0;
#else
		*params = 0;
#endif
		break;
	case GL_SHADER_SOURCE_LENGTH:
		*params = s->source ? (strlen(s->source) + 1) : 0;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

void glGetShaderInfoLog(GLuint handle, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (maxLength < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	GLsizei len = 0;
#ifdef HAVE_SHARK_LOG
	shader *s = &shaders[handle - 1];
	if (s->log) {
		len = min(strlen(s->log), maxLength - 1);
		vgl_fast_memcpy(infoLog, s->log, len);
		infoLog[len] = 0;
	}
#endif
	if (length)
		*length = len;
}

void glGetShaderSource(GLuint handle, GLsizei bufSize, GLsizei *length, GLchar *source) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (bufSize < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Grabbing passed shader
	shader *s = &shaders[handle - 1];

	GLsizei size = 0;
	if (s->source) {
		GLsizei src_len = strlen(s->source);
		if (bufSize <= src_len)
			src_len = bufSize - 1;
		strncpy(source, s->source, src_len);
		size = src_len;
	}
	if (length)
		*length = size;
}

void glShaderSource(GLuint handle, GLsizei count, const GLchar *const *string, const GLint *length) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (count < 0 || count > 32) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	// Grabbing passed shader
	shader *s = &shaders[handle - 1];
	
	uint32_t size = 1;
	size_t lengths[32];

	for (int i = 0; i < count; i++) {
		if (length && length[i] >= 0) {
			lengths[i] = length[i];
		} else {
			lengths[i] = strlen(string[i]);
			
		}
		size += lengths[i];
	}

	s->source = (char *)vglMalloc(size);
	s->source[0] = 0;

	for (int i = 0; i < count; i++) {
		strncat(s->source, string[i], lengths[i]);
	}

	s->size = size - 1;
}

void glShaderBinary(GLsizei count, const GLuint *handles, GLenum binaryFormat, const void *binary, GLsizei length) {
	THREAD_SAFE()

	// Grabbing passed shader
	shader *s = &shaders[handles[0] - 1];

	unserialize_shader((void *)binary, length, s, GL_FALSE);
}

void glCompileShader(GLuint handle) {
	THREAD_SAFE()

	// If vitaShaRK is not enabled, we try to initialize it
	if (!is_shark_online && !start_shader_compiler()) {
		SET_GL_ERROR(GL_INVALID_OPERATION)
	}
	
	// Grabbing passed shader
	shader *s = &shaders[handle - 1];
	
	// If we use VGL_MODE_POSTPONED, we compile shaders in glLinkProgram
	if (s->is_glsl && glsl_sema_mode == VGL_MODE_POSTPONED)
		return;
	
#ifdef HAVE_SHADER_CACHE
	char fname[256];
	sprintf(fname, "%s/%s/%llX.gxp", vgl_shader_cache_path, s->type == GL_VERTEX_SHADER ? "v" : "f", get_shader_cache_hash(s, NULL));
	SceUID f = sceIoOpen(fname, SCE_O_RDONLY, 0777);
	if (f >= 0) {
		vgl_free(s->source);
		s->source = NULL;
		size_t sz = sceIoLseek(f, 0, SCE_SEEK_END);
		sceIoLseek(f, 0, SCE_SEEK_SET);
		void *buf = vglMalloc(sz);
		sceIoRead(f, buf, sz);
		sceIoClose(f);
		unserialize_shader(buf, sz, s, GL_FALSE);
		vgl_free(buf);
		return;
	}
#endif

	if (s->is_glsl) {
		glsl_translator_process(s);
	}
	vgl_compile_shader(s, GL_FALSE);
}

void glDeleteShader(GLuint shad) {
	THREAD_SAFE()

	// Grabbing passed shader
	shader *s = &shaders[shad - 1];

	// If the shader is attached to any program, we only mark it for deletion
	if (s->ref_counter > 0)
		s->dirty = GL_TRUE;
	else
		release_shader(s);
}

void glAttachShader(GLuint prog, GLuint shad) {
	THREAD_SAFE()

	// Grabbing passed shader and program
	shader *s = &shaders[shad - 1];
	program *p = &progs[prog - 1];
	
	// Attaching shader to desired program
	if (p->status == PROG_UNLINKED && s->valid) {
		switch (s->type) {
		case GL_VERTEX_SHADER:
			s->ref_counter++;
			if (p->vshader) {
				p->vshader->ref_counter--;
				if (p->vshader->dirty && p->vshader->ref_counter == 0)
					release_shader(p->vshader);
			}
			p->vshader = s;
			// If we use VGL_MODE_POSTPONED, we perform attributes binding in glLinkProgram
			if (glsl_sema_mode != VGL_MODE_POSTPONED || !s->is_glsl) {
				// Setting progressive default attribute bindings
				set_default_attrib_binding();	
			}
			break;
		case GL_FRAGMENT_SHADER:
			s->ref_counter++;
			if (p->fshader) {
				p->fshader->ref_counter--;
				if (p->fshader->dirty && p->fshader->ref_counter == 0)
					release_shader(p->fshader);
			}
			p->fshader = s;
			break;
		default:
			break;
		}
	} else {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
}

void glGetAttachedShaders(GLuint prog, GLsizei maxCount, GLsizei *count, GLuint *shads) {
	THREAD_SAFE()

	// Grabbing passed program
	program *p = &progs[prog - 1];

#ifndef SKIP_ERROR_HANDLING
	if (maxCount < 0) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, maxCount)
	}
#endif

	// Returning attached shaders
	GLuint shad = 1;
	*count = 0;
	if (p->vshader) {
		for (int i = 1; i <= MAX_CUSTOM_SHADERS; i++) {
			if (p->vshader == &shaders[i - 1]) {
				shad = i;
				break;
			}
		}
		shads[0] = shad;
		*count = 1;
	}
	if (p->fshader) {
		for (int i = 1; i <= MAX_CUSTOM_SHADERS; i++) {
			if (p->fshader == &shaders[i - 1]) {
				shad = i;
				break;
			}
		}
		shads[*count] = shad;
		*count = *count + 1;
	}
}

GLuint glCreateProgram(void) {
	// Looking for a free program slot
	GLuint i, j, res = 0xFFFFFFFF;
	for (i = 1; i <= MAX_CUSTOM_PROGRAMS; i++) {
		// Program slot found, reserving and initializing it
		if (!(progs[i - 1].status)) {
			res = i--;
			progs[i].status = PROG_UNLINKED;
			progs[i].attr_num = 0;
#ifdef ENABLE_LEGACY_PIPELINE
			progs[i].attr_mode = VGL_ATTRIB_REGULAR;
#endif
			progs[i].attr_idx = 0;
			progs[i].max_frag_texunit_idx = 0;
			progs[i].max_vert_texunit_idx = 0;
#ifdef HAVE_FFP_SHADER_SUPPORT
			for (int j = 0; j < FFP_BINDS_NUM; j++) {
				progs[i].ffp_binds[j] = NULL;
			}
#endif
			progs[i].vshader = NULL;
			progs[i].fshader = NULL;
			progs[i].vprog = NULL;
			progs[i].fprog = NULL;
			progs[i].vert_uniforms = NULL;
			progs[i].frag_uniforms = NULL;
			progs[i].unif_vbuffer = NULL;
			progs[i].unif_fbuffer = NULL;
			progs[i].vert_uniforms_num = 0;
			progs[i].frag_uniforms_num = 0;
			progs[i].vert_ubos = NULL;
			progs[i].frag_ubos = NULL;
			progs[i].attr_highest_idx = 0;
			progs[i].num_glsl_attr = 0;
			progs[i].glsl_attr_map = NULL;
			progs[i].is_fbo_float = 0xFF;
#ifdef HAVE_PROGRAM_VARIANT_CACHE
			progs[i].vertex_variants = NULL;
			progs[i].fragment_variants = NULL;
#endif
			for (j = 0; j < VERTEX_ATTRIBS_NUM; j++) {
				progs[i].attr[j].regIndex = 0xDEAD;
			}
			break;
		}
	}
#ifndef SKIP_ERROR_HANDLING
	if (res == 0xFFFFFFFF) {
		vgl_log("%s:%d %s: Out of programs handles. Consider increasing MAX_CUSTOM_PROGRAMS...\n", __FILE__, __LINE__, __func__);
		return 0;
	}
#endif
	return res;
}

GLboolean glIsProgram(GLuint i) {
	if (progs[i - 1].status != PROG_INVALID)
		return GL_TRUE;
	return GL_FALSE;
}

void glGetProgramBinary(GLuint prog, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (bufSize < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Grabbing passed program
	program *p = &progs[prog - 1];

	// Saving info related to bound attributes locations
	GLuint *b = (GLuint *)binary;
	b[0] = p->attr_highest_idx;
	vgl_fast_memcpy(&b[1], p->attr, sizeof(SceGxmVertexAttribute) * VERTEX_ATTRIBS_NUM);
	GLsizei size = sizeof(GLuint) + sizeof(SceGxmVertexAttribute) * VERTEX_ATTRIBS_NUM;
	uint8_t *buf = (uint8_t *)binary + size;

	// Dumping vertex binary
	serialize_shader(&buf[sizeof(uint32_t)], (size_t *)buf, p->vshader, GL_FALSE);

	// Dumping fragment binary
	buf += sizeof(uint32_t) + *(uint32_t *)buf;
	serialize_shader(&buf[sizeof(uint32_t)], (size_t *)buf, p->fshader, GL_FALSE);

	if (length)
		*length = ((uintptr_t)buf - (uintptr_t)binary) + sizeof(uint32_t) + *(uint32_t *)buf;
}

void glProgramBinary(GLuint prog, GLenum binaryFormat, const void *binary, GLsizei length) {
	THREAD_SAFE()

	vgl_optimizer_program_storage_reset(prog);

	// Grabbing passed program
	program *p = &progs[prog - 1];
	
	// Restoring bound attributes info
	GLuint *b = (GLuint *)binary;
	p->attr_highest_idx = b[0];
	vgl_fast_memcpy(p->attr, &b[1], sizeof(SceGxmVertexAttribute) * VERTEX_ATTRIBS_NUM);
	GLsizei size = sizeof(GLuint) + sizeof(SceGxmVertexAttribute) * VERTEX_ATTRIBS_NUM;

	// Restoring shaders
	GLuint vs = glCreateShader(GL_CG_VERTEX_SHADER_EXT);
	GLuint fs = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT);
	uint32_t *sizeptr = (uint32_t *)((uint8_t *)binary + size);
	glShaderBinary(1, &vs, 0, &sizeptr[1], sizeptr[0]);
	sizeptr = (uint32_t *)((uint8_t *)binary + size + sizeptr[0] + sizeof(uint32_t));
	glShaderBinary(1, &fs, 0, &sizeptr[1], sizeptr[0]);
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);

	// Linking program and marking for deletion temporary shaders
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
}

void glDeleteProgram(GLuint prog) {
	THREAD_SAFE()

	vgl_optimizer_program_storage_reset(prog);

	// Grabbing passed program
	program *p = &progs[prog - 1];

	// Releasing both vertex and fragment programs from sceGxmShaderPatcher
	if (p->status) {
		sceGxmFinish(gxm_context);
#ifdef HAVE_PROGRAM_VARIANT_CACHE
		release_program_variants(p);
#endif
		if (p->vert_uniforms) {
			vgl_free(p->vert_uniforms);
			if (p->unif_vbuffer) {
				vgl_free(p->unif_vbuffer);
			}
		}
		if (p->frag_uniforms) {
			vgl_free(p->frag_uniforms);
			if (p->unif_fbuffer) {
				vgl_free(p->unif_fbuffer);
			}
		}
		while (p->vert_ubos) {
			ubo *old = p->vert_ubos;
			p->vert_ubos = (ubo *)p->vert_ubos->chain;
			vgl_free(old);
		}
		while (p->frag_ubos) {
			ubo *old = p->frag_ubos;
			p->frag_ubos = (ubo *)p->frag_ubos->chain;
			vgl_free(old);
		}
		if (p->glsl_attr_map) {
			vgl_free(p->glsl_attr_map);
		}

		// Checking if attached shaders are marked for deletion and should be deleted
		if (p->vshader) {
			p->vshader->ref_counter--;
			if (p->vshader->dirty && p->vshader->ref_counter == 0)
				release_shader(p->vshader);
		}
		if (p->fshader) {
			p->fshader->ref_counter--;
			if (p->fshader->dirty && p->fshader->ref_counter == 0)
				release_shader(p->fshader);
		}
	}
	p->status = PROG_INVALID;
}

void glGetProgramInfoLog(GLuint program, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
	THREAD_SAFE()

	if (length)
		*length = 0;
}

void glGetProgramiv(GLuint progr, GLenum pname, GLint *params) {
	THREAD_SAFE()

	// Grabbing passed program
	program *p = &progs[progr - 1];
	int i, cnt;
	matrix_uniform *m;
	uint32_t *ptr;
	uint32_t dummy;

	switch (pname) {
	case GL_LINK_STATUS:
	case GL_VALIDATE_STATUS:
		*params = p->status == PROG_LINKED;
		break;
	case GL_INFO_LOG_LENGTH:
		*params = 0;
		break;
	case GL_PROGRAM_BINARY_LENGTH:
		*params = serialized_shader_size(p->vshader, GL_FALSE, &dummy) + serialized_shader_size(p->fshader, GL_FALSE, &dummy) + sizeof(GLuint) * 3 + sizeof(SceGxmVertexAttribute) * VERTEX_ATTRIBS_NUM;
		break;
	case GL_ATTACHED_SHADERS:
		i = 0;
		if (p->fshader)
			i++;
		if (p->vshader)
			i++;
		*params = i;
		break;
	case GL_ACTIVE_ATTRIBUTES:
		*params = p->attr_num;
		break;
	case GL_ACTIVE_UNIFORM_MAX_LENGTH:
		i = 0;
		for (int j = 0; j < p->vert_uniforms_num; j++) {
			int len = strlen(sceGxmProgramParameterGetName(p->vert_uniforms[j].ptr)) + 1;
			if (len > i)
				i = len;
		}
		for (int j = 0; j < p->frag_uniforms_num; j++) {
			int len = strlen(sceGxmProgramParameterGetName(p->frag_uniforms[j].ptr)) + 1;
			if (len > i)
				i = len;
		}
		*params = i;
		break;
	case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH:
		i = 0;
		cnt = sceGxmProgramGetParameterCount(p->vshader->prog);
		uint32_t *ptr = vglProgramGetParameterBase(p->vshader->prog);
		while (cnt--) {
			SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr;
			if (sceGxmProgramParameterGetCategory(param) == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) {
				int len = strlen(sceGxmProgramParameterGetName(param)) + 1;
				if (len > i)
					i = len;
			}
			ptr += 4;
		}
		*params = i;
		break;
	case GL_ACTIVE_UNIFORMS:
		*params = p->vert_uniforms_num + p->frag_uniforms_num;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

void glLinkProgram(GLuint progr) {
	THREAD_SAFE()

	vgl_optimizer_program_storage_reset(progr);

	// Grabbing passed program
	program *p = &progs[progr - 1];
#ifndef SKIP_ERROR_HANDLING
	if (glsl_sema_mode == VGL_MODE_POSTPONED) {
		if (!(p->fshader->prog || (p->fshader->is_glsl && p->fshader->source)) || !(p->vshader->prog || (p->vshader->is_glsl && p->vshader->source))) {
			vgl_log("%s:%d: %s: %s shader is missing.\n", __FILE__, __LINE__, __func__, (p->fshader->prog || (p->fshader->is_glsl && p->fshader->source)) ? "vertex" : "fragment");
			return;
		}
	} else {
		if (!p->fshader->prog || !p->vshader->prog) {
			vgl_log("%s:%d: %s: %s shader is missing.\n", __FILE__, __LINE__, __func__, p->fshader->prog ? "vertex" : "fragment");
			return;
		}
	}
#endif

	// With VGL_MODE_POSTPONED we perform shaders translation+compilation and attributes binding prior actual program linking
	if (glsl_sema_mode == VGL_MODE_POSTPONED && (p->vshader->is_glsl || p->fshader->is_glsl)) {
		glsl_sema_mode = VGL_MODE_SHADER_PAIR;
#ifdef HAVE_SHADER_CACHE
		char frag_fname[256], vert_fname[256];
		char *fname;
		if (!p->vshader->prog) {
			sprintf(vert_fname, "%s/v/%llX.gxp", vgl_shader_cache_path, get_shader_cache_hash(p->vshader, p->fshader));
			SceUID f = sceIoOpen(vert_fname, SCE_O_RDONLY, 0777);
			if (f >= 0) {
				size_t sz = sceIoLseek(f, 0, SCE_SEEK_END);
				sceIoLseek(f, 0, SCE_SEEK_SET);
				void *buf = vglMalloc(sz);
				sceIoRead(f, buf, sz);
				sceIoClose(f);
				unserialize_shader(buf, sz, p->vshader, GL_TRUE);
				vgl_free(buf);
			}
		}
		if (!p->fshader->prog) {
			sprintf(frag_fname, "%s/f/%llX.gxp", vgl_shader_cache_path, get_shader_cache_hash(p->fshader, p->vshader));
			SceUID f = sceIoOpen(frag_fname, SCE_O_RDONLY, 0777);
			if (f >= 0) {
				size_t sz = sceIoLseek(f, 0, SCE_SEEK_END);
				sceIoLseek(f, 0, SCE_SEEK_SET);
				void *buf = vglMalloc(sz);
				sceIoRead(f, buf, sz);
				sceIoClose(f);
				unserialize_shader(buf, sz, p->fshader, GL_TRUE);
				vgl_free(buf);
			}
		}
#endif
		if (!p->vshader->prog || !p->fshader->prog) {
			if (p->vshader->is_glsl || p->fshader->is_glsl) {
				glsl_translator_set_process(p->vshader, p->fshader);
			}
			if (!p->vshader->prog) {
#ifdef HAVE_SHADER_CACHE
				fname = vert_fname;
#endif
				vgl_compile_shader(p->vshader, GL_TRUE);
			}
			if (!p->fshader->prog) {
#ifdef HAVE_SHADER_CACHE
				fname = frag_fname;
#endif
				vgl_compile_shader(p->fshader, GL_TRUE);
			}
		}

		// Setting progressive default attribute bindings
		set_default_attrib_binding();

		if (p->glsl_attr_map) {
			for (int i = 0; i < p->num_glsl_attr; i++) {
				glBindAttribLocation(progr, p->glsl_attr_map[i].idx, p->glsl_attr_map[i].name);
			}
			vgl_free(p->glsl_attr_map);
			p->glsl_attr_map = NULL;
		}
		glsl_sema_mode = VGL_MODE_POSTPONED;
	}

	if (p->status == PROG_LINKED) {
		vgl_log("%s:%d: %s: A program has been re-linked. vitaGL doesn't support re-linking, glitches may happen.\n", __FILE__, __LINE__, __func__);
		return;
	}
	p->status = PROG_LINKED;
	
	// Set up uniform buffers
	if (p->vshader->unif_buf_size) {
		p->unif_vbuffer = vglCalloc(1, p->vshader->unif_buf_size);
	}
	if (p->fshader->unif_buf_size) {
		p->unif_fbuffer = vglCalloc(1, p->fshader->unif_buf_size);
	}

	// Analyzing fragment shader
	uint32_t i, cnt, j;
	for (i = 0; i < TEXTURE_IMAGE_UNITS_NUM; i++) {
		p->frag_texunits[i] = GL_FALSE;
		p->vert_texunits[i] = GL_FALSE;
	}
	cnt = sceGxmProgramGetParameterCount(p->fshader->prog);
	uint32_t *_ptr = vglProgramGetParameterBase(p->fshader->prog);
	uint32_t *ptr = _ptr;
	for (i = 0; i < cnt; i++) {
		SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr;
		SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(param);
		if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER || (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM && sceGxmProgramParameterGetContainerIndex(param) == UBOS_NUM)) {
			p->frag_uniforms_num++;
		} else if (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER) {
			ubo *u = (ubo *)vglMalloc(sizeof(ubo));
			u->chain = p->frag_ubos;
			u->idx = sceGxmProgramParameterGetResourceIndex(param);
			u->ptr = (const SceGxmProgramParameter *)get_block_details(p->fshader->unif_blk, u->idx);
			u->bind = 0;
			u->alias = NULL;
			p->frag_ubos = u;
		}
		ptr += 4;
	}
	if (p->frag_uniforms_num) {
		p->frag_uniforms = (uniform *)vglMalloc(sizeof(uniform) * p->frag_uniforms_num);
		ptr = _ptr;
		j = 0;
		for (i = 0; i < cnt; i++) {
			SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr;
			SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(param);
			if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
				uint8_t texunit_idx = sceGxmProgramParameterGetResourceIndex(param) + 1;
				if (p->max_frag_texunit_idx < texunit_idx)
					p->max_frag_texunit_idx = texunit_idx;
				uniform *u = &p->frag_uniforms[j++];
				u->ptr = param;
				u->type = sceGxmProgramParameterIsSamplerCube(param) ? UNIFORM_CUBE_SAMPLER : UNIFORM_SAMPLER;
				u->sampler_index = 0;
				p->frag_texunits[texunit_idx - 1] = u;
#ifdef HAVE_GLSL_TEXTURE_SIZE
				u->sampler = NULL;
				const char *pname = sceGxmProgramParameterGetName(param);
				for (uint8_t i = 0; i < p->fshader->sized_samplers_num; i++) {
					if (!strcmp(pname, p->fshader->sized_samplers[i].name)) {
						char smp[16];
						sprintf(smp, "vgl_smp%u", i);
						// If the linked uniform is missing, textureSize usage has been stripped out
						if (sceGxmProgramFindParameterByName(p->fshader->prog, smp))
							u->sampler = &p->fshader->sized_samplers[i];
						break;
					}
				}
#endif
			} else if (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM && sceGxmProgramParameterGetContainerIndex(param) == UBOS_NUM) {
				uniform *u = &p->frag_uniforms[j++];
				u->ptr = param;
				u->vptr = NULL;
				u->fptr = (uint8_t *)p->unif_fbuffer + sceGxmProgramParameterGetResourceIndex(param) * 4;
				u->type = UNIFORM_DATA;
#ifdef HAVE_GLSL_TEXTURE_SIZE
				u->sampler = NULL;
				const char *pname = sceGxmProgramParameterGetName(param);
				for (uint8_t i = 0; i < p->fshader->sized_samplers_num; i++) {
					char smp[16];
					sprintf(smp, "vgl_smp%u", i);
					if (!strcmp(pname, smp)) {
						u->sampler = &p->fshader->sized_samplers[i];
						break;
					}
				}
#endif
			}
			ptr += 4;
		}
#ifdef HAVE_GLSL_TEXTURE_SIZE
		// Assign to every sampler uniform the uniform buffer ptr of the equivalent data uniform
		for (int i = 0; i < p->frag_uniforms_num; i++) {
			uniform *u1 = &p->frag_uniforms[i];
			if (u1->sampler && u1->type != UNIFORM_DATA) {
				for (int j = 0; j < p->frag_uniforms_num; j++) {
					uniform *u2 = &p->frag_uniforms[j];
					if (u2->sampler == u1->sampler && u2->type == UNIFORM_DATA) {
						u1->fptr = u2->fptr;
						break;
					}
				}
			}
		}
#endif
#ifdef HAVE_FFP_SHADER_SUPPORT
		for (int i = 0; i < FFP_BINDS_NUM; i++) {
			SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->fshader->prog, ffp_bind_names[i]);
			if (param) {
				for (int j = 0; j < p->frag_uniforms_num; j++) {
					if (p->frag_uniforms[j].ptr == param) {
						p->ffp_binds[i] = &p->frag_uniforms[j];
						break;
					}
				}
			}
		}
#endif
	}

	// Analyzing vertex shader
	cnt = sceGxmProgramGetParameterCount(p->vshader->prog);
	_ptr = vglProgramGetParameterBase(p->vshader->prog);
	ptr = _ptr;
	for (i = 0; i < cnt; i++) {
		SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr;
		SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(param);
		if (cat == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE) {
			p->attr_num++;
		} else if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER || (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM && sceGxmProgramParameterGetContainerIndex(param) == UBOS_NUM)) {
			p->vert_uniforms_num++;
		} else if (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM_BUFFER) {
			ubo *u = (ubo *)vglMalloc(sizeof(ubo));
			u->chain = p->vert_ubos;
			u->idx = sceGxmProgramParameterGetResourceIndex(param);
			u->ptr = (const SceGxmProgramParameter *)get_block_details(p->vshader->unif_blk, u->idx);
			u->bind = 0;
			u->alias = has_block_alias(p->frag_ubos, ((block_uniform *)u->ptr)->name);
			p->vert_ubos = u;
		}
		ptr += 4;
	}
	if (p->vert_uniforms_num) {
		p->vert_uniforms = (uniform *)vglMalloc(sizeof(uniform) * p->vert_uniforms_num);
		j = 0;
		ptr = _ptr;
		for (i = 0; i < cnt; i++) {
			SceGxmProgramParameter *param = (SceGxmProgramParameter *)ptr;
			SceGxmParameterCategory cat = sceGxmProgramParameterGetCategory(param);
			if (cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
				uint8_t texunit_idx = sceGxmProgramParameterGetResourceIndex(param) + 1;
				if (p->max_vert_texunit_idx < texunit_idx)
					p->max_vert_texunit_idx = texunit_idx;
				uniform *u = &p->vert_uniforms[j++];
				u->ptr = param;
				u->type = sceGxmProgramParameterIsSamplerCube(param) ? UNIFORM_CUBE_SAMPLER : UNIFORM_SAMPLER;
				u->sampler_index = 0;
				p->vert_texunits[texunit_idx - 1] = u;
			} else if (cat == SCE_GXM_PARAMETER_CATEGORY_UNIFORM && sceGxmProgramParameterGetContainerIndex(param) == UBOS_NUM) {
				uniform *u = &p->vert_uniforms[j++];
				u->ptr = param;
				u->type = UNIFORM_DATA;
				u->vptr = (uint8_t *)p->unif_vbuffer + sceGxmProgramParameterGetResourceIndex(param) * 4;
				u->fptr = get_uniform_alias_data_ptr(p->frag_uniforms, p->frag_uniforms_num, sceGxmProgramParameterGetName(param));
			}
			ptr += 4;
		}
#ifdef HAVE_FFP_SHADER_SUPPORT
		for (int i = 0; i < FFP_BINDS_NUM; i++) {
			SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->vshader->prog, ffp_bind_names[i]);
			if (param) {
				for (int j = 0; j < p->vert_uniforms_num; j++) {
					if (p->vert_uniforms[j].ptr == param) {
						p->ffp_binds[i] = &p->vert_uniforms[j];
						break;
					}
				}
			}
		}
#endif
	}

#ifdef ENABLE_LEGACY_PIPELINE
	// Creating fragment and vertex program via sceGxmShaderPatcher if using vgl* draw pipeline
	if (p->attr_mode != VGL_ATTRIB_REGULAR) {
#ifdef HAVE_PROGRAM_VARIANT_CACHE
		get_vertex_program_variant(p, p->attr, p->attr_num, p->stream, p->attr_mode == VGL_ATTRIB_UNPACKED ? p->attr_num : 1, &p->vprog);
		get_fragment_program_variant(p, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, &p->fprog);
#else
		patch_vertex_program(gxm_shader_patcher, p->vshader->id, p->attr, p->attr_num,
			p->stream, p->attr_mode == VGL_ATTRIB_UNPACKED ? p->attr_num : 1, &p->vprog);
		rebuild_frag_shader(p->fshader->id, &p->fprog, (SceGxmProgram *)p->vshader->prog, is_fbo_float ? SCE_GXM_OUTPUT_REGISTER_FORMAT_HALF4 : SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4);
#endif
		p->is_fbo_float = is_fbo_float;

		// Populating current blend settings
		p->blend_info.raw = blend_info.raw;
	} else
#endif
	{
		// Checking if bound attributes are aligned
		p->has_unaligned_attrs = GL_FALSE;

		for (i = 0; i < p->attr_num; i++) {
			p->attr_map[i] = i;
			if (p->attr[i].regIndex == 0xDEAD) {
				p->has_unaligned_attrs = GL_TRUE;
				break;
			}
		}

		// Fixing attributes mapping cache if in presence of unaligned attributes
		if (p->has_unaligned_attrs) {
			int j = 0;
			for (i = 0; i < p->attr_highest_idx; i++) {
				if (p->attr[i].regIndex != 0xDEAD) {
					p->attr_map[j] = i;
					j++;
				}
			}
		}
	}
}

#ifdef HAVE_UNSAFE_HOT_STATE_ENTRY
static void __attribute__((noinline, cold)) use_program_slow(GLuint prog) {
	vgl_optimizer_program_changed(prog);
	cur_program = prog;
	dirty_shader_frag_unifs = GL_TRUE;
	dirty_shader_vert_unifs = GL_TRUE;
	dirty_vert_unifs = 0xFFFF;
	dirty_frag_unifs = 0xFFFFFFFF;
}

void __attribute__((hot)) glUseProgram(GLuint prog) {
	THREAD_SAFE()

	if (__builtin_expect(cur_program == prog, 1)) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
	vgl_optimizer_gl_state(GL_FALSE);
	use_program_slow(prog);
}
#else
void glUseProgram(GLuint prog) {
	THREAD_SAFE()

#ifdef HAVE_GL_BIND_CACHE
	if (cur_program == prog) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
	vgl_optimizer_gl_state(GL_FALSE);
#endif

	vgl_optimizer_program_changed(prog);

	// Setting current custom program to passed program
	cur_program = prog;
	dirty_shader_frag_unifs = GL_TRUE;
	dirty_shader_vert_unifs = GL_TRUE;
	dirty_vert_unifs = 0xFFFF;
	dirty_frag_unifs = 0xFFFFFFFF;
}
#endif

GLuint glGetUniformBlockIndex(GLuint prog, const GLchar *uniformBlockName) {
	// Grabbing passed program
	program *p = &progs[prog - 1];

	// Getting the desired location
	ubo *j = p->vert_ubos;
	while (j) {
		block_uniform *b = (block_uniform *)j->ptr;
		if (!strcmp(b->name, uniformBlockName))
			return j->alias ? (GLuint)j->alias : (GLuint)j;
		j = j->chain;
	}
	j = p->frag_ubos;
	while (j) {
		block_uniform *b = (block_uniform *)j->ptr;
		if (!strcmp(b->name, uniformBlockName))
			return (GLuint)j;
		j = j->chain;
	}

	return GL_INVALID_INDEX;
}

void glUniformBlockBinding(GLuint prog, GLuint uniformBlockIndex, GLuint uniformBlockBinding) {
	THREAD_SAFE()

	ubo *u = (ubo *)uniformBlockIndex;
	u->bind = uniformBlockBinding;
}

GLint glGetUniformLocation(GLuint prog, const GLchar *name) {
	// Grabbing passed program
	program *p = &progs[prog - 1];

	// texture, sampler and matrix are reserved keywords in CG but are not in GLSL
	if (!strcmp(name, "texture"))
		name = "vgl_tex";
	else if (!strcmp(name, "Texture"))
		name = "Vgl_tex";
	else if (!strcmp(name, "matrix"))
		name = "_matrix";
	else if (!strcmp(name, "Matrix"))
		name = "_Matrix";
	else if (!strcmp(name, "sampler"))
		name = "_sampler";

#ifdef STRICT_UNIFORMS_COMPLIANCE
	int index = 0;
	char tmp[64];
	char *start = strstr(name, "[");
	if (start) {
		strcpy(tmp, name);
		start = tmp + (start - name);
		name = tmp;
		char *end = strstr(start + 1, "]");
		start[0] = end[0] = 0;
		index = atoi(start + 1);
	}

	uniform_location ret;
#endif
	// Checking if parameter is a vertex or fragment related one
	uniform *j;
	uint32_t cnt;
	const SceGxmProgramParameter *u = sceGxmProgramFindParameterByName(p->vshader->prog, name);
	if (u == NULL) {
		u = sceGxmProgramFindParameterByName(p->fshader->prog, name);
		if (u == NULL) {
			return -1;
		} else {
			j = p->frag_uniforms;
			cnt = p->frag_uniforms_num;
#ifdef STRICT_UNIFORMS_COMPLIANCE
			ret.is_vertex = GL_FALSE;
#endif
		}
	} else {
		j = p->vert_uniforms;
		cnt = p->vert_uniforms_num;
#ifdef STRICT_UNIFORMS_COMPLIANCE
		ret.is_vertex = GL_TRUE;
#endif
	}

	// Getting the desired location
	for (uint32_t i = 0; i < cnt; i++) {
		if (j[i].ptr == u) {
#ifdef STRICT_UNIFORMS_COMPLIANCE			
			ret.offset = index;
			ret.zero = 0;
			ret.program_idx = prog - 1;
			ret.uniform_idx = i;
			return ret.raw;
#else
			return -((GLint)&j[i]);
#endif
		}
	}

	return -1;
}

#define vgl_fill_uniform_data(p, type, size, count) \
	if (vgl_optimizer_uniform_changed(location, p, size, count, type)) { \
		if (u->vptr) { \
			vglSetUniformData(u->vptr, sceGxmProgramParameterGetType(u->ptr), offs, count, size, p, type); \
			dirty_shader_vert_unifs = GL_TRUE; \
		} \
		if (u->fptr) { \
			vglSetUniformData(u->fptr, sceGxmProgramParameterGetType(u->ptr), offs, count, size, p, type); \
			dirty_shader_frag_unifs = GL_TRUE; \
		} \
	}

inline void glUniform1i(GLint location, GLint v0) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	if (u->type != UNIFORM_DATA) { // Sampler
		u->sampler_index = v0;
	} else { // Regular Uniform
		vgl_fill_uniform_data(&v0, SCE_GXM_PARAMETER_TYPE_S32, 1, 1)
	}
}

void glProgramUniform1i(GLuint prog, GLint location, GLint v0) {
	glUniform1i(location, v0);
}

inline void glUniform1iv(GLint location, GLsizei count, const GLint *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);
	
	// Setting passed value to desired uniform
	if (u->type != UNIFORM_DATA) { // Sampler
		u->sampler_index = value[0];
	} else { // Regular Uniform
		vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_S32, 1, count)
	}
}

void glProgramUniform1iv(GLuint prog, GLint location, GLsizei count, const GLint *value) {
	glUniform1iv(location, count, value);
}

inline void glUniform1f(GLint location, GLfloat v0) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);
	
	// Setting passed value to desired uniform
	vgl_fill_uniform_data(&v0, SCE_GXM_PARAMETER_TYPE_F32, 1, 1)
}

void glProgramUniform1f(GLuint prog, GLint location, GLfloat v0) {
	glUniform1f(location, v0);
}

inline void glUniform1fv(GLint location, GLsizei count, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 1, count)
}

void glProgramUniform1fv(GLuint prog, GLint location, GLsizei count, const GLfloat *value) {
	glUniform1fv(location, count, value);
}

inline void glUniform2i(GLint location, GLint v0, GLint v1) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLint src[2] = {v0, v1};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_S32, 2, 1)
}

void glProgramUniform2i(GLuint prog, GLint location, GLint v0, GLint v1) {
	glUniform2i(location, v0, v1);
}

inline void glUniform2iv(GLint location, GLsizei count, const GLint *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_S32, 2, count)
}

void glProgramUniform2iv(GLuint prog, GLint location, GLsizei count, const GLint *value) {
	glUniform2iv(location, count, value);
}

inline void glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLfloat src[2] = {v0, v1};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_F32, 2, 1)
}

void glProgramUniform2f(GLuint prog, GLint location, GLfloat v0, GLfloat v1) {
	glUniform2f(location, v0, v1);
}

inline void glUniform2fv(GLint location, GLsizei count, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 2, count)
}

void glProgramUniform2fv(GLuint prog, GLint location, GLsizei count, const GLfloat *value) {
	glUniform2fv(location, count, value);
}

inline void glUniform3i(GLint location, GLint v0, GLint v1, GLint v2) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLint src[3] = {v0, v1, v2};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_S32, 3, 1)
}

void glProgramUniform3i(GLuint prog, GLint location, GLint v0, GLint v1, GLint v2) {
	glUniform3i(location, v0, v1, v2);
}

inline void glUniform3iv(GLint location, GLsizei count, const GLint *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_S32, 3, count)
}

void glProgramUniform3iv(GLuint prog, GLint location, GLsizei count, const GLint *value) {
	glUniform3iv(location, count, value);
}

inline void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLfloat src[3] = {v0, v1, v2};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_F32, 3, 1)
}

void glProgramUniform3f(GLuint prog, GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
	glUniform3f(location, v0, v1, v2);
}

inline void glUniform3fv(GLint location, GLsizei count, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 3, count)
}

void glProgramUniform3fv(GLuint prog, GLint location, GLsizei count, const GLfloat *value) {
	glUniform3fv(location, count, value);
}

inline void glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLint src[4] = {v0, v1, v2, v3};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_S32, 4, 1)
}

void glProgramUniform4i(GLuint prog, GLint location, GLint v0, GLint v1, GLint v2, GLint v3) {
	glUniform4i(location, v0, v1, v2, v3);
}

inline void glUniform4iv(GLint location, GLsizei count, const GLint *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_S32, 4, 1)
}

void glProgramUniform4iv(GLuint prog, GLint location, GLsizei count, const GLint *value) {
	glUniform4iv(location, count, value);
}

inline void glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	GLfloat src[4] = {v0, v1, v2, v3};
	vgl_fill_uniform_data(src, SCE_GXM_PARAMETER_TYPE_F32, 4, 1)
}

void glProgramUniform4f(GLuint prog, GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
	glUniform4f(location, v0, v1, v2, v3);
}

inline void glUniform4fv(GLint location, GLsizei count, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 4, count)
}

void glProgramUniform4fv(GLuint prog, GLint location, GLsizei count, const GLfloat *value) {
	glUniform4fv(location, count, value);
}

inline void glUniformMatrix2fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	if (transpose) {
		float *tmp = gpu_alloc_mapped_temp(count * 4 * sizeof(float));
		for (int i = 0; i < count; i++) {
			matrix2x2_transpose(&tmp[i * 4], &value[i * 4]);
		}
		value = tmp;
	}
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 2, 2 * count)
}

void glProgramUniformMatrix2fv(GLuint prog, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	glUniformMatrix2fv(location, count, transpose, value);
}

inline void glUniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0)
		return;

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	if (transpose) {
		float *tmp = gpu_alloc_mapped_temp(count * 9 * sizeof(float));
		for (int i = 0; i < count; i++) {
			matrix3x3_transpose(&tmp[i * 9], &value[i * 9]);
		}
		value = tmp;
	}
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 3, 3 * count)
}

void glProgramUniformMatrix3fv(GLuint prog, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	glUniformMatrix3fv(location, count, transpose, value);
}

inline void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	THREAD_SAFE()

	// Checking if the uniform does exist
	if (location == -1 || location == 0) {
		return;
	}

	// Grabbing passed uniform
	int offs = 0;
	uniform *u = (uniform *)get_uniform_from_ptr(location, &offs);

	// Setting passed value to desired uniform
	if (transpose) {
		float *tmp = gpu_alloc_mapped_temp(count * 16 * sizeof(float));
		for (int i = 0; i < count; i++) {
			matrix3x3_transpose(&tmp[i * 16], &value[i * 16]);
		}
		value = tmp;
	}
	vgl_fill_uniform_data(value, SCE_GXM_PARAMETER_TYPE_F32, 4, 4 * count)
}

void glProgramUniformMatrix4fv(GLuint prog, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
	glUniformMatrix4fv(location, count, transpose, value);
}

void glEnableVertexAttribArray(GLuint index) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	uint32_t attrib_bit = 1 << index;
#ifdef HAVE_GL_BIND_CACHE
	if (cur_vao->vertex_attrib_state & attrib_bit) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
	vgl_optimizer_gl_state(GL_FALSE);
#endif
	cur_vao->vertex_attrib_state |= attrib_bit;
	invalidate_vao_cache(cur_vao);
}

void glDisableVertexAttribArray(GLuint index) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	uint32_t attrib_bit = 1 << index;
#ifdef HAVE_GL_BIND_CACHE
	if (!(cur_vao->vertex_attrib_state & attrib_bit)) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
	vgl_optimizer_gl_state(GL_FALSE);
#endif
	cur_vao->vertex_attrib_state &= ~attrib_bit;
	invalidate_vao_cache(cur_vao);
}

void glGetVertexAttribPointerv(GLuint index, GLenum pname, void **pointer) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	} else if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE);
	}
#endif
	pointer[0] = (void *)cur_vao->vertex_attrib_offsets[index];
}

#ifdef HAVE_UNSAFE_HOT_STATE_ENTRY
static void __attribute__((noinline, cold)) vertex_attrib_pointer_slow(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
#else
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
#endif
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (size < 1 || size > 4 || stride < 0 || index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

#if defined(HAVE_UNSAFE_COMMON_VERTEX_REPEAT_FAST_PATH) && !defined(HAVE_UNSAFE_HOT_STATE_ENTRY)
	if (__builtin_expect(type == GL_FLOAT && index < VERTEX_ATTRIBS_NUM, 1)) {
		unsigned short real_stride = stride ? stride : 4 * size;
		SceGxmVertexAttribute *attributes = &cur_vao->vertex_attrib_config[index];
		SceGxmVertexStream *streams = &cur_vao->vertex_stream_config[index];
		if (cur_vao->vertex_attrib_offsets[index] == (uint32_t)pointer && cur_vao->vertex_attrib_vbo[index] == vertex_array_unit && attributes->format == SCE_GXM_ATTRIBUTE_FORMAT_F32 && attributes->componentCount == size && streams->stride == real_stride) {
			vgl_optimizer_gl_state(GL_TRUE);
			return;
		}

		vgl_optimizer_gl_state(GL_FALSE);
		cur_vao->vertex_attrib_offsets[index] = (uint32_t)pointer;
		cur_vao->vertex_attrib_vbo[index] = vertex_array_unit;
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		attributes->componentCount = size;
		streams->stride = real_stride;
		invalidate_vao_cache(cur_vao);
		return;
	}
#endif

	SceGxmAttributeFormat format;
	unsigned short bpe;
#ifdef HAVE_COMMON_VERTEX_FORMAT_FAST_PATH
	if (type == GL_FLOAT) {
		format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = 4;
	} else
#endif
	switch (type) {
	case GL_HALF_FLOAT:
	case GL_HALF_FLOAT_OES:
		format = SCE_GXM_ATTRIBUTE_FORMAT_F16;
		bpe = 2;
		break;
#ifndef HAVE_COMMON_VERTEX_FORMAT_FAST_PATH
	case GL_FLOAT:
#endif
#ifdef HAVE_FIXED_ATTRIBUTES
	case GL_FIXED:
#endif
		format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = 4;
		break;
	case GL_SHORT:
		format = normalized ? SCE_GXM_ATTRIBUTE_FORMAT_S16N : SCE_GXM_ATTRIBUTE_FORMAT_S16;
		bpe = 2;
		break;
	case GL_UNSIGNED_SHORT:
		format = normalized ? SCE_GXM_ATTRIBUTE_FORMAT_U16N : SCE_GXM_ATTRIBUTE_FORMAT_U16;
		bpe = 2;
		break;
	case GL_BYTE:
		format = normalized ? SCE_GXM_ATTRIBUTE_FORMAT_S8N : SCE_GXM_ATTRIBUTE_FORMAT_S8;
		bpe = 1;
		break;
	case GL_UNSIGNED_BYTE:
		format = normalized ? SCE_GXM_ATTRIBUTE_FORMAT_U8N : SCE_GXM_ATTRIBUTE_FORMAT_U8;
		bpe = 1;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}

	unsigned short real_stride = stride ? stride : bpe * size;
	SceGxmVertexAttribute *attributes = &cur_vao->vertex_attrib_config[index];
	SceGxmVertexStream *streams = &cur_vao->vertex_stream_config[index];
#ifdef HAVE_GL_BIND_CACHE
	if (cur_vao->vertex_attrib_offsets[index] == (uint32_t)pointer && cur_vao->vertex_attrib_vbo[index] == vertex_array_unit && attributes->format == format && attributes->componentCount == size && streams->stride == real_stride) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
	vgl_optimizer_gl_state(GL_FALSE);
#endif

	cur_vao->vertex_attrib_offsets[index] = (uint32_t)pointer;
	cur_vao->vertex_attrib_vbo[index] = vertex_array_unit;
	attributes->format = format;
	attributes->componentCount = size;
	streams->stride = real_stride;
	invalidate_vao_cache(cur_vao);
}

#ifdef HAVE_UNSAFE_HOT_STATE_ENTRY
void __attribute__((hot)) glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (size < 1 || size > 4 || stride < 0 || index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	if (__builtin_expect(type == GL_FLOAT && index < VERTEX_ATTRIBS_NUM, 1)) {
		unsigned short real_stride = stride ? stride : 4 * size;
		SceGxmVertexAttribute *attributes = &cur_vao->vertex_attrib_config[index];
		SceGxmVertexStream *streams = &cur_vao->vertex_stream_config[index];
		if (cur_vao->vertex_attrib_offsets[index] == (uint32_t)pointer && cur_vao->vertex_attrib_vbo[index] == vertex_array_unit && attributes->format == SCE_GXM_ATTRIBUTE_FORMAT_F32 && attributes->componentCount == size && streams->stride == real_stride) {
			vgl_optimizer_gl_state(GL_TRUE);
			return;
		}

		vgl_optimizer_gl_state(GL_FALSE);
		cur_vao->vertex_attrib_offsets[index] = (uint32_t)pointer;
		cur_vao->vertex_attrib_vbo[index] = vertex_array_unit;
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		attributes->componentCount = size;
		streams->stride = real_stride;
		invalidate_vao_cache(cur_vao);
		return;
	}

	vertex_attrib_pointer_slow(index, size, type, normalized, stride, pointer);
}
#endif

void glVertexAttribDivisor(GLuint index, GLuint divisor) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)		
	}
	if (divisor > 1) {
		vgl_log("%s:%d %s: Divisor value higher than 1 is not supported. Downgrading to 1.\n", __FILE__, __LINE__, __func__);
	}
#endif

	uint32_t attrib_bit = 1 << index;
#ifdef HAVE_GL_BIND_CACHE
	if (!!(cur_vao->vertex_attrib_divisor & attrib_bit) == !!divisor) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
	vgl_optimizer_gl_state(GL_FALSE);
#endif
	if (divisor) {
		cur_vao->vertex_attrib_divisor |= attrib_bit;
	} else {
		cur_vao->vertex_attrib_divisor &= ~attrib_bit;
	}
	invalidate_vao_cache(cur_vao);
}

void glGetVertexAttribiv(GLuint index, GLenum pname, GLint *params) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	switch (pname) {
	case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_attrib_vbo[index] : 0;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? GL_TRUE : GL_FALSE;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_SIZE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_attrib_config[index].componentCount : cur_vao->vertex_attrib_size[index];
		break;
	case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_stream_config[index].stride : 0;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_TYPE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? gxm_vd_fmt_to_gl(cur_vao->vertex_attrib_config[index].format) : GL_FLOAT;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? (cur_vao->vertex_attrib_config[index].format >= SCE_GXM_ATTRIBUTE_FORMAT_U8N && cur_vao->vertex_attrib_config[index].format <= SCE_GXM_ATTRIBUTE_FORMAT_S16N) : GL_FALSE;
		break;
	case GL_CURRENT_VERTEX_ATTRIB:
#ifndef SKIP_ERROR_HANDLING
		if (index == 0) {
			SET_GL_ERROR(GL_INVALID_OPERATION)
		}
#endif
		params[0] = cur_vao->vertex_attrib_value[index][0];
		params[1] = cur_vao->vertex_attrib_size[index] > 1 ? cur_vao->vertex_attrib_value[index][1] : 0;
		params[2] = cur_vao->vertex_attrib_size[index] > 2 ? cur_vao->vertex_attrib_value[index][2] : 0;
		params[3] = cur_vao->vertex_attrib_size[index] > 3 ? cur_vao->vertex_attrib_value[index][3] : 1;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

void glGetVertexAttribfv(GLuint index, GLenum pname, GLfloat *params) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (index >= VERTEX_ATTRIBS_NUM) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif
	switch (pname) {
	case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_attrib_vbo[index] : 0;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? GL_TRUE : GL_FALSE;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_SIZE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_attrib_config[index].componentCount : cur_vao->vertex_attrib_size[index];
		break;
	case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? cur_vao->vertex_stream_config[index].stride : 0;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_TYPE:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? gxm_vd_fmt_to_gl(cur_vao->vertex_attrib_config[index].format) : GL_FLOAT;
		break;
	case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
		params[0] = (cur_vao->vertex_attrib_state & (1 << index)) ? (cur_vao->vertex_attrib_config[index].format >= SCE_GXM_ATTRIBUTE_FORMAT_U8N && cur_vao->vertex_attrib_config[index].format <= SCE_GXM_ATTRIBUTE_FORMAT_S16N) : GL_FALSE;
		break;
	case GL_CURRENT_VERTEX_ATTRIB:
#ifndef SKIP_ERROR_HANDLING
		if (index == 0) {
			SET_GL_ERROR(GL_INVALID_OPERATION)
		}
#endif
		params[0] = cur_vao->vertex_attrib_value[index][0];
		params[1] = cur_vao->vertex_attrib_size[index] > 1 ? cur_vao->vertex_attrib_value[index][1] : 0;
		params[2] = cur_vao->vertex_attrib_size[index] > 2 ? cur_vao->vertex_attrib_value[index][2] : 0;
		params[3] = cur_vao->vertex_attrib_size[index] > 3 ? cur_vao->vertex_attrib_value[index][3] : 1;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, pname)
	}
}

void glVertexAttrib1f(GLuint index, GLfloat v0) {
	THREAD_SAFE()

	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(1);
	cur_vao->vertex_attrib_size[index] = 1;
	cur_vao->vertex_attrib_value[index][0] = v0;
	invalidate_vao_cache(cur_vao);
}

void glVertexAttrib2f(GLuint index, GLfloat v0, GLfloat v1) {
	THREAD_SAFE()

	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(2);
	cur_vao->vertex_attrib_size[index] = 2;
	cur_vao->vertex_attrib_value[index][0] = v0;
	cur_vao->vertex_attrib_value[index][1] = v1;
	invalidate_vao_cache(cur_vao);
}

void glVertexAttrib3f(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2) {
	THREAD_SAFE()

	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(3);
	cur_vao->vertex_attrib_size[index] = 3;
	cur_vao->vertex_attrib_value[index][0] = v0;
	cur_vao->vertex_attrib_value[index][1] = v1;
	cur_vao->vertex_attrib_value[index][2] = v2;
	invalidate_vao_cache(cur_vao);
}

void glVertexAttrib4f(GLuint index, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
	THREAD_SAFE()

	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(4);
	cur_vao->vertex_attrib_size[index] = 4;
	cur_vao->vertex_attrib_value[index][0] = v0;
	cur_vao->vertex_attrib_value[index][1] = v1;
	cur_vao->vertex_attrib_value[index][2] = v2;
	cur_vao->vertex_attrib_value[index][3] = v3;
	invalidate_vao_cache(cur_vao);
}

void glVertexAttrib1fv(GLuint index, const GLfloat *v) {
	THREAD_SAFE()

	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(1);
	cur_vao->vertex_attrib_size[index] = 1;
	cur_vao->vertex_attrib_value[index][0] = v[0];
	invalidate_vao_cache(cur_vao);
}

void glVertexAttrib2fv(GLuint index, const GLfloat *v) {
	THREAD_SAFE()

	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(2);
	cur_vao->vertex_attrib_size[index] = 2;
	cur_vao->vertex_attrib_value[index][0] = v[0];
	cur_vao->vertex_attrib_value[index][1] = v[1];
	invalidate_vao_cache(cur_vao);
}

void glVertexAttrib3fv(GLuint index, const GLfloat *v) {
	THREAD_SAFE()

	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(3);
	cur_vao->vertex_attrib_size[index] = 3;
	cur_vao->vertex_attrib_value[index][0] = v[0];
	cur_vao->vertex_attrib_value[index][1] = v[1];
	cur_vao->vertex_attrib_value[index][2] = v[2];
	invalidate_vao_cache(cur_vao);
}

void glVertexAttrib4fv(GLuint index, const GLfloat *v) {
	THREAD_SAFE()

	cur_vao->vertex_attrib_value[index] = reserve_attrib_pool(4);
	cur_vao->vertex_attrib_size[index] = 4;
	cur_vao->vertex_attrib_value[index][0] = v[0];
	cur_vao->vertex_attrib_value[index][1] = v[1];
	cur_vao->vertex_attrib_value[index][2] = v[2];
	cur_vao->vertex_attrib_value[index][3] = v[3];
	invalidate_vao_cache(cur_vao);
}

void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (target != GL_UNIFORM_BUFFER) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	} else if (index >= UBOS_NUM) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, index)
	} else if (size <= 0) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, size)
	}
#endif

	ubo_buf[index] = (vbo *)buffer;
	ubo_offset[index] = offset;
}

void glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (target != GL_UNIFORM_BUFFER) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, target)
	} else if (index >= UBOS_NUM) {
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_VALUE, index)
	}
#endif

	ubo_buf[index] = (vbo *)buffer;
	ubo_offset[index] = 0;
}

void glBindAttribLocation(GLuint prog, GLuint index, const GLchar *name) {
	THREAD_SAFE()

	// Grabbing passed program
	program *p = &progs[prog - 1];
	
	// If we use VGL_MODE_POSTPONED, we perform attributes binding in glLinkProgram
	if (glsl_sema_mode == VGL_MODE_POSTPONED && p->vshader->is_glsl) {
		if (!p->glsl_attr_map)
			p->glsl_attr_map = vglMalloc(sizeof(attr_mapping) * VERTEX_ATTRIBS_NUM);
		p->glsl_attr_map[p->num_glsl_attr].idx = index;
		strcpy(p->glsl_attr_map[p->num_glsl_attr++].name, name);
		return;
	}

	// Looking for desired parameter in requested program
	const SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->vshader->prog, name);
	if (param == NULL || sceGxmProgramParameterGetCategory(param) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
		return;
	uint32_t attr_index = sceGxmProgramParameterGetResourceIndex(param);
	
	// Swapping any previously made bind to the requested attribute
	for (int i = 0; i < p->attr_highest_idx; i++) {
		if (p->attr[i].regIndex == attr_index) {
			p->attr[i].regIndex = p->attr[index].regIndex;
			break;
		}
	}
	
	// Set new binding to the requested attribute
	p->attr[index].regIndex = attr_index;
	if (p->attr_highest_idx <= index)
		p->attr_highest_idx = index + 1;
}

GLint glGetAttribLocation(GLuint prog, const GLchar *name) {
	program *p = &progs[prog - 1];
	const SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->vshader->prog, name);
	if (param == NULL || sceGxmProgramParameterGetCategory(param) != SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
		return -1;
	uint32_t index = sceGxmProgramParameterGetResourceIndex(param);

	// Return requested attribute location
	for (int i = 0; i < p->attr_highest_idx; i++) {
		if (p->attr[i].regIndex == index)
			return i;
	}

	return -1;
}

void glGetActiveAttrib(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (bufSize < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Grabbing passed program
	program *p = &progs[prog - 1];

	int i, cnt = sceGxmProgramGetParameterCount(p->vshader->prog);
	uint32_t *ptr = vglProgramGetParameterBase(p->vshader->prog);
	SceGxmProgramParameter *param;
	for (i = 0; i < cnt; i++) {
		param = (SceGxmProgramParameter *)ptr;
		if (sceGxmProgramParameterGetCategory(param) == SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE && (sceGxmProgramParameterGetResourceIndex(param) / 4) == index)
			break;
		ptr += 4;
	}

	// Copying attribute name
	const char *pname = sceGxmProgramParameterGetName(param);
	bufSize = min(strlen(pname), bufSize - 1);
	if (length)
		*length = bufSize;
	strncpy(name, pname, bufSize);
	name[bufSize] = 0;

	*type = gxm_attr_type_to_gl(sceGxmProgramParameterGetComponentCount(param), sceGxmProgramParameterGetArraySize(param));
	*size = 1;
}

void glGetActiveUniform(GLuint prog, GLuint index, GLsizei bufSize, GLsizei *length, GLint *size, GLenum *type, GLchar *name) {
	THREAD_SAFE()

	// Grabbing passed program
	program *p = &progs[prog - 1];

#ifndef SKIP_ERROR_HANDLING
	if (bufSize < 0 || (index >= p->vert_uniforms_num + p->frag_uniforms_num)) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Grabbing desired uniform
	uniform *u;
	if (index >= p->vert_uniforms_num) {
		u = &p->frag_uniforms[index - p->vert_uniforms_num];
	} else {
		u = &p->vert_uniforms[index];
	}

	// Detecting uniform type
	const char *pname = sceGxmProgramParameterGetName(u->ptr);
	if (sceGxmProgramParameterGetCategory(u->ptr) == SCE_GXM_PARAMETER_CATEGORY_SAMPLER) {
		*type = sceGxmProgramParameterIsSamplerCube(u->ptr) ? GL_SAMPLER_CUBE : GL_SAMPLER_2D;
		*size = 1;
	} else {
		*size = sceGxmProgramParameterGetArraySize(u->ptr);
		*type = gxm_unif_type_to_gl(sceGxmProgramParameterGetType(u->ptr), sceGxmProgramParameterGetComponentCount(u->ptr), size);
		if (*type >= GL_FLOAT_VEC2 && *type <= GL_FLOAT_VEC4 && *size > 1) {
			matrix_uniform *m = p->vshader->mat;
			while (m) {
				if (m->ptr == u->ptr) {
					gxm_unif_to_mat(type, size);
					break;
				}
				m = (matrix_uniform *)m->chain;
			}
			if (!m) {
				m = p->fshader->mat;
				while (m) {
					if (m->ptr == u->ptr) {
						gxm_unif_to_mat(type, size);
						break;
					}
					m = (matrix_uniform *)m->chain;
				}
			}
		}
	}
	
	// Copying uniform name
	if (bufSize == 0) {
		if (length)
			*length = 0;
		return;
	}
	// texture, sampler and matrix are reserved keywords in CG but are not in GLSL
	if (!strcmp(pname, "vgl_tex"))
		pname = "texture";
	else if (!strcmp(pname, "Vgl_tex"))
		pname = "Texture";
	else if (!strcmp(pname, "_matrix"))
		pname = "matrix";
	else if (!strcmp(pname, "_sampler"))
		pname = "sampler";

	bufSize = min(strlen(pname), bufSize - 1);
	if (length)
		*length = bufSize;
	strncpy(name, pname, bufSize);
	name[bufSize] = 0;
}

/*
 * ------------------------------
 * -    VGL_EXT_gxp_shaders     -
 * ------------------------------
 */

// Equivalent of glBindAttribLocation but for sceGxm architecture
void vglBindAttribLocation(GLuint prog, GLuint index, const GLchar *name, const GLuint num, const GLenum type) {
	THREAD_SAFE()

#ifdef ENABLE_LEGACY_PIPELINE
	// Grabbing passed program
	program *p = &progs[prog - 1];
	SceGxmVertexAttribute *attributes = &p->attr[index];
	SceGxmVertexStream *streams = &p->stream[index];

	// Looking for desired parameter in requested program
	const SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->vshader->prog, name);
	if (param == NULL)
		return;

	// Setting stream index and offset values
	attributes->streamIndex = index;
	attributes->offset = 0;

	// Detecting attribute format and size
	int bpe;
	switch (type) {
	case GL_FLOAT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = sizeof(float);
		break;
	case GL_SHORT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
		bpe = sizeof(int16_t);
		break;
	case GL_UNSIGNED_BYTE:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
		bpe = sizeof(uint8_t);
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}

	// Setting various info about the stream
	attributes->componentCount = num;
	attributes->regIndex = sceGxmProgramParameterGetResourceIndex(param);
	streams->stride = bpe * num;
	streams->indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	p->attr_mode = VGL_ATTRIB_UNPACKED;
#endif
}

// Equivalent of glBindAttribLocation but for sceGxm architecture when packed attributes are used
GLint vglBindPackedAttribLocation(GLuint prog, const GLchar *name, const GLuint num, const GLenum type, GLuint offset, GLint stride) {
#ifdef ENABLE_LEGACY_PIPELINE
	// Grabbing passed program
	program *p = &progs[prog - 1];
	SceGxmVertexAttribute *attributes = &p->attr[p->attr_idx];
	SceGxmVertexStream *streams = &p->stream[0];

	// Looking for desired parameter in requested program
	const SceGxmProgramParameter *param = sceGxmProgramFindParameterByName(p->vshader->prog, name);
	if (param == NULL)
		return GL_FALSE;

	// Setting stream index and offset values
	attributes->streamIndex = 0;
	attributes->offset = offset;

	// Detecting attribute format and size
	int bpe;
	switch (type) {
	case GL_FLOAT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
		bpe = sizeof(float);
		break;
	case GL_SHORT:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_S16N;
		bpe = sizeof(int16_t);
		break;
	case GL_UNSIGNED_BYTE:
		attributes->format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
		bpe = sizeof(uint8_t);
		break;
	default:
		SET_GL_ERROR_WITH_RET(GL_INVALID_ENUM, GL_FALSE)
	}

	// Setting various info about the stream
	attributes->componentCount = num;
	attributes->regIndex = sceGxmProgramParameterGetResourceIndex(param);
	streams->stride = stride ? stride : bpe * num;
	streams->indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	p->attr_mode = VGL_ATTRIB_PACKED;
	p->attr_idx++;
#endif
	return GL_TRUE;
}

// Equivalent of glVertexAttribPointer but for sceGxm architecture
void vglVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, GLuint count, const GLvoid *pointer) {
	THREAD_SAFE()

#ifdef ENABLE_LEGACY_PIPELINE
#ifndef SKIP_ERROR_HANDLING
	// Error handling
	if (stride < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Detecting type size
	int bpe;
	switch (type) {
	case GL_FLOAT:
		bpe = sizeof(GLfloat);
		break;
	case GL_SHORT:
		bpe = sizeof(GLshort);
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, type)
	}

	// Allocating enough memory on vitaGL mempool
	void *ptr = gpu_alloc_mapped_temp(count * bpe * size);

	// Copying passed data to vitaGL mempool
	if (stride == 0)
		vgl_fast_memcpy(ptr, pointer, count * bpe * size); // Faster if stride == 0
	else {
		int i;
		uint8_t *dst = (uint8_t *)ptr;
		uint8_t *src = (uint8_t *)pointer;
		for (i = 0; i < count; i++) {
			vgl_fast_memcpy(dst, src, bpe * size);
			dst += (bpe * size);
			src += stride;
		}
	}

	// Setting vertex stream to passed index in sceGxm
	sceGxmSetVertexStream(gxm_context, index, ptr);
#endif
}

void vglVertexAttribPointerMapped(GLuint index, const GLvoid *pointer) {
	THREAD_SAFE()

#ifdef ENABLE_LEGACY_PIPELINE
	// Setting vertex stream to passed index in sceGxm
	sceGxmSetVertexStream(gxm_context, index, pointer);
#endif
}

void vglGetShaderBinary(GLuint handle, GLsizei bufSize, GLsizei *length, void *binary) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (bufSize < 0) {
		SET_GL_ERROR(GL_INVALID_VALUE)
	}
#endif

	// Grabbing passed shader
	shader *s = &shaders[handle - 1];

#ifndef SKIP_ERROR_HANDLING
	if (s->prog == NULL) {
		SET_GL_ERROR(GL_INVALID_OPERATION)
	}
#endif

	size_t sz;
	serialize_shader(binary, length ? (size_t *)length : &sz, s, GL_FALSE);
}

void vglAddSemanticBinding(const GLchar *const *varying, GLint index, GLenum type) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (glsl_custom_bindings_num >= MAX_CUSTOM_BINDINGS) {
		vgl_log("%s:%d %s: Too many custom bindings supplied. Consider increasing MAX_CUSTOM_BINDINGS.\n", __FILE__, __LINE__, __func__);
		return;
	}			
#endif
	strcpy(glsl_custom_bindings[glsl_custom_bindings_num].name, varying);
	glsl_custom_bindings[glsl_custom_bindings_num].idx = index;
	glsl_custom_bindings[glsl_custom_bindings_num].type = type;
	glsl_custom_bindings[glsl_custom_bindings_num++].ref_idx = glsl_current_ref_idx;
}

void vglAddSemanticBindingHint(const GLchar *const *varying, GLenum type) {
	THREAD_SAFE()

#ifndef SKIP_ERROR_HANDLING
	if (glsl_custom_bindings_num >= MAX_CUSTOM_BINDINGS) {
		vgl_log("%s:%d %s: Too many custom bindings supplied. Consider increasing MAX_CUSTOM_BINDINGS.\n", __FILE__, __LINE__, __func__);
		return;
	}			
#endif
	strcpy(glsl_custom_bindings[glsl_custom_bindings_num].name, varying);
	glsl_custom_bindings[glsl_custom_bindings_num].idx = -1;
	glsl_custom_bindings[glsl_custom_bindings_num].type = type;
	glsl_custom_bindings[glsl_custom_bindings_num++].ref_idx = glsl_current_ref_idx;
}

void vglUseLowPrecision(GLboolean val) {
	glsl_precision_low = val;
}

void vglSetSemanticBindingMode(GLenum mode) {
	glsl_sema_mode = mode;
}

void vglOverrideTexFormat(GLenum target) {
	THREAD_SAFE()

#ifdef HAVE_UNPURE_TEXFORMATS
	switch (target) {
	case GL_TEXTURE_1D:
		tex2d_override = 1;
		break;
	default:
		tex2d_override = 0;
		break;
	}
#endif
}

void vglShaderGxpBinary(GLsizei count, const GLuint *handles, const void *binary, GLsizei length) {
	THREAD_SAFE()

	// Grabbing passed shader
	shader *s = &shaders[handles[0] - 1];
	
	s->size = length;
	s->prog = (SceGxmProgram *)vglMalloc(s->size);
	vgl_fast_memcpy((SceGxmProgram *)s->prog, binary, s->size);
	sceGxmShaderPatcherRegisterProgram(gxm_shader_patcher, s->prog, &s->id);
	s->unif_buf_size = sceGxmProgramGetDefaultUniformBufferSize(s->prog);
}
