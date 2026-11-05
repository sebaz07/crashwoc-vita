#!/usr/bin/env python3
"""Extract one function from dtk assembly and convert it to the syntax m2c
expects (glabel, plain instructions). usage: asm2m2c.py ASMFILE FUNC > out.s"""
import re, sys
path, fn = sys.argv[1], sys.argv[2]
out = []
inside = False
for line in open(path, errors="replace"):
    if re.match(r"^\.fn %s," % re.escape(fn), line):
        inside = True
        out.append(".text\nglabel %s\n" % fn)
        continue
    if inside and line.startswith(".endfn"):
        break
    if not inside:
        continue
    line = re.sub(r"^/\*.*?\*/\s*", "\t", line)
    if line.strip().startswith(".sym") or line.strip().startswith(".rel"):
        continue
    out.append(line if line.endswith("\n") else line + "\n")
sys.stdout.write("".join(out))
