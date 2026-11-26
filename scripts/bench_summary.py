#!/usr/bin/env python3
"""Summarise one benchmark run of the Vita port.

Inputs:
  - the game's log (ux0:data/crashwoc/crashwoc.log, downloaded): every 5 s it
    prints "frame N: fps ..." plus the "prof ms/frame" and "tb ms/frame"
    breakdowns (port/src/port_video.c);
  - optionally the vita-agent-use event journal and the run id, for the
    per-second performance samples (fps from framebuffer submissions,
    system-wide CPU0-3, memory).

Only steady windows are kept: those that start after the last level load
("long frame" lines) and are not the first window after it.

Usage: scripts/bench_summary.py LOG [--events events.jsonl --run RUN_ID] [--json OUT]
"""
import argparse
import json
import re
import statistics as st
import sys

FRAME_RE = re.compile(r"^frame (\d+): ([\d.]+) fps, (\d+) GX blocks/frame, (\d+) GL draws/frame, (\d+) verts/frame")
LONG_RE = re.compile(r"^long frame (\d+): (\d+) ms \(file reads (\d+) ms, ([\d.]+) MB, (\d+) opens(?: (\d+) ms)?\)")
PROF_HEAD_RE = re.compile(r"total ([\d.]+) game ([\d.]+)")
PROF_ITEM_RE = re.compile(r"(\w+) ([\d.]+)\((\d+)\)")
RT_BUSY_RE = re.compile(r"render (?:thread|inline) busy ([\d.]+):")
TB_ITEM_RE = re.compile(r"(\w+) ([\d.]+)")


def parse_log(path):
    windows, loads, env = [], [], []
    cur = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("env: "):
                env.append(line[5:].split(" (")[0])
            m = LONG_RE.match(line)
            if m:
                loads.append({"frame": int(m[1]), "ms": int(m[2]), "io_ms": int(m[3]), "mb": float(m[4])})
                continue
            m = FRAME_RE.match(line)
            if m:
                cur = {"frame": int(m[1]), "fps": float(m[2]), "blocks": int(m[3]), "draws": int(m[4]),
                       "verts": int(m[5]), "prof": {}, "tb": {}}
                windows.append(cur)
                continue
            if cur is None:
                continue
            if line.startswith("prof ms/frame:"):
                m = PROF_HEAD_RE.search(line)
                cur["prof"]["total"], cur["prof"]["game"] = float(m[1]), float(m[2])
                m = RT_BUSY_RE.search(line)
                if m:
                    cur["prof"]["rt_busy"] = float(m[1])
                for name, ms, _n in PROF_ITEM_RE.findall(line):
                    cur["prof"][name] = float(ms)
            elif line.startswith("tb ms/frame:"):
                body = line.split("|")
                for part in body[1:3]:
                    for name, ms in TB_ITEM_RE.findall(part):
                        cur["tb"][name] = float(ms)
                m = re.search(r"ticks ([\d.]+)", line)
                if m:
                    cur["tb"]["ticks"] = float(m[1])
    return windows, loads, env


def last_load_frame(loads):
    """Frame of the last level load: a long frame that read files (plus the
    couple of slow frames that follow it)."""
    io = [l["frame"] for l in loads if l["mb"] >= 0.5 or l["io_ms"] >= 200]
    return (max(io) + 3) if io else 0


def steady(windows, loads):
    last_load = last_load_frame(loads)
    prev, out = 0, []
    for w in windows:
        if prev > last_load and w["prof"]:
            out.append(w)
        prev = w["frame"]
    return out[1:] if len(out) > 1 else out


def hitches(loads, windows):
    """Long frames (CWOC_LONGFRAME ms or more) inside the steady part."""
    if not windows:
        return []
    start = windows[0]["frame"]
    return [l for l in loads if start - 300 < l["frame"] <= windows[-1]["frame"] and l["frame"] > last_load_frame(loads)]


def agg(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return {"mean": round(st.mean(values), 2), "min": round(min(values), 2), "max": round(max(values), 2)}


def samples_for_run(events, run_id):
    out = []
    with open(events, encoding="utf-8") as f:
        for line in f:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            if e.get("type") == "performance.sample" and e.get("run_id") == run_id:
                out.append(e["sample"])
    return out


def summarise_samples(samples, seconds):
    game = [s for s in samples if (s.get("foreground") or {}).get("title_id") == "CRWC00001"]
    game = game[:-1]  # the last window spans the exit
    tail = game[-seconds:] if seconds and len(game) > seconds else game
    if not tail:
        return None
    res = {"n": len(tail), "fps": agg([s["fps"]["value"] for s in tail if s.get("fps", {}).get("error_code") == 0])}
    for core in ("CPU0", "CPU1", "CPU2", "CPU3"):
        res[core] = agg([s["cpu"]["percent"].get(core) for s in tail if s.get("cpu", {}).get("error_code") == 0])
    mem = {}
    for s in tail:
        for p in (s.get("memory") or {}).get("partitions", []):
            if p["total_bytes"]:
                mem.setdefault(p["name"], []).append(p["used_bytes"] / 1048576)
    res["mem_used_mb"] = {k: round(max(v), 1) for k, v in mem.items()}
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--events")
    ap.add_argument("--run")
    ap.add_argument("--json")
    a = ap.parse_args()
    windows, loads, env = parse_log(a.log)
    sw = steady(windows, loads)
    res = {"env": env, "load": [l for l in loads if l["frame"] <= last_load_frame(loads)][-4:], "windows": len(sw)}
    if sw:
        h = hitches(loads, sw)
        res["hitches"] = {"n": len(h), "ms": [l["ms"] for l in h]}
        res["fps"] = agg([w["fps"] for w in sw])
        res["ms_frame"] = agg([w["prof"].get("total") for w in sw])
        for k in ("game", "rt_busy", "verts", "rt_xf", "rt_draw", "rt_swap", "wait", "texhash", "shader"):
            res[k] = agg([w["prof"].get(k) for w in sw])
        for k in ("ticks", "logic", "chars", "scene", "world", "fx", "panel", "mtlrender", "deb", "explo"):
            res["tb_" + k] = agg([w["tb"].get(k) for w in sw])
        for k in ("draws", "blocks", "verts"):
            res["n_" + k] = agg([w[k] for w in sw])
        seconds = int(sum(5 for _ in sw))
    else:
        seconds = 0
    if a.events and a.run:
        res["agent"] = summarise_samples(samples_for_run(a.events, a.run), seconds)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)
    json.dump(res, sys.stdout, indent=1)
    print()
    return 0 if sw else 1


if __name__ == "__main__":
    sys.exit(main())
