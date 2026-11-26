#!/usr/bin/env python3
"""Compare two P6 PPM frames (CWOC_SHOT_DIR captures of the same game frame).

Prints the share of differing pixels, the largest channel difference and the
bounding box of the differences; with --out writes a PPM of the differences
(white where they differ, the frame dimmed elsewhere).

Usage: scripts/ppm_diff.py A.ppm B.ppm [--out diff.ppm]
"""
import argparse


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    if parts[0] != b"P6" or parts[3] != b"255":
        raise SystemExit("%s: not an 8-bit P6 PPM" % path)
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--out")
    a = ap.parse_args()
    wa, ha, pa = read_ppm(a.a)
    wb, hb, pb = read_ppm(a.b)
    if (wa, ha) != (wb, hb):
        raise SystemExit("sizes differ: %dx%d vs %dx%d" % (wa, ha, wb, hb))
    n = wa * ha
    diff = 0
    worst = 0
    x0, y0, x1, y1 = wa, ha, -1, -1
    out = bytearray(len(pa)) if a.out else None
    for i in range(n):
        o = i * 3
        d = max(abs(pa[o] - pb[o]), abs(pa[o + 1] - pb[o + 1]), abs(pa[o + 2] - pb[o + 2]))
        if d:
            diff += 1
            worst = max(worst, d)
            x, y = i % wa, i // wa
            x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
        if out is not None:
            out[o:o + 3] = b"\xff\xff\xff" if d else bytes(v // 4 for v in pa[o:o + 3])
    print("%d of %d pixels differ (%.3f %%), largest difference %d%s" % (
        diff, n, 100.0 * diff / n, worst, ", box %d,%d-%d,%d" % (x0, y0, x1, y1) if diff else ""))
    if out is not None:
        with open(a.out, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (wa, ha) + bytes(out))


if __name__ == "__main__":
    main()
