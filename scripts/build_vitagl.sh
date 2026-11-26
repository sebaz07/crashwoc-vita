#!/usr/bin/env bash
# Build the vitaGL the port links: the commit of the VitaSDK package
# (vdpm vitaGL-0.0.0.r1488.g2bdbe89, built there with `make NO_DEBUG=1`)
# plus HAVE_SHADER_CACHE=1, which keeps every compiled shader on the memory
# card (with its varying bindings), so a shader is compiled once per install
# instead of once per session. patches/vitagl.patch keeps the cached shaders
# flat in v/ and f/ (vitaGL made 512 hash subfolders at the first start: 142 s
# on a memory card) and keeps the circular data pool 8-byte aligned (an odd
# reservation misaligned glBlitFramebuffer's vertices: a data abort). Output: build-vitagl/lib/libvitaGL.a and
# build-vitagl/include/vitaGL.h (scripts/build_vita.sh passes them to CMake).
#
# VITAGL_GIT: where to clone from (default: GitHub; a local clone works too).
# VITAGL_FLAGS: make flags (default: NO_DEBUG=1 HAVE_SHADER_CACHE=1).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
: "${VITASDK:?set VITASDK to your VitaSDK install}"
export PATH="$VITASDK/bin:$PATH"
# VITAGL_COMMIT / VITAGL_PATCH: another vitaGL commit and its patch (A/B
# against a newer upstream; patches/vitagl-head.patch fits 788c61d)
commit="${VITAGL_COMMIT:-2bdbe895ddc6a7e410a9ea55a8f53d8173c61ad8}"
repo="${VITAGL_GIT:-https://github.com/Rinnegatamante/vitaGL.git}"
flags="${VITAGL_FLAGS:-NO_DEBUG=1 HAVE_SHADER_CACHE=1}"
out="$here/build-vitagl"
src="$out/src"
stamp="$out/lib/.built"
patch="${VITAGL_PATCH:-$here/patches/vitagl.patch}"
want="$commit $flags $(sha256sum "$patch" | cut -c1-12)"
if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
  echo "vitaGL up to date ($want)"
  exit 0
fi
mkdir -p "$out/lib" "$out/include"
[ -d "$src/.git" ] || git clone -q "$repo" "$src"
git -C "$src" cat-file -e "$commit^{commit}" 2>/dev/null || git -C "$src" fetch -q "$repo" "+refs/*:refs/fetched/*"
git -C "$src" checkout -q -f --detach "$commit"
git -C "$src" apply "$patch"
make -C "$src" clean >/dev/null
# shellcheck disable=SC2086
make -C "$src" $flags -j"$(nproc)" >/dev/null
cp "$src/libvitaGL.a" "$out/lib/libvitaGL.a"
cp "$src/source/vitaGL.h" "$out/include/vitaGL.h"
echo "$want" > "$stamp"
echo "vitaGL built: $out/lib/libvitaGL.a ($want)"
