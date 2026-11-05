#!/bin/sh
# Apply tools/unstatic.py to every "static in C" error in a ppc2c report.
# usage: fix_statics.sh build/port_recomp_report.txt
here=$(dirname "$0")
src="${CWOC_DECOMP_DIR:-$here/../../decomp}/src"; [ -d "$src" ] || src="$here/../../src"
grep "static in C" "$1" | sed -E 's/ERROR: (\S+) \((\S+)\.s\).*/\2 \1/' | sort | \
awk '{a[$1]=a[$1]" "$2} END {for (k in a) print k, a[k]}' | while read unit names; do
  f=$(ls "$src"/*/"$unit".c "$src"/*/*/"$unit".c 2>/dev/null | head -1)
  [ -n "$f" ] && python3 "$here/unstatic.py" "$f" $names
  h="${f%.c}.h"
  [ -f "$h" ] && python3 "$here/unstatic.py" "$h" $names
done
