#!/usr/bin/env python3
"""Make file-local functions/variables global for the port only:
   'static <decl> NAME' -> '#ifndef PORT\\nstatic\\n#endif\\n<decl> NAME'
usage: unstatic.py FILE NAME [NAME...]"""
import re, sys
path, names = sys.argv[1], sys.argv[2:]
s = open(path, newline='').read()  # keep CRLF files intact
nl = '\r\n' if '\r\n' in s else '\n'
for n in names:
    rx = re.compile(r'^static\s+((?:inline\s+)?[^;{()=]*?\b' + re.escape(n) + r'\s*\()', re.M)
    cnt = 0
    def rep(m):
        global cnt
        cnt += 1
        return '#ifndef PORT' + nl + 'static' + nl + '#endif' + nl + m.group(1)
    s, k = rx.subn(lambda m: '#ifndef PORT' + nl + 'static' + nl + '#endif' + nl + m.group(1), s)
    if k == 0:  # file-scope variable
        rxv = re.compile(r'^static\s+((?:const\s+|volatile\s+)*[^;(){}=]*?\b' + re.escape(n) + r'\b\s*[\[=;,])', re.M)
        s, k = rxv.subn(lambda m: '#ifndef PORT' + nl + 'static' + nl + '#endif' + nl + m.group(1), s)
    print('%s: %s -> %d' % (path, n, k))
open(path, 'w', newline='').write(s)
