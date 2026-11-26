#!/usr/bin/env python3
"""Markdown tables from a bench_vita.py output directory (one row per
scenario, mean of the repetitions, with the rep-to-rep range of the fps).

Usage: scripts/bench_table.py OUTDIR [--compare OTHER_OUTDIR]
"""
import argparse
import json
import pathlib
import statistics as st

ORDER = ["hub", "snow_m", "castle", "jungle_r", "earth_r"]


def load(outdir):
    res = {}
    for f in sorted(pathlib.Path(outdir).glob("*_[0-9].json")):
        d = json.loads(f.read_text())
        if d.get("phase") != "completed" or not d.get("windows"):
            continue
        res.setdefault(d["scenario"], []).append(d)
    return res


def m(runs, key, sub="mean"):
    vals = [(r.get(key) or {}).get(sub) for r in runs]
    vals = [v for v in vals if v is not None]
    return st.mean(vals) if vals else None


def agent(runs, key, sub="mean"):
    vals = [((r.get("agent") or {}).get(key) or {}).get(sub) for r in runs]
    vals = [v for v in vals if v is not None]
    return st.mean(vals) if vals else None


def fmt(v, nd=1):
    return "—" if v is None else ("%.*f" % (nd, v))


def table(res, names):
    out = ["| escena | pasadas | fps (rango) | ms/frame | hilo juego: mtlrender / verts / logic / ticks | hilo render: ocupado / rt_xf | CPU0 / CPU1 % | tirones >100 ms |",
           "|---|---|---|---|---|---|---|---|"]
    for n in names:
        runs = res.get(n)
        if not runs:
            continue
        fps = [r["fps"]["mean"] for r in runs]
        hitches = sum((r.get("hitches") or {}).get("n", 0) for r in runs)
        out.append("| %s | %d | %s (%s–%s) | %s | %s / %s / %s / %s | %s / %s | %s / %s | %d |" % (
            n, len(runs), fmt(st.mean(fps)), fmt(min(fps)), fmt(max(fps)), fmt(m(runs, "ms_frame")),
            fmt(m(runs, "tb_mtlrender")), fmt(m(runs, "verts")), fmt(m(runs, "tb_logic")), fmt(m(runs, "tb_ticks"), 2),
            fmt(m(runs, "rt_busy")), fmt(m(runs, "rt_xf")), fmt(agent(runs, "CPU0"), 0), fmt(agent(runs, "CPU1"), 0),
            hitches))
    return "\n".join(out)


def compare(a, b, names):
    out = ["| escena | fps antes | fps después | Δ fps | ms/frame antes → después |", "|---|---|---|---|---|"]
    for n in names:
        if n not in a or n not in b:
            continue
        fa = st.mean(r["fps"]["mean"] for r in a[n])
        fb = st.mean(r["fps"]["mean"] for r in b[n])
        out.append("| %s | %.1f | %.1f | %+.1f (%+.0f%%) | %s → %s |" % (
            n, fa, fb, fb - fa, 100.0 * (fb - fa) / fa, fmt(m(a[n], "ms_frame")), fmt(m(b[n], "ms_frame"))))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--compare")
    a = ap.parse_args()
    res = load(a.outdir)
    names = [n for n in ORDER if n in res] + sorted(set(res) - set(ORDER))
    if a.compare:
        print(compare(load(a.compare), res, names))
    else:
        print(table(res, names))


if __name__ == "__main__":
    main()
