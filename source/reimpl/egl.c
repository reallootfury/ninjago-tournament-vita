/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See licenses/upstream-MIT.txt for details.
 */

#include "reimpl/egl.h"

#include "utils/glutil.h"
#include "utils/logger.h"

#include <string.h>
#include <stdlib.h>

static char display_token;
static char config_token;
static char context_token;
static char window_surface_token;
static char pbuffer_surface_token;
static EGLDisplay current_display;
static EGLContext current_context;
static EGLSurface current_draw_surface;
static EGLSurface current_read_surface;
static EGLBoolean graphics_initialized;

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
    l_debug("eglInitialize(0x%x)", (int)dpy);

    if (!graphics_initialized) {
        gl_init();
        graphics_initialized = EGL_TRUE;
    }
    current_display = dpy ? dpy : (EGLDisplay)&display_token;

    if (major) *major = 2;
    if (minor) *minor = 2;

    return EGL_TRUE;
}

EGLBoolean eglQueryContext(EGLDisplay dpy, EGLContext ctx, EGLint attribute,
                           EGLint *value) {
    EGLBoolean ret = EGL_TRUE;
    switch (attribute) {
        case EGL_CONFIG_ID:
            *value = 0;
            break;
        case EGL_CONTEXT_CLIENT_TYPE:
            *value = EGL_OPENGL_ES_API;
            break;
        case EGL_CONTEXT_CLIENT_VERSION:
            *value = 2;
            break;
        case EGL_RENDER_BUFFER:
            *value = EGL_BACK_BUFFER;
            break;
        default:
            l_error("eglQueryContext / EGL_BAD_ATTRIBUTE: 0x%x", attribute);
            ret = EGL_FALSE;
            break;
    }

    return ret;
}


EGLBoolean eglQuerySurface(EGLDisplay dpy, EGLSurface eglSurface,
                           EGLint attribute, EGLint *value) {
    EGLBoolean ret = EGL_TRUE;
    switch (attribute) {
        case EGL_CONFIG_ID:
            *value = 0;
            break;
        case EGL_WIDTH:
            *value = 960;
            break;
        case EGL_HEIGHT:
            *value = 544;
            break;
        case EGL_TEXTURE_FORMAT:
            *value = EGL_TEXTURE_RGBA;
            break;
        case EGL_TEXTURE_TARGET:
            *value = EGL_TEXTURE_2D;
            break;
        case EGL_SWAP_BEHAVIOR:
            *value = EGL_BUFFER_PRESERVED;
            break;
        case EGL_LARGEST_PBUFFER:
        case EGL_MIPMAP_TEXTURE:
            *value = EGL_FALSE;
            break;
        case EGL_MIPMAP_LEVEL:
            *value = 0;
            break;
        case EGL_MULTISAMPLE_RESOLVE:
            // ignored when creating the surface, return default
            *value = EGL_MULTISAMPLE_RESOLVE_DEFAULT;
            break;
        case EGL_HORIZONTAL_RESOLUTION:
        case EGL_VERTICAL_RESOLUTION:
            *value = 220 * EGL_DISPLAY_SCALING; // VITA DPI is 220
            break;
        case EGL_PIXEL_ASPECT_RATIO:
            // Please don't ask why * EGL_DISPLAY_SCALING, the document says it
            *value = 960 / 544 * EGL_DISPLAY_SCALING;
            break;
        case EGL_RENDER_BUFFER:
            *value = EGL_BACK_BUFFER;
            break;
        case EGL_VG_COLORSPACE:
            // ignored when creating the surface, return default
            *value = EGL_VG_COLORSPACE_sRGB;
            break;
        case EGL_VG_ALPHA_FORMAT:
            // ignored when creating the surface, return default
            *value = EGL_VG_ALPHA_FORMAT_NONPRE;
            break;
        case EGL_TIMESTAMPS_ANDROID:
            *value = EGL_FALSE;
            break;
        default:
            l_error("eglQuerySurface / EGL_BAD_ATTRIBUTE: 0x%x", attribute);
            ret = EGL_FALSE;
            break;
    }

    return ret;
}


EGLBoolean eglGetConfigAttrib(EGLDisplay display, EGLConfig config,
                              EGLint attribute, EGLint * value) {
    switch (attribute) {
        case EGL_ALPHA_SIZE: {
            *value = 8;
            break;
        }
        case EGL_ALPHA_MASK_SIZE: {
            *value = 8;
            break;
        }
        case EGL_BIND_TO_TEXTURE_RGB: {
            *value = EGL_TRUE;
            break;
        }
        case EGL_BIND_TO_TEXTURE_RGBA: {
            *value = EGL_TRUE;
            break;
        }
        case EGL_BLUE_SIZE: {
            *value = 8;
            break;
        }
        case EGL_BUFFER_SIZE: {
            *value = 32;
            break;
        }
        case EGL_COLOR_BUFFER_TYPE: {
            *value = EGL_RGB_BUFFER;
            break;
        }
        case EGL_CONFIG_CAVEAT: {
            *value = EGL_NONE;
            break;
        }
        case EGL_CONFIG_ID: {
            *value = 0;
            break;
        }
        case EGL_CONFORMANT: {
            *value = 0;
            break;
        }
        case EGL_DEPTH_SIZE: {
            *value = 24;
            break;
        }
        case EGL_GREEN_SIZE: {
            *value = 8;
            break;
        }
        case EGL_LEVEL: {
            *value = 0;
            break;
        }
        case EGL_LUMINANCE_SIZE: {
            *value = 0;
            break;
        }
        case EGL_MAX_PBUFFER_WIDTH: {
            *value = 0;
            break;
        }
        case EGL_MAX_PBUFFER_HEIGHT: {
            *value = 0;
            break;
        }
        case EGL_MAX_PBUFFER_PIXELS: {
            *value = 0;
            break;
        }
        case EGL_MAX_SWAP_INTERVAL: {
            *value = 0;
            break;
        }
        case EGL_MIN_SWAP_INTERVAL: {
            *value = 0;
            break;
        }
        case EGL_NATIVE_RENDERABLE: {
            *value = 0;
            break;
        }
        case EGL_NATIVE_VISUAL_ID: {
            *value = 0;
            break;
        }
        case EGL_NATIVE_VISUAL_TYPE: {
            *value = 0;
            break;
        }
        case EGL_RED_SIZE: {
            *value = 8;
            break;
        }
        case EGL_RENDERABLE_TYPE: {
            *value = EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_BIT;
            break;
        }
        case EGL_SAMPLE_BUFFERS: {
            *value = 0;
            break;
        }
        case EGL_SAMPLES: {
            *value = 0;
            break;
        }
        case EGL_STENCIL_SIZE: {
            *value = 8;
            break;
        }
        case EGL_SURFACE_TYPE: {
            *value = 0 | EGL_WINDOW_BIT;
            break;
        }
        case EGL_TRANSPARENT_TYPE: {
            *value = 0;
            break;
        }
        case EGL_TRANSPARENT_RED_VALUE: {
            *value = 0;
            break;
        }
        case EGL_TRANSPARENT_GREEN_VALUE: {
            *value = 0;
            break;
        }
        case EGL_TRANSPARENT_BLUE_VALUE: {
            *value = 0;
            break;
        }
        default:
            l_error("eglGetConfigAttrib / EGL_BAD_ATTRIBUTE: 0x%x", attribute);
            return EGL_FALSE;
    }
    return EGL_TRUE;
}

EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                           EGLConfig *configs, EGLint config_size,
                           EGLint *num_config) {
    if (!num_config) {
        return EGL_BAD_PARAMETER;
    }

    if (!configs) {
        *num_config = 1;
        return EGL_TRUE;
    }

    *configs = (EGLConfig)&config_token;
    *num_config = 1;

    return EGL_TRUE;
}

EGLContext eglCreateContext(EGLDisplay dpy, EGLConfig config,
                            EGLContext share_context,
                            const EGLint *attrib_list) {
    return (EGLContext)&context_token;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                  void * win, const EGLint *attrib_list) {
    return (EGLSurface)&window_surface_token;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                   const EGLint *attrib_list) {
    (void)dpy;
    (void)config;
    (void)attrib_list;
    return (EGLSurface)&pbuffer_surface_token;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read,
                          EGLContext ctx) {
    current_display = dpy;
    current_context = ctx;
    current_draw_surface = draw;
    current_read_surface = read;
    return EGL_TRUE;
}

EGLBoolean eglDestroyContext (EGLDisplay dpy, EGLContext ctx) {
    (void)dpy;
    if (current_context == ctx) current_context = NULL;
    return EGL_TRUE;
}

EGLBoolean eglDestroySurface (EGLDisplay dpy, EGLSurface surface) {
    (void)dpy;
    if (current_draw_surface == surface) current_draw_surface = NULL;
    if (current_read_surface == surface) current_read_surface = NULL;
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy) {
    return EGL_TRUE;
}

EGLContext eglGetCurrentContext (void) {
    return current_context;
}

EGLDisplay eglGetCurrentDisplay(void) {
    return current_display;
}

EGLSurface eglGetCurrentSurface(EGLint which) {
    if (which == EGL_DRAW) return current_draw_surface;
    if (which == EGL_READ) return current_read_surface;
    return NULL;
}

EGLBoolean eglSwapInterval(EGLDisplay display, EGLint interval) {
    (void)display;
    (void)interval;
    return EGL_TRUE;
}

EGLBoolean eglSwapBuffers_soloader(EGLDisplay display, EGLSurface surface) {
    (void)display;
    (void)surface;
    gl_swap();
    return EGL_TRUE;
}

void egl_host_prepare(void) {
    EGLDisplay display = (EGLDisplay)&display_token;
    eglInitialize(display, NULL, NULL);
    eglMakeCurrent(display,
                   (EGLSurface)&window_surface_token,
                   (EGLSurface)&window_surface_token,
                   (EGLContext)&context_token);
}

char const * eglQueryString(EGLDisplay display, EGLint name) {
    switch (name) {
    case EGL_CLIENT_APIS:
        return "OpenGL OpenGL_ES";
    case EGL_VENDOR:
        return "VitaGL Ninjago compatibility layer";
    case EGL_VERSION:
        return "2.2 VitaGL";
    case EGL_EXTENSIONS:
        return "";
    default:
        return NULL;
    }
}

EGLBoolean eglGetConfigs(EGLDisplay display, EGLConfig * configs,
                         EGLint config_size, EGLint * num_config) {
    if (!num_config) {
        l_error("eglGetConfigs / EGL_BAD_PARAMETER");
        return EGL_FALSE;
    }

    if (configs && config_size > 0) {
        *configs = (EGLConfig)&config_token;
    }

    *num_config = 1;

    return EGL_TRUE;
}
