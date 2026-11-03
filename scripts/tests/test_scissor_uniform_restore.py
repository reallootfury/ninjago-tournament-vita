#!/usr/bin/env python3
"""Execute the scissor restore tail and real cached uniform setter on host.

GXM's setter/reserve effects are represented by one bound-buffer pointer;
renderer cache/restore code comes from the actual pinned source, not a model.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(text, name):
    definition = re.search(r"\b" + re.escape(name) + r"\([^;{}]*\)\s*\{", text)
    if definition is None:
        raise ValueError(f"Missing function definition: {name}")
    begin = definition.end() - 1
    depth = 1
    end = begin + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[begin:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    args = parser.parse_args()
    root = args.renderer / "source"
    header = (root / "vgl_optimizer.h").read_text()
    cache_context = function(header, "vgl_fast_cache_context")
    setter = function(header, "vgl_fast_set_vertex_default_uniform_buffer")
    reset = function((root / "vgl_optimizer.c").read_text(), "vgl_optimizer_scene_reset")
    restore = function((root / "utils/gxm_utils.h").read_text(), "vglRestoreVertexUniformBuffer")
    scissor = function((root / "tests.c").read_text(), "update_scissor_test")
    # Retain the real ordering of cache invalidation and uniform restoration.
    tail = scissor[scissor.rindex("refresh_stencil_settings();"):]
    code = """
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define HAVE_RUNTIME_OPTIMIZER 1
#define GL_TRUE 1
#define GL_FALSE 0
#define SCE_OK 0
#define VGL_FAST_VERTEX_DEFAULT_BUFFER_VALID 4
typedef struct { const void *bound; } SceGxmContext;
typedef struct {
    SceGxmContext *context;
    uint64_t valid;
    const void *vertex_default_uniform;
} vgl_fast_gxm_state;
typedef vgl_fast_gxm_state vgl_gxm_state_cache;
static vgl_fast_gxm_state vgl_fast_state;
static vgl_gxm_state_cache gxm_state_cache;
static int optimizer_enabled = 1, dirty_scissor_state;
static SceGxmContext context;
static SceGxmContext *gxm_context = &context;
static void *vgl_def_vert_buf;
#define vgl_memset memset
/* No precomputed state is active in this fixture. */
static void vgl_optimizer_leave_precomputed_state(SceGxmContext *ctx) { (void)ctx; }
static void refresh_stencil_settings(void) {}
static int sceGxmSetVertexDefaultUniformBuffer(SceGxmContext *ctx, const void *buf) {
    ctx->bound = buf;
    return SCE_OK;
}
"""
    code += "static void vgl_optimizer_scene_reset(void) " + reset + "\n"
    code += "static void vgl_fast_cache_context(SceGxmContext *context) " + cache_context + "\n"
    code += "static int vgl_fast_set_vertex_default_uniform_buffer(SceGxmContext *context, const void *buffer_data) " + setter + "\n"
    code += "#define sceGxmSetVertexDefaultUniformBuffer vgl_fast_set_vertex_default_uniform_buffer\n"
    code += "static void vglRestoreVertexUniformBuffer(void) " + restore + "\n"
    code += "static void finish_scissor(void) {\n" + tail + "\n"
    code += """
int main(void) {
    int game_uniforms, mask_uniforms;
    vgl_def_vert_buf = &game_uniforms;
    vgl_fast_set_vertex_default_uniform_buffer(gxm_context, vgl_def_vert_buf);
    /* Native GXM reserve binds temporary mask uniforms outside the cache. */
    gxm_context->bound = &mask_uniforms;
    dirty_scissor_state = GL_TRUE;
    finish_scissor();
    if (gxm_context->bound != vgl_def_vert_buf || dirty_scissor_state) {
        fputs("FAIL: scissor retained temporary mask uniforms instead of game uniforms\\n", stderr);
        return 1;
    }
    puts("PASS: warm-cache scissor restores the original game uniform binding");
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="ninjago-scissor-restore-") as temp:
        source = Path(temp) / "restore.c"
        executable = Path(temp) / "restore"
        source.write_text(code)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
