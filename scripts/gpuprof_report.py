#!/usr/bin/env python3
"""Report of a GPU profiler capture of the port (CWOC_GPUPROF build).

Reads PREFIX.capture.csv with psp2-cex-gpues4-prof's tools/analyze.py (as a
module: the same model and checks), joins every timed draw with its row of
PREFIX.draws.csv (draw names are "#<row id>") and ranks the sampled fragment
time per frame by pass, game shader, blending, depth state, draw kind and
texture size, then the most expensive draws. Appends analyze.py's own text
report (pipeline activity: vertex/fragment instruction activity, texture and
memory stalls).

Usage: scripts/gpuprof_report.py PREFIX    (e.g. bench/castle_1.gpuprof)
GPUPROF_SRC: the psp2-cex-gpues4-prof checkout (default: the one inside
~/vita-dev/third_party/vita-agent-use/agent/tools).
"""
import collections
import csv
import importlib.util
import os
import pathlib
import sys

SRC = pathlib.Path(os.environ.get("GPUPROF_SRC", pathlib.Path.home() /
                                  "vita-dev/third_party/vita-agent-use/agent/tools/psp2-cex-gpues4-prof"))
TOP = ["rest", "opaque", "sorted", "other"]
ITEM = ["", ".geom", ".skin", ".face"]
BLEND = {0: "none", 1: "blend", 2: "logic", 3: "subtract"}
FACTOR = ["0", "1", "srcclr", "1-srcclr", "srca", "1-srca", "dsta", "1-dsta"]


def load_analyzer():
    spec = importlib.util.spec_from_file_location("gpuprof_analyze", SRC / "tools/analyze.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def tag_name(tag):
    return "%s %s%s%s" % ("hud" if tag & 16 else "3d", TOP[(tag >> 2) & 3], ITEM[tag & 3], " fx" if tag & 32 else "")


def table(title, rows, frames, total, limit=None):
    out = ["", "### " + title, "", "| | ms/frame | % | draws/frame | timed |", "|---|---:|---:|---:|---:|"]
    rows = sorted(rows.items(), key=lambda kv: -kv[1][0])
    for name, (ms, n, timed) in rows[:limit]:
        out.append("| %s | %.2f | %.1f | %.1f | %d/%d |" % (name, ms / frames, 100 * ms / total if total else 0,
                                                          n / frames, timed, n))
    if limit and len(rows) > limit:
        rest = rows[limit:]
        out.append("| (%d more) | %.2f | | | |" % (len(rest), sum(r[1][0] for r in rest) / frames))
    return out


def whole_window(an, rep, frames):
    """Signal assertion over every sample of the window (the sampler only runs
    inside it), not only inside matched fragment-draw windows as analyze.py
    reports them: vertex work and idle time included. Rate x window time per
    frame = an estimate of the ms per frame each signal is asserted (any of
    the 4 cores, averaged over cores)."""
    samples = [s for s in rep["diagnostics"] if not s["result"]]
    if not samples or frames < 1:
        return []
    span = (max(s["after_us"] for s in samples) - min(s["before_us"] for s in samples)) / 1000.0
    lookup = collections.defaultdict(list)
    for metric, _cat, name, selectors in an.PIPELINE_METRICS:
        for group, bit in selectors:
            lookup[group].append((name, bit))
    seen, hit = collections.Counter(), collections.Counter()
    for s in samples:
        for name, bit in lookup.get(s["group"], ()):
            for c in s["cores"]:
                seen[name] += 1
                hit[name] += bool(c["value"] & (1 << bit))
    out = ["", "### Whole window (all samples, %.1f ms/frame of wall time)" % (span / frames), "",
           "| signal | rate | ≈ ms/frame | samples |", "|---|---:|---:|---:|"]
    for _m, _cat, name, _sel in an.PIPELINE_METRICS:
        if seen[name]:
            r = hit[name] / seen[name]
            out.append("| %s | %.1f %% | %.1f | %d |" % (name, 100 * r, r * span / frames, seen[name]))
    return out


def main():
    prefix = sys.argv[1]
    an = load_analyzer()
    with open(prefix + ".capture.csv", encoding="utf-8", errors="replace", newline="") as f:
        events, dropped = an.read_capture(f)
    rep = an.summarize(events, dropped)
    timing = rep["sampled_fragment_timing"]
    rows = {}
    with open(prefix + ".draws.csv", newline="") as f:
        for r in csv.DictReader(f):
            rows[int(r["id"])] = {k: int(v) for k, v in r.items()}
    frames = max((r["frame"] for r in rows.values()), default=0) + 1
    out = ["# GPU profiler capture: %s" % pathlib.Path(prefix).name, ""]
    # scenes since vglInit are recorded for the identity counters; count the window's
    out.append("- %d events (%d dropped), %d GXM draws, %d scenes in the window, %d transfers, %d samples, %d frames" % (
        rep["events"], rep["dropped"], len(rep["draws"]), sum(1 for sc in rep["scenes"] if sc["frame"] is not None),
        len(rep["transfers"]),
        len(rep["diagnostics"]), frames))
    if not timing["available"]:
        out.append("- **no timing**: %s" % timing["reason"])
    warn = rep["warnings"]
    if warn:
        out.append("- warnings: " + "; ".join("%s ×%d" % kv for kv in sorted(warn.items())))
    groups = {k: collections.defaultdict(lambda: [0.0, 0, 0]) for k in
              ("pass", "shader", "blend", "depth", "kind", "tex", "named", "prims")}
    perdraw = []
    total = 0.0
    for d in rep["draws"]:
        ms = d.get("sampled_fragment_ms")
        name = d.get("name") or ""
        r = rows.get(int(name[1:])) if name.startswith("#") and name[1:].isdigit() else None
        keys = {}
        if r:
            keys["pass"] = tag_name(r["tag"])
            keys["shader"] = "s%03d" % r["shader"]
            keys["blend"] = BLEND.get(r["blend"], "?") + (
                "(%s,%s)" % (FACTOR[r["bsrc"] & 7], FACTOR[r["bdst"] & 7]) if r["blend"] == 1 else "")
            keys["depth"] = ("z" + ("test" if r["zen"] else "off") + ("+write" if r["zen"] and r["zup"] else "") +
                             (" atest" if r["aref0"] or r["aref1"] else ""))
            keys["kind"] = "static" if r["kind"] == 2 else "dynamic"
            big = max(r["tw"], r["th"])
            keys["tex"] = "none" if not r["ntev"] or not big else "≤64" if big <= 64 else "≤128" if big <= 128 else \
                "≤256" if big <= 256 else ">256"
            prims = r["ni"] // 3 if r["ni"] else r["nv"] // 3
            keys["prims"] = "<16" if prims < 16 else "<128" if prims < 128 else "<1024" if prims < 1024 else "≥1024"
        else:
            keys["pass"] = (d.get("pass") or "unlabelled")
            keys["named"] = name or "(unnamed)"
        for k, v in keys.items():
            g = groups[k][v]
            g[1] += 1
            if ms is not None:
                g[0] += ms
                g[2] += 1
        if ms is not None:
            total += ms
            perdraw.append((ms, name, r))
    out.append("- attributed fragment time: **%.2f ms/frame** (%d of %d draws timed)" % (
        total / max(frames, 1), sum(1 for d in rep["draws"] if d.get("sampled_fragment_ms") is not None),
        len(rep["draws"])))
    cov = timing.get("unassigned_gap_ms") or {}
    if cov:
        out.append("- unassigned sampling gaps (ms, whole capture): " +
                   ", ".join("%s %.1f" % kv for kv in sorted(cov.items(), key=lambda kv: -kv[1])))
    if timing.get("observation_rejections"):
        out.append("- rejected core observations: " + ", ".join(
            "%s %d" % kv for kv in sorted(timing["observation_rejections"].items(), key=lambda kv: -kv[1])))
    fr = max(frames, 1)
    out += table("Passes", groups["pass"], fr, total)
    out += table("Game shaders", groups["shader"], fr, total, 25)
    out += table("Blending", groups["blend"], fr, total)
    out += table("Depth / alpha test", groups["depth"], fr, total)
    out += table("Draw kind", groups["kind"], fr, total)
    out += table("Texture size (stage 0)", groups["tex"], fr, total)
    out += table("Triangles per draw", groups["prims"], fr, total)
    if groups["named"]:
        out += table("Other GPU work (clears, copies, vitaGL)", groups["named"], fr, total)
    out += ["", "### Most expensive draws (whole capture)", "",
            "| draw | frame | pass | shader | kind | tris | blend | depth | tex | ms |",
            "|---|---:|---|---|---|---:|---|---|---|---:|"]
    for ms, name, r in sorted(perdraw, key=lambda x: -x[0])[:30]:
        if r:
            out.append("| %s | %d | %s | s%03d | %s | %d | %s | z%d%d f%d | %dx%d/%d | %.3f |" % (
                name, r["frame"], tag_name(r["tag"]), r["shader"], "static" if r["kind"] == 2 else "dyn",
                (r["ni"] or r["nv"]) // 3, BLEND.get(r["blend"], "?") +
                ("(%s,%s)" % (FACTOR[r["bsrc"] & 7], FACTOR[r["bdst"] & 7]) if r["blend"] == 1 else ""),
                r["zen"], r["zup"], r["zfn"], r["tw"], r["th"], r["ntev"], ms))
        else:
            out.append("| %s | | | | | | | | | %.3f |" % (name or "(unnamed)", ms))
    out += whole_window(an, rep, frames)
    out += ["", "## analyze.py", "", "```", an.format_report(rep), "```"]
    print("\n".join(out))


if __name__ == "__main__":
    main()
