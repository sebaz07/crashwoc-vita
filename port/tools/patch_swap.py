#!/usr/bin/env python3
"""Insert '#ifdef PORT port_swap32(ptr, n); #endif' after bulk read statements.

usage: patch_swap.py FILE 'regex-matching-the-read-call' [kind]
The regex must capture (ptr) and (size) groups; kind is 32 (default) or 16.
Idempotent: skips statements already followed by a PORT swap.
"""
import re
import sys

path, pattern = sys.argv[1], sys.argv[2]
kind = sys.argv[3] if len(sys.argv) > 3 else "32"
rx = re.compile(pattern)
lines = open(path).read().split("\n")
out = []
n = 0
i = 0
while i < len(lines):
    line = lines[i]
    out.append(line)
    m = rx.search(line)
    if m and not (i + 1 < len(lines) and lines[i + 1].strip() == "#ifdef PORT"):
        indent = re.match(r"\s*", line).group(0)
        ptr, size = m.group("ptr"), m.group("size")
        div = "4" if kind == "32" else "2"
        out.append("#ifdef PORT")
        out.append("%sport_swap%s(%s, (%s) / %s);" % (indent, kind, ptr, size, div))
        out.append("#endif")
        n += 1
    i += 1
open(path, "w").write("\n".join(out))
print("%s: %d swaps inserted" % (path, n))
