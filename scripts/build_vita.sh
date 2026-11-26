#!/usr/bin/env bash
# Build crashwoc.vpk. Needs: VitaSDK (VITASDK set), Python 3 + pyelftools,
# ninja, cmake, and your own GCBE7D main.dol at decomp/orig/GCBE7D/sys/main.dol
# (crashwoc-extract --dol <path> writes it from your disc image).
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
: "${VITASDK:?set VITASDK to your VitaSDK install}"
export PATH="$VITASDK/bin:$PATH"
dol="$here/decomp/orig/GCBE7D/sys/main.dol"
[ -d "$here/decomp/src" ] || "$here/scripts/setup_decomp.sh"
[ -f "$dol" ] || { echo "missing $dol (your own GCBE7D main.dol)" >&2; exit 1; }
# split the DOL into assembly (decomp-toolkit, downloaded by configure.py)
( cd "$here/decomp" && python3 configure.py && ninja build/GCBE7D/config.json )
extra=()
[ -n "${CWOC_LIVEAREA_DIR:-}" ] && extra+=("-DCWOC_LIVEAREA_DIR=$CWOC_LIVEAREA_DIR")
# shader seed packed into the VPK when there is one (scripts/shader_seed.py)
seed="${CWOC_SHADER_SEED:-$here/shader-seed}"
if [ -d "$seed" ]; then extra+=("-DCWOC_SHADER_SEED=$seed"); else extra+=("-DCWOC_SHADER_SEED="); fi
# vitaGL with the shader disk cache (scripts/build_vitagl.sh); CWOC_SDK_VITAGL=1
# links the VitaSDK package instead
if [ -n "${CWOC_SDK_VITAGL:-}" ]; then
  extra+=("-DPORT_VITAGL_DIR=")
else
  "$here/scripts/build_vitagl.sh"
  extra+=("-DPORT_VITAGL_DIR=$here/build-vitagl")
fi
# CWOC_GPUPROF=1: measurement build with the GPU profiler capture
# (port/src/port_gpuprof.c) in build-vita-gpuprof; GPUPROF_PREFIX: the install
# prefix of the psp2-cex-gpues4-prof client libraries
bdir="$here/build-vita"
if [ -n "${CWOC_GPUPROF:-}" ]; then
  bdir="$here/build-vita-gpuprof"
  extra+=("-DCWOC_GPUPROF=ON" "-DCMAKE_PREFIX_PATH=${GPUPROF_PREFIX:-$HOME/vita-dev/gpuprof-prefix}")
fi
cmake -S "$here/port" -B "$bdir" -G Ninja -DPORT_VITA=ON \
  -DCWOC_DOL="$dol" -DCWOC_ASM_DIR="$here/decomp/build/GCBE7D/asm" "${extra[@]}"
ninja -C "$bdir"
echo "VPK: $bdir/crashwoc.vpk"
