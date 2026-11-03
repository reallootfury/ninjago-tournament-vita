/*
 * This file is part of vitaGL
 * Copyright 2017, 2018, 2019, 2020 Rinnegatamante
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
 * blending.c:
 * Implementation for blending related functions
 */

#include "shared.h"

GLboolean blend_state = GL_FALSE; // Current state for GL_BLEND
SceGxmBlendFactor blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE; // Current in use RGB source blend factor
SceGxmBlendFactor blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_ZERO; // Current in use RGB dest blend factor
SceGxmBlendFactor blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE; // Current in use A source blend factor
SceGxmBlendFactor blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ZERO; // Current in use A dest blend factor

blend_config blend_info; // Current blend info mode
#ifdef HAVE_UNSAFE_FAST_BLEND_TOGGLE
uint32_t blend_enabled_raw;
uint32_t blend_disabled_raw;
#endif

SceGxmColorMask blend_color_mask = SCE_GXM_COLOR_MASK_ALL; // Current in-use color mask (glColorMask)
SceGxmBlendFunc blend_func_rgb = SCE_GXM_BLEND_FUNC_ADD; // Current in-use RGB blend func
SceGxmBlendFunc blend_func_a = SCE_GXM_BLEND_FUNC_ADD; // Current in-use A blend func

#ifdef HAVE_GL_BIND_CACHE
static GLenum blend_sfactor_rgb_gl = GL_ONE;
static GLenum blend_dfactor_rgb_gl = GL_ZERO;
static GLenum blend_sfactor_a_gl = GL_ONE;
static GLenum blend_dfactor_a_gl = GL_ZERO;
static GLenum blend_func_rgb_gl = GL_FUNC_ADD;
static GLenum blend_func_a_gl = GL_FUNC_ADD;
static GLboolean blend_factor_cache_valid = GL_TRUE;
static GLboolean blend_equation_cache_valid = GL_TRUE;

void invalidate_blend_state_cache(void) {
	blend_factor_cache_valid = GL_FALSE;
	blend_equation_cache_valid = GL_FALSE;
}
#endif

GLenum gxm_blend_eq_to_gl(SceGxmBlendFunc factor) {
	switch (factor) {
	case SCE_GXM_BLEND_FUNC_ADD:
		return GL_FUNC_ADD;
	case SCE_GXM_BLEND_FUNC_SUBTRACT:
		return GL_FUNC_SUBTRACT;
	case SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT:
		return GL_FUNC_REVERSE_SUBTRACT;
	case SCE_GXM_BLEND_FUNC_MIN:
		return GL_MIN;
	case SCE_GXM_BLEND_FUNC_MAX:
		return GL_MAX;
	default:
		break;
	}

	return 0;
}

GLenum gxm_blend_to_gl(SceGxmBlendFactor factor) {
	switch (factor) {
	case SCE_GXM_BLEND_FACTOR_ZERO:
		return GL_ZERO;
	case SCE_GXM_BLEND_FACTOR_ONE:
		return GL_ONE;
	case SCE_GXM_BLEND_FACTOR_SRC_COLOR:
		return GL_SRC_COLOR;
	case SCE_GXM_BLEND_FACTOR_DST_COLOR:
		return GL_DST_COLOR;
	case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
		return GL_ONE_MINUS_SRC_COLOR;
	case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
		return GL_ONE_MINUS_DST_COLOR;
	case SCE_GXM_BLEND_FACTOR_SRC_ALPHA:
		return GL_SRC_ALPHA;
	case SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
		return GL_ONE_MINUS_SRC_ALPHA;
	case SCE_GXM_BLEND_FACTOR_DST_ALPHA:
		return GL_DST_ALPHA;
	case SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
		return GL_ONE_MINUS_DST_ALPHA;
	case SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE:
		return GL_SRC_ALPHA_SATURATE;
	default:
		break;
	}

	return 0;
}

#ifdef HAVE_UNSAFE_FAST_BLEND_TOGGLE
static void rebuild_blend_modes(void) {
	blend_config enabled = {.raw = 0};
	blend_config disabled = {.raw = 0};
	enabled.info.colorMask = disabled.info.colorMask = blend_color_mask;
	enabled.info.colorFunc = blend_func_rgb;
	enabled.info.alphaFunc = blend_func_a;
	enabled.info.colorSrc = blend_sfactor_rgb;
	enabled.info.colorDst = blend_dfactor_rgb;
	enabled.info.alphaSrc = blend_sfactor_a;
	enabled.info.alphaDst = blend_dfactor_a;
	disabled.info.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
	disabled.info.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
	disabled.info.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
	disabled.info.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	disabled.info.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
	disabled.info.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
	blend_enabled_raw = enabled.raw;
	blend_disabled_raw = disabled.raw;
}
#endif

void change_blend_factor() {
#ifdef HAVE_UNSAFE_FAST_BLEND_TOGGLE
	rebuild_blend_modes();
	blend_info.raw = blend_enabled_raw;
#else
	blend_info.info.colorMask = blend_color_mask;
	blend_info.info.colorFunc = blend_func_rgb;
	blend_info.info.alphaFunc = blend_func_a;
	blend_info.info.colorSrc = blend_sfactor_rgb;
	blend_info.info.colorDst = blend_dfactor_rgb;
	blend_info.info.alphaSrc = blend_sfactor_a;
	blend_info.info.alphaDst = blend_dfactor_a;
#endif
}

void change_blend_mask() {
#ifdef HAVE_UNSAFE_FAST_BLEND_TOGGLE
	rebuild_blend_modes();
	blend_info.raw = blend_disabled_raw;
#else
	blend_info.info.colorMask = blend_color_mask;
	blend_info.info.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
	blend_info.info.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
	blend_info.info.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
	blend_info.info.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_info.info.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
	blend_info.info.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
#endif
}

/*
 * ------------------------------
 * - IMPLEMENTATION STARTS HERE -
 * ------------------------------
 */

#ifdef HAVE_UNSAFE_HOT_STATE_ENTRY
static void __attribute__((noinline, cold)) blend_func_slow(GLenum sfactor, GLenum dfactor) {
#else
void glBlendFunc(GLenum sfactor, GLenum dfactor) {
#endif
	THREAD_SAFE()

#ifdef HAVE_DLISTS
	// Enqueueing function to a display list if one is being compiled
	if (_vgl_enqueue_list_func(glBlendFunc, DLIST_FUNC_U32_U32, sfactor, dfactor))
		return;
#endif
#ifdef HAVE_GL_BIND_CACHE
	if (blend_factor_cache_valid && blend_sfactor_rgb_gl == sfactor && blend_sfactor_a_gl == sfactor && blend_dfactor_rgb_gl == dfactor && blend_dfactor_a_gl == dfactor) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
#endif
	switch (sfactor) {
	case GL_ZERO:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ZERO;
		break;
	case GL_ONE:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE;
		break;
	case GL_SRC_COLOR:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_SRC_COLOR;
		break;
	case GL_ONE_MINUS_SRC_COLOR:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		break;
	case GL_DST_COLOR:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_DST_COLOR;
		break;
	case GL_ONE_MINUS_DST_COLOR:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		break;
	case GL_SRC_ALPHA:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
		break;
	case GL_ONE_MINUS_SRC_ALPHA:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case GL_DST_ALPHA:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_DST_ALPHA;
		break;
	case GL_ONE_MINUS_DST_ALPHA:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		break;
	case GL_SRC_ALPHA_SATURATE:
		blend_sfactor_rgb = blend_sfactor_a = SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, sfactor)
	}
	switch (dfactor) {
	case GL_ZERO:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ZERO;
		break;
	case GL_ONE:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE;
		break;
	case GL_SRC_COLOR:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_SRC_COLOR;
		break;
	case GL_ONE_MINUS_SRC_COLOR:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		break;
	case GL_DST_COLOR:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_DST_COLOR;
		break;
	case GL_ONE_MINUS_DST_COLOR:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		break;
	case GL_SRC_ALPHA:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
		break;
	case GL_ONE_MINUS_SRC_ALPHA:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case GL_DST_ALPHA:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_DST_ALPHA;
		break;
	case GL_ONE_MINUS_DST_ALPHA:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		break;
	case GL_SRC_ALPHA_SATURATE:
		blend_dfactor_rgb = blend_dfactor_a = SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dfactor)
	}
#ifdef HAVE_GL_BIND_CACHE
	blend_sfactor_rgb_gl = blend_sfactor_a_gl = sfactor;
	blend_dfactor_rgb_gl = blend_dfactor_a_gl = dfactor;
	blend_factor_cache_valid = GL_TRUE;
	vgl_optimizer_gl_state(GL_FALSE);
#endif
	if (blend_state)
		change_blend_factor();
#ifdef HAVE_UNSAFE_FAST_BLEND_TOGGLE
	else
		change_blend_mask();
#endif
}

#ifdef HAVE_UNSAFE_HOT_STATE_ENTRY
void __attribute__((hot)) glBlendFunc(GLenum sfactor, GLenum dfactor) {
	THREAD_SAFE()

#if defined(HAVE_GL_BIND_CACHE) && !defined(HAVE_DLISTS)
	if (__builtin_expect(blend_factor_cache_valid && blend_sfactor_rgb_gl == sfactor && blend_sfactor_a_gl == sfactor && blend_dfactor_rgb_gl == dfactor && blend_dfactor_a_gl == dfactor, 1)) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
#endif
	blend_func_slow(sfactor, dfactor);
}
#endif

void glBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
	THREAD_SAFE()

#ifdef HAVE_DLISTS
	// Enqueueing function to a display list if one is being compiled
	if (_vgl_enqueue_list_func(glBlendFunc, DLIST_FUNC_U32_U32_U32_U32, srcRGB, dstRGB, srcAlpha, dstAlpha))
		return;
#endif
#ifdef HAVE_GL_BIND_CACHE
	if (blend_factor_cache_valid && blend_sfactor_rgb_gl == srcRGB && blend_dfactor_rgb_gl == dstRGB && blend_sfactor_a_gl == srcAlpha && blend_dfactor_a_gl == dstAlpha) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
#endif
	switch (srcRGB) {
	case GL_ZERO:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_ZERO;
		break;
	case GL_ONE:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE;
		break;
	case GL_SRC_COLOR:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_SRC_COLOR;
		break;
	case GL_ONE_MINUS_SRC_COLOR:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		break;
	case GL_DST_COLOR:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_DST_COLOR;
		break;
	case GL_ONE_MINUS_DST_COLOR:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		break;
	case GL_SRC_ALPHA:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
		break;
	case GL_ONE_MINUS_SRC_ALPHA:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case GL_DST_ALPHA:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_DST_ALPHA;
		break;
	case GL_ONE_MINUS_DST_ALPHA:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		break;
	case GL_SRC_ALPHA_SATURATE:
		blend_sfactor_rgb = SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, srcRGB)
	}
	switch (dstRGB) {
	case GL_ZERO:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_ZERO;
		break;
	case GL_ONE:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE;
		break;
	case GL_SRC_COLOR:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_SRC_COLOR;
		break;
	case GL_ONE_MINUS_SRC_COLOR:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		break;
	case GL_DST_COLOR:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_DST_COLOR;
		break;
	case GL_ONE_MINUS_DST_COLOR:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		break;
	case GL_SRC_ALPHA:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
		break;
	case GL_ONE_MINUS_SRC_ALPHA:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case GL_DST_ALPHA:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_DST_ALPHA;
		break;
	case GL_ONE_MINUS_DST_ALPHA:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		break;
	case GL_SRC_ALPHA_SATURATE:
		blend_dfactor_rgb = SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dstRGB)
	}
	switch (srcAlpha) {
	case GL_ZERO:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ZERO;
		break;
	case GL_ONE:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE;
		break;
	case GL_SRC_COLOR:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_SRC_COLOR;
		break;
	case GL_ONE_MINUS_SRC_COLOR:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		break;
	case GL_DST_COLOR:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_DST_COLOR;
		break;
	case GL_ONE_MINUS_DST_COLOR:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		break;
	case GL_SRC_ALPHA:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
		break;
	case GL_ONE_MINUS_SRC_ALPHA:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case GL_DST_ALPHA:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_DST_ALPHA;
		break;
	case GL_ONE_MINUS_DST_ALPHA:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		break;
	case GL_SRC_ALPHA_SATURATE:
		blend_sfactor_a = SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, srcAlpha)
	}
	switch (dstAlpha) {
	case GL_ZERO:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ZERO;
		break;
	case GL_ONE:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE;
		break;
	case GL_SRC_COLOR:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_SRC_COLOR;
		break;
	case GL_ONE_MINUS_SRC_COLOR:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		break;
	case GL_DST_COLOR:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_DST_COLOR;
		break;
	case GL_ONE_MINUS_DST_COLOR:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		break;
	case GL_SRC_ALPHA:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
		break;
	case GL_ONE_MINUS_SRC_ALPHA:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case GL_DST_ALPHA:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_DST_ALPHA;
		break;
	case GL_ONE_MINUS_DST_ALPHA:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		break;
	case GL_SRC_ALPHA_SATURATE:
		blend_dfactor_a = SCE_GXM_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, dstAlpha)
	}
#ifdef HAVE_GL_BIND_CACHE
	blend_sfactor_rgb_gl = srcRGB;
	blend_dfactor_rgb_gl = dstRGB;
	blend_sfactor_a_gl = srcAlpha;
	blend_dfactor_a_gl = dstAlpha;
	blend_factor_cache_valid = GL_TRUE;
	vgl_optimizer_gl_state(GL_FALSE);
#endif
	if (blend_state)
		change_blend_factor();
#ifdef HAVE_UNSAFE_FAST_BLEND_TOGGLE
	else
		change_blend_mask();
#endif
}

void glBlendEquation(GLenum mode) {
	THREAD_SAFE()

#ifdef HAVE_DLISTS
	// Enqueueing function to a display list if one is being compiled
	if (_vgl_enqueue_list_func(glBlendEquation, DLIST_FUNC_U32, mode))
		return;
#endif
#ifdef HAVE_GL_BIND_CACHE
	if (blend_equation_cache_valid && blend_func_rgb_gl == mode && blend_func_a_gl == mode) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
#endif
	switch (mode) {
	case GL_FUNC_ADD:
		blend_func_rgb = blend_func_a = SCE_GXM_BLEND_FUNC_ADD;
		break;
	case GL_FUNC_SUBTRACT:
		blend_func_rgb = blend_func_a = SCE_GXM_BLEND_FUNC_SUBTRACT;
		break;
	case GL_FUNC_REVERSE_SUBTRACT:
		blend_func_rgb = blend_func_a = SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
		break;
	case GL_MIN:
		blend_func_rgb = blend_func_a = SCE_GXM_BLEND_FUNC_MIN;
		break;
	case GL_MAX:
		blend_func_rgb = blend_func_a = SCE_GXM_BLEND_FUNC_MAX;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, mode)
	}
#ifdef HAVE_GL_BIND_CACHE
	blend_func_rgb_gl = blend_func_a_gl = mode;
	blend_equation_cache_valid = GL_TRUE;
	vgl_optimizer_gl_state(GL_FALSE);
#endif
	if (blend_state)
		change_blend_factor();
#ifdef HAVE_UNSAFE_FAST_BLEND_TOGGLE
	else
		change_blend_mask();
#endif
}

void glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
	THREAD_SAFE()

#ifdef HAVE_DLISTS
	// Enqueueing function to a display list if one is being compiled
	if (_vgl_enqueue_list_func(glBlendEquationSeparate, DLIST_FUNC_U32_U32, modeRGB, modeAlpha))
		return;
#endif
#ifdef HAVE_GL_BIND_CACHE
	if (blend_equation_cache_valid && blend_func_rgb_gl == modeRGB && blend_func_a_gl == modeAlpha) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
#endif
	switch (modeRGB) {
	case GL_FUNC_ADD:
		blend_func_rgb = SCE_GXM_BLEND_FUNC_ADD;
		break;
	case GL_FUNC_SUBTRACT:
		blend_func_rgb = SCE_GXM_BLEND_FUNC_SUBTRACT;
		break;
	case GL_FUNC_REVERSE_SUBTRACT:
		blend_func_rgb = SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
		break;
	case GL_MIN:
		blend_func_rgb = SCE_GXM_BLEND_FUNC_MIN;
		break;
	case GL_MAX:
		blend_func_rgb = SCE_GXM_BLEND_FUNC_MAX;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, modeRGB)
	}
	switch (modeAlpha) {
	case GL_FUNC_ADD:
		blend_func_a = SCE_GXM_BLEND_FUNC_ADD;
		break;
	case GL_FUNC_SUBTRACT:
		blend_func_a = SCE_GXM_BLEND_FUNC_SUBTRACT;
		break;
	case GL_FUNC_REVERSE_SUBTRACT:
		blend_func_a = SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT;
		break;
	case GL_MIN:
		blend_func_a = SCE_GXM_BLEND_FUNC_MIN;
		break;
	case GL_MAX:
		blend_func_a = SCE_GXM_BLEND_FUNC_MAX;
		break;
	default:
		SET_GL_ERROR_WITH_VALUE(GL_INVALID_ENUM, modeAlpha)
	}
#ifdef HAVE_GL_BIND_CACHE
	blend_func_rgb_gl = modeRGB;
	blend_func_a_gl = modeAlpha;
	blend_equation_cache_valid = GL_TRUE;
	vgl_optimizer_gl_state(GL_FALSE);
#endif
	if (blend_state)
		change_blend_factor();
#ifdef HAVE_UNSAFE_FAST_BLEND_TOGGLE
	else
		change_blend_mask();
#endif
}

void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
	THREAD_SAFE()

#ifdef HAVE_DLISTS
	// Enqueueing function to a display list if one is being compiled
	if (_vgl_enqueue_list_func(glColorMask, DLIST_FUNC_U8_U8_U8_U8, red, green, blue, alpha))
		return;
#endif
	SceGxmColorMask color_mask = SCE_GXM_COLOR_MASK_NONE;
	if (red)
		color_mask |= SCE_GXM_COLOR_MASK_R;
	if (green)
		color_mask |= SCE_GXM_COLOR_MASK_G;
	if (blue)
		color_mask |= SCE_GXM_COLOR_MASK_B;
	if (alpha)
		color_mask |= SCE_GXM_COLOR_MASK_A;
#ifdef HAVE_GL_BIND_CACHE
	if (blend_color_mask == color_mask) {
		vgl_optimizer_gl_state(GL_TRUE);
		return;
	}
	vgl_optimizer_gl_state(GL_FALSE);
#endif
	blend_color_mask = color_mask;
	if (blend_state)
		change_blend_factor();
	else
		change_blend_mask();
}
