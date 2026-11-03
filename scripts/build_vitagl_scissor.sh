#!/usr/bin/env bash
# Rebuild pinned vitaGL with scissor-state guards and uniform restoration.
# Does not modify the supplied repository, install to the SDK, or build VPKs.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
renderer_source="$root/lib/vitagl"
source_dir="$root/.deps-src/vitagl"
patch_file="$root/scripts/vitagl-scissor.patch"
: "${VITASDK:?Export VITASDK to your soft-float VitaSDK directory first}"
export PATH="$VITASDK/bin:$PATH"
if [[ ! -d "$source_dir" ]]; then
    mkdir -p "$source_dir"
    cp -R "$renderer_source/." "$source_dir/"
fi
cp "$renderer_source/Makefile" "$source_dir/Makefile"
if patch --batch --silent -d "$source_dir" -p1 --forward --dry-run < "$patch_file"; then
    patch --batch --silent -d "$source_dir" -p1 --forward < "$patch_file"
else
    # An existing matching extraction is an incremental build, not a reset.
    patch --batch --silent -d "$source_dir" -p1 --reverse --dry-run < "$patch_file"
fi
python3 "$root/scripts/tests/test_scissor_guards.py" "$source_dir"
python3 "$root/scripts/tests/test_scissor_uniform_restore.py" "$source_dir"
make -C "$source_dir" -j"$(nproc)" SOFTFP_ABI=1 VITAGL=1 \
    VITAGL_STATS=0 VITAGL_UNSAFE=1 NO_SPLASHSCREEN=1 \
    VGL_GIT_HASH=vitaGL
mkdir -p "$root/.deps-softfp"
cp "$source_dir/libvitaGL.a" "$root/.deps-softfp/libvitaGL.a"
sha256sum "$root/.deps-softfp/libvitaGL.a"
