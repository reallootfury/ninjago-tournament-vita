/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See licenses/upstream-MIT.txt for details.
 */

/**
 * @file  glutil.h
 * @brief OpenGL API initializer, related functions.
 */

#ifndef SOLOADER_GLUTIL_H
#define SOLOADER_GLUTIL_H

#include <vitaGL.h>

#ifdef __cplusplus
extern "C" {
#endif

void gl_init();

void gl_preload();

void gl_swap();

void gl_heart_draw_trace_begin(unsigned int transition);
void gl_heart_draw_trace_end(void);
void glEnable_soloader(GLenum cap);
void glDisable_soloader(GLenum cap);

typedef struct GlPerformanceCounters {
    unsigned int draw_calls;
    unsigned int redundant_state_binds_skipped;
    unsigned int main_gl_mutex_contentions;
    unsigned int background_gl_mutex_contentions;
    uint64_t main_gl_mutex_wait_us;
    uint64_t background_gl_mutex_wait_us;
    unsigned int gpu_finish_waits;
    uint64_t gpu_finish_wait_us;
    unsigned int framebuffer_async_transitions;
    unsigned int shader_compiles;
    unsigned int program_links;
    uint64_t shader_compile_us;
    uint64_t program_link_us;
    unsigned int duplicate_compile_finishes_skipped;
    unsigned int compressed_texture_calls;
    uint64_t compressed_texture_bytes;
    uint64_t compressed_texture_upload_us;
    unsigned int atc_base_uploads;
    uint64_t atc_base_upload_us;
    unsigned int cached_program_materializations;
    uint64_t cached_program_materialization_us;
} GlPerformanceCounters;

void gl_get_performance_counters(GlPerformanceCounters *counters);

void glCompileShader_soloader(GLuint shader);

GLuint glCreateProgram_soloader(void);

GLuint glCreateShader_soloader(GLenum type);

void glAttachShader_soloader(GLuint program, GLuint shader);

void glBindAttribLocation_soloader(GLuint program, GLuint index,
                                   const GLchar *name);

void glLinkProgram_soloader(GLuint program);

void glGetProgramiv_soloader(GLuint program, GLenum pname, GLint *params);

void glGetProgramBinary_soloader(GLuint program, GLsizei buf_size,
                                 GLsizei *length, GLenum *binary_format,
                                 void *binary);

void glProgramBinary_soloader(GLuint program, GLenum binary_format,
                              const void *binary, GLsizei length);

GLint glGetUniformLocation_soloader(GLuint program, const GLchar *name);

void (*eglGetProcAddress_soloader(const char *procname))(void);

void glFinish_soloader(void);

void glFlush_soloader(void);

void glBindFramebuffer_soloader(GLenum target, GLuint framebuffer);

void glDrawArrays_soloader(GLenum mode, GLint first, GLsizei count);

void glDrawElements_soloader(GLenum mode, GLsizei count, GLenum type,
                             const void *indices);

void glDisableVertexAttribArray_soloader(GLuint index);

void glEnableVertexAttribArray_soloader(GLuint index);

void glActiveTexture_soloader(GLenum texture);
void glBindBuffer_soloader(GLenum target, GLuint buffer);
void glBindRenderbuffer_soloader(GLenum target, GLuint renderbuffer);
void glBindTexture_soloader(GLenum target, GLuint texture);
void glBufferData_soloader(GLenum target, GLsizeiptr size,
                           const GLvoid *data, GLenum usage);
void glBufferSubData_soloader(GLenum target, GLintptr offset,
                              GLsizeiptr size, const GLvoid *data);
GLenum glCheckFramebufferStatus_soloader(GLenum target);
void glClear_soloader(GLbitfield mask);
void glCompressedTexImage2D_soloader(
    GLenum target, GLint level, GLenum internalformat, GLsizei width,
    GLsizei height, GLint border, GLsizei imageSize, const GLvoid *data);
void glCopyTexImage2D_soloader(
    GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
    GLsizei width, GLsizei height, GLint border);
void glCopyTexSubImage2D_soloader(
    GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
    GLsizei width, GLsizei height);
void glDeleteBuffers_soloader(GLsizei n, const GLuint *buffers);
void glDeleteFramebuffers_soloader(GLsizei n, const GLuint *framebuffers);
void glDeleteProgram_soloader(GLuint program);
void glDeleteRenderbuffers_soloader(GLsizei n, const GLuint *renderbuffers);
void glDeleteShader_soloader(GLuint shader);
void glDeleteTextures_soloader(GLsizei n, const GLuint *textures);
void glFramebufferRenderbuffer_soloader(
    GLenum target, GLenum attachment, GLenum renderbuffertarget,
    GLuint renderbuffer);
void glFramebufferTexture2D_soloader(
    GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
    GLint level);
void glGenBuffers_soloader(GLsizei n, GLuint *buffers);
void glGenerateMipmap_soloader(GLenum target);
void glGenFramebuffers_soloader(GLsizei n, GLuint *framebuffers);
void glGenRenderbuffers_soloader(GLsizei n, GLuint *renderbuffers);
void glGenTextures_soloader(GLsizei n, GLuint *textures);
void glUseProgram_soloader(GLuint program);
void glGetBufferParameteriv_soloader(
    GLenum target, GLenum pname, GLint *params);
void glReadPixels_soloader(
    GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
    GLenum type, GLvoid *pixels);
void glRenderbufferStorage_soloader(
    GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
void glScissor_soloader(GLint x, GLint y, GLsizei width, GLsizei height);
void glTexImage2D_soloader(
    GLenum target, GLint level, GLint internalformat, GLsizei width,
    GLsizei height, GLint border, GLenum format, GLenum type,
    const GLvoid *pixels);
void glTexParameterf_soloader(GLenum target, GLenum pname, GLfloat param);
void glTexParameteri_soloader(GLenum target, GLenum pname, GLint param);
void glTexParameteriv_soloader(
    GLenum target, GLenum pname, const GLint *params);
void glTexParameterx_soloader(GLenum target, GLenum pname, GLfixed param);
void glTexSubImage2D_soloader(
    GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
    GLsizei height, GLenum format, GLenum type, const GLvoid *pixels);
void glVertexAttribPointer_soloader(
    GLuint index, GLint size, GLenum type, GLboolean normalized,
    GLsizei stride, const void *pointer);
void glViewport_soloader(
    GLint x, GLint y, GLsizei width, GLsizei height);

void glUniform1f_soloader(GLint location, GLfloat v0);
void glUniform1fv_soloader(GLint location, GLsizei count,
                           const GLfloat *value);
void glUniform1i_soloader(GLint location, GLint v0);
void glUniform1iv_soloader(GLint location, GLsizei count,
                           const GLint *value);
void glUniform2f_soloader(GLint location, GLfloat v0, GLfloat v1);
void glUniform2fv_soloader(GLint location, GLsizei count,
                           const GLfloat *value);
void glUniform2i_soloader(GLint location, GLint v0, GLint v1);
void glUniform2iv_soloader(GLint location, GLsizei count,
                           const GLint *value);
void glUniform3f_soloader(GLint location, GLfloat v0, GLfloat v1,
                          GLfloat v2);
void glUniform3fv_soloader(GLint location, GLsizei count,
                           const GLfloat *value);
void glUniform3i_soloader(GLint location, GLint v0, GLint v1, GLint v2);
void glUniform3iv_soloader(GLint location, GLsizei count,
                           const GLint *value);
void glUniform4f_soloader(GLint location, GLfloat v0, GLfloat v1,
                          GLfloat v2, GLfloat v3);
void glUniform4fv_soloader(GLint location, GLsizei count,
                           const GLfloat *value);
void glUniform4i_soloader(GLint location, GLint v0, GLint v1, GLint v2,
                          GLint v3);
void glUniform4iv_soloader(GLint location, GLsizei count,
                           const GLint *value);
void glUniformMatrix2fv_soloader(GLint location, GLsizei count,
                                 GLboolean transpose,
                                 const GLfloat *value);
void glUniformMatrix3fv_soloader(GLint location, GLsizei count,
                                 GLboolean transpose,
                                 const GLfloat *value);
void glUniformMatrix4fv_soloader(GLint location, GLsizei count,
                                 GLboolean transpose,
                                 const GLfloat *value);

void glShaderSource_soloader(GLuint shader, GLsizei count,
                             const GLchar **string, const GLint *_length);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_GLUTIL_H
