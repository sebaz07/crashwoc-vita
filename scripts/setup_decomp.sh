#!/usr/bin/env bash
# Fetch the community decompilation at the commit this port was made against
# and apply the port's changes (all under #ifdef PORT, so the GameCube matching
# build is unaffected). Result: ./decomp
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
url="${DECOMP_URL:-https://github.com/denzi-gh/crashwoc-decomp-gc.git}"
rev="$(cat "$here/patches/DECOMP_COMMIT")"
dst="$here/decomp"
if [ ! -d "$dst/.git" ]; then
  git -c core.autocrlf=false clone "$url" "$dst"
fi
cd "$dst"
git -c core.autocrlf=false checkout -q "$rev"
if git apply --check "$here/patches/decomp-port.patch" 2>/dev/null; then
  git apply "$here/patches/decomp-port.patch"
  echo "decomp ready at $dst (commit $rev + port patch)"
elif git apply -R --check "$here/patches/decomp-port.patch" 2>/dev/null; then
  echo "decomp at $dst already has the port patch"
else
  echo "the port patch does not apply to $dst: start from a clean clone (rm -rf decomp)" >&2
  exit 1
fi
