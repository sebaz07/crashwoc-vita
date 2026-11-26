#!/usr/bin/env python3
"""Shader programs and hitches in a crashwoc.log.

Splits the run at the last level load (a long frame that read files) and,
for the whole run and for the part after it (play), reports programs
compiled (and their compile + link ms), programs restored from the disk
cache, and long frames (CWOC_LONGFRAME ms or more) with how many of them
have a compile in the same 5 s window.

Usage: scripts/shader_hitches.py LOG [LOG ...]
"""
import re
import sys

LONG_RE = re.compile(r"^long frame (\d+): (\d+) ms \(file reads (\d+) ms, ([\d.]+) MB")
FRAME_RE = re.compile(r"^frame (\d+): ")
SH_RE = re.compile(r"^shaders: (\d+) compiled \(vs ([\d.]+) ms, fs ([\d.]+) ms, link\+lookups ([\d.]+) ms\), "
                   r"(\d+) from cache \(([\d.]+) ms\)")


def parse(path):
    longs, sh, frame = [], [], 0
    with open(path, errors="replace") as f:
        for line in f:
            m = FRAME_RE.match(line)
            if m:
                frame = int(m[1])
                continue
            m = LONG_RE.match(line)
            if m:
                longs.append({"frame": int(m[1]), "ms": int(m[2]), "io_ms": int(m[3]), "mb": float(m[4])})
                continue
            m = SH_RE.match(line)
            if m:
                # the line closes the 5 s window that ended at `frame`
                sh.append({"frame": frame, "n": int(m[1]), "ms": float(m[2]) + float(m[3]) + float(m[4]),
                           "hit": int(m[5]), "hit_ms": float(m[6])})
    return longs, sh


def summary(longs, sh, since):
    s = [x for x in sh if x["frame"] > since]
    lf = [l for l in longs if l["frame"] > since]
    with_compile = 0
    for l in lf:
        # the shaders line that follows the long frame covers it
        nxt = [x for x in sh if x["frame"] >= l["frame"]]
        if nxt and nxt[0]["n"] > 0:
            with_compile += 1
    return {"compiled": sum(x["n"] for x in s), "compile_ms": round(sum(x["ms"] for x in s)),
            "cache_hits": sum(x["hit"] for x in s), "cache_ms": round(sum(x["hit_ms"] for x in s)),
            "long_frames": len(lf), "long_ms": [l["ms"] for l in lf], "long_with_compile": with_compile}


def main():
    for path in sys.argv[1:]:
        longs, sh = parse(path)
        io = [l["frame"] for l in longs if l["mb"] >= 0.5 or l["io_ms"] >= 200]
        last_load = (max(io) + 3) if io else 0
        print(path)
        print("  whole run:", summary(longs, sh, -1))
        print("  play (after frame %d):" % last_load, summary(longs, sh, last_load))


if __name__ == "__main__":
    main()
