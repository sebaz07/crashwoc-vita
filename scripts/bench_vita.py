#!/usr/bin/env python3
"""Unattended benchmark runs of the port on a PS Vita through vita-agent-use.

For every scenario and repetition: write ux0:data/crashwoc/env.txt, start a
vita-agent-use run (the first one can install the VPK), wait until the game
prints "reached --frames" and exits, download its log and summarise it with
bench_summary.py (fps, frame-time breakdown, hitches, CPU per core, memory).

Needs `serve` running. Paths: VAU_DIR (vita-agent-use checkout), VAU_PYTHON
(Python with `cryptography`), VITA_AGENT_CONFIG_DIR.

Usage: scripts/bench_vita.py OUTDIR [--install build-vita/crashwoc.vpk]
       [--reps 3] [--only hub,snow_m] [--frames 3200] [--env KEY=VALUE ...]
       [--gpuprof FIRST,FRAMES]

--gpuprof (a VPK from CWOC_GPUPROF=1 scripts/build_vita.sh): records the GPU
work of FRAMES presented frames from FIRST on, downloads capture.csv,
draws.csv and shaders.txt as <scenario>_<rep>.gpuprof.* and writes the
report of scripts/gpuprof_report.py next to them.
"""
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
VAU_DIR = pathlib.Path(os.environ.get("VAU_DIR", pathlib.Path.home() / "vita-dev/third_party/vita-agent-use"))
VAU_PY = os.environ.get("VAU_PYTHON", str(pathlib.Path.home() / ".local/share/vita-devtools/bin/python3"))
CFG = pathlib.Path(os.environ.setdefault("VITA_AGENT_CONFIG_DIR", str(pathlib.Path.home() / "vita-dev/vita-agent-config")))
EVENTS = CFG / "pc/server/events.jsonl"
LOG = "ux0:data/crashwoc/crashwoc.log"
ENV = "ux0:data/crashwoc/env.txt"
GPUPROF = "ux0:data/crashwoc/gpuprof"

# name -> level for CWOC_WARP (None: stay in the hub, where CWOC_INTRO=0 starts)
SCENARIOS = {
    "hub": None,
    "snow_m": 7,
    "castle": 10,
    "jungle_r": 8,
    "earth_r": 4,
}
# with a fresh save the first level entered from the hub opens with a story
# scene in the vortex (cut.c UpdateGameCut) that waits for A/START
SKIP_INPUT = "A@420-425,A@520-525,A@620-625"
# "_mv" scenarios: Crash runs, jumps and spins (A jump, B spin; frames are
# presented frames). Lighting and effects change as he moves, which the
# standing scenarios never show: use them for hitches and shader counts,
# not for fps.
MOVE_INPUT = ("SUP@760-1300,A@820-826,B@950-956,A@1100-1106,SRIGHT@1300-1550,SUP@1550-2200,B@1650-1656,"
              "A@1800-1806,B@1950-1956,SLEFT@2200-2450,SUP@2450-3000,A@2500-2506,B@2650-2656,A@2800-2806")
SCENARIOS.update({"jungle_mv": 8, "earth_mv": 4, "hub_mv": None})


def scenario_level(name):
    """Level of a scenario: the table above, or lvlN / lvlN_mv for any level
    index N (CWOC_WARP; scripts/shader_seed.py walks them all)."""
    if name in SCENARIOS:
        return SCENARIOS[name]
    m = re.fullmatch(r"lvl(\d+)(_mv)?", name)
    if not m:
        raise SystemExit("unknown scenario %s" % name)
    return int(m.group(1))


def vau(*args, timeout=600):
    p = subprocess.run([VAU_PY, "client/vita_agent.py", *args], cwd=VAU_DIR, capture_output=True, text=True,
                       timeout=timeout)
    lines = [l for l in p.stdout.splitlines() if l.strip().startswith("{")]
    if not lines:
        raise RuntimeError("vita-agent-use %s: no JSON output: %s" % (args[0], (p.stdout + p.stderr)[-400:]))
    return json.loads(lines[-1])


def upload_env(src, out):
    """Upload src as env.txt and read it back. Each upload gets its own
    transfer state: vita-agent-use treats a finished state for the same
    source and bytes as already done (it only re-checks the old operation),
    so re-sending an identical env.txt after another one replaced it would
    silently leave the other one on the Vita."""
    xfer = out / ".xfer"
    xfer.mkdir(exist_ok=True)
    state = xfer / ("%s-%d.json" % (pathlib.Path(src).stem, time.time_ns()))
    r = vau("fs", "upload", str(src), ENV, "--overwrite", "--yes", "--transfer-state", str(state))
    if r.get("verified") is not True and (r.get("result") or {}).get("verified") is not True:
        raise RuntimeError("env.txt upload not verified: %s" % json.dumps(r)[-300:])
    back = xfer / (state.stem + ".readback.txt")
    vau("fs", "download", ENV, str(back))
    if back.read_bytes() != pathlib.Path(src).read_bytes():
        raise RuntimeError("env.txt on the Vita does not match %s" % src)


def env_text(name, level, frames, extra=()):
    lines = ["# benchmark run (written by scripts/bench_vita.py); delete this file for normal play",
             "CWOC_SAVE=ux0:data/crashwoc/save_bench", "CWOC_SKIP_LOGOS=1", "CWOC_INTRO=0",
             "CWOC_KEEPAWAKE=1", "CWOC_LONGFRAME=100", "CWOC_PADOFF=1", "CWOC_FRAMES=%d" % frames]
    inp = []
    if level is not None:
        lines += ["CWOC_WARP=%d@200" % level]
        inp.append(SKIP_INPUT)
    if name.endswith("_mv"):
        inp.append(MOVE_INPUT)
    if inp:
        lines += ["CWOC_INPUT=" + ",".join(inp)]
    lines += list(extra)
    return "\n".join(lines) + "\n"


def fetch_gpuprof(out, tag, logf):
    """Download a GPU profiler capture of this run (only when its log says the
    window closed: the files of an earlier run may still be on the Vita)."""
    text = logf.read_text(errors="replace") if logf.exists() else ""
    if "gpuprof: window closed" not in text:
        print("%s: no GPU profiler capture in the log" % tag, flush=True)
        return
    for f in ("capture.csv", "draws.csv", "shaders.txt"):
        vau("fs", "download", "%s/%s" % (GPUPROF, f), str(out / ("%s.gpuprof.%s" % (tag, f))), timeout=1800)
    rep = subprocess.run([sys.executable, str(HERE / "gpuprof_report.py"), str(out / (tag + ".gpuprof"))],
                         capture_output=True, text=True)
    (out / (tag + ".gpuprof.md")).write_text(rep.stdout + rep.stderr)
    print("%s: GPU profiler report in %s" % (tag, out / (tag + ".gpuprof.md")), flush=True)


def run_one(out, name, level, rep, frames, install, extra=(), captures=(), gpuprof=False):
    tag = "%s_%d" % (name, rep)
    envf = out / (tag + ".env.txt")
    envf.write_text(env_text(name, level, frames, extra))
    upload_env(envf, out)
    spec = {"title_id": "CRWC00001", "timeout_s": 600, "performance_interval_ms": 1000,
            "close_on_completion": False,
            "completion": {"log": {"path": LOG, "literal": "reached --frames"}}}
    if install:
        spec["prepare"] = {"install": {"source": str(pathlib.Path(install).resolve())}}
    specf = out / (tag + ".run.json")
    specf.write_text(json.dumps(spec, indent=1))
    r = vau("run", "start", str(specf), *(["--yes"] if install else []))
    if "result" not in r:
        raise RuntimeError("vita-agent-use run start refused: %s" % json.dumps(r)[-600:])
    run_id = r["result"]["run_id"]
    t0 = time.time()
    pending = sorted(captures)
    while True:
        time.sleep(1 if pending else 5)
        while pending and time.time() - t0 >= pending[0]:
            # screen capture at a fixed time after the run started (A/B checks)
            vau("screen", "capture", "--output", str(out / ("%s_t%03d.jpg" % (tag, pending.pop(0)))))
        st = vau("run", "status")["result"]
        if st.get("phase") in ("completed", "finished", "failed", "cancelled"):
            break
    dt = time.time() - t0
    logf = out / (tag + ".log")
    vau("fs", "download", LOG, str(logf))
    if gpuprof:
        fetch_gpuprof(out, tag, logf)
    summ = subprocess.run([sys.executable, str(HERE / "bench_summary.py"), str(logf), "--events", str(EVENTS),
                           "--run", run_id, "--json", str(out / (tag + ".json"))], capture_output=True, text=True)
    res = json.loads(summ.stdout) if summ.stdout.strip().startswith("{") else {}
    res.update({"scenario": name, "rep": rep, "run_id": run_id, "phase": st.get("phase"),
                "error": st.get("error"), "wall_s": round(dt)})
    (out / (tag + ".json")).write_text(json.dumps(res, indent=1))
    fps = (res.get("fps") or {}).get("mean")
    ms = (res.get("ms_frame") or {}).get("mean")
    print("%-10s rep %d: %s (%s) fps %s ms/frame %s windows %s hitches %s  [%ds]" % (
        name, rep, st.get("phase"), st.get("error") or "ok", fps, ms, res.get("windows"),
        (res.get("hitches") or {}).get("n"), dt), flush=True)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--install")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--only")
    ap.add_argument("--frames", type=int, default=3200)
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for env.txt")
    ap.add_argument("--capture", default="", help="screen captures at these seconds after each run starts, e.g. 110,130")
    ap.add_argument("--gpuprof", help="FIRST,FRAMES: GPU profiler capture (CWOC_GPUPROF=1 build)")
    a = ap.parse_args()
    if a.gpuprof:
        a.env.append("CWOC_GPUPROF=" + a.gpuprof)
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    names = a.only.split(",") if a.only else list(SCENARIOS)
    install = a.install
    try:
        for rep in range(1, a.reps + 1):
            for name in names:
                res = run_one(out, name, scenario_level(name), rep, a.frames, install, a.env,
                              [int(x) for x in a.capture.split(",") if x], bool(a.gpuprof))
                install = None
                if res.get("phase") != "completed":
                    print("stopping: run did not complete (locked screen, crash or dialog?)", flush=True)
                    return 1
    finally:
        # leave a comment-only env.txt so the game starts normally afterwards
        off = out / "env_off.txt"
        off.write_text("# Opciones de prueba del port (KEY=VALUE por linea). Vacio = juego normal.\n"
                       "# Lo reescribe scripts/bench_vita.py para las mediciones.\n")
        upload_env(off, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
