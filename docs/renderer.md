# vitaGL renderer

The release pins renderer source commit
`7e3bfa277b4d9cc96a73707a81832a56b7aeb34d` with the small correctness patch in
`scripts/vitagl-scissor.patch`. Its source and original license notices are
included under `lib/vitagl/`. It retains the established rendering path.

The patch applies pending scissor changes before triangle drawing and restores
the game's uniform-buffer binding after mask drawing. Progressive player and
boss hearts were confirmed working on a physical Vita. Native artwork,
geometry, UVs and health animations are unchanged.

From the repository root:

```bash
export VITASDK="$HOME/vitasdk-softfp"
export PATH="$VITASDK/bin:$PATH"
bash scripts/build_vitagl_scissor.sh
```

The script copies an isolated source tree, applies the patch, runs two host
correctness tests and builds `.deps-softfp/libvitaGL.a`. It does not modify the
included renderer source, install into the SDK, or build a VPK. Original source
licenses remain in both the included source and extracted dependency.

The tests exercise all 96 dirty-state guard combinations and a warm-cache
uniform-buffer restore. They protect CPU contracts, not physical GPU timing.
After renderer changes, test full/damaged/healed player and boss hearts,
backgrounds, pause/quit menus, audio and controls on the Vita. Keep the matching
unstripped ELF with device evidence. Compiler/path changes can affect archive
hashes; do not bypass the CMake archive hash check.
