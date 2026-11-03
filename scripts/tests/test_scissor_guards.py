#!/usr/bin/env python3
"""Execute the pinned renderer's real scene/draw predicates on the host.

This detects lost scissor invalidation, not GXM pixel correctness. No timing
results or emulated GPU claims are produced. Pass the extracted renderer root.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("renderer", type=Path)
    args = parser.parse_args()
    shared = (args.renderer / "source/shared.h").read_text()
    draw = (args.renderer / "source/draw.c").read_text()
    scene = re.search(
        r"void scene_reset\(void\) \{\s*if \(([^\n]+)\)\s*\n", shared
    )
    draws = re.findall(
        r"^\s*if \(([^\n]*mode == GL_TRIANGLES[^\n]*)\) \{", draw, re.M
    )
    if scene is None or len(draws) != 2:
        raise SystemExit("Unsupported renderer layout; do not silently skip a guard")
    predicates = [scene.group(1), *draws]
    code = """
#include <stdio.h>
#define GL_TRIANGLES 4
#define MAX_IDX_NUMBER 65535
static int in_use_framebuffer, active_write_fb, needs_scene_reset;
static int dirty_framebuffer, dirty_query, dirty_scissor_state;
static int mode = GL_TRIANGLES, count = 6, no_polygons_mode, cur_program = 13;
"""
    for index, predicate in enumerate(predicates):
        code += f"static int predicate_{index}(void) {{ return !!({predicate}); }}\n"
    code += """
int main(void) {
    int failures = 0;
    /* Exhaustive dirty-state combinations include: clean, scissor-only,
     * and unrelated scene invalidations. A dirty mask must bar BOTH shortcuts. */
    for (unsigned state = 0; state < 32; ++state) {
        in_use_framebuffer = state & 1;
        needs_scene_reset = !!(state & 2);
        dirty_framebuffer = !!(state & 4);
        dirty_query = !!(state & 8);
        dirty_scissor_state = !!(state & 16);
        int want_reset = state != 0;
        int got[] = {predicate_0(), !predicate_1(), !predicate_2()};
        for (int guard = 0; guard < 3; ++guard) {
            if (got[guard] != want_reset) {
                fprintf(stderr, "guard %d state %u: reset=%d, expected=%d\\n",
                        guard, state, got[guard], want_reset);
                ++failures;
            }
        }
    }
    return failures != 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="ninjago-scissor-test-") as temp:
        source = Path(temp) / "guards.c"
        executable = Path(temp) / "guards"
        source.write_text(code)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
    print("PASS: 96 real renderer guard evaluations, including scissor-only changes")


if __name__ == "__main__":
    main()
