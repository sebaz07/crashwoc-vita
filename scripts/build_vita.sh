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
cmake -S "$here/port" -B "$here/build-vita" -G Ninja -DPORT_VITA=ON \
  -DCWOC_DOL="$dol" -DCWOC_ASM_DIR="$here/decomp/build/GCBE7D/asm" "${extra[@]}"
ninja -C "$here/build-vita"
echo "VPK: $here/build-vita/crashwoc.vpk"
