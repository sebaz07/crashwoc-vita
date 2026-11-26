#!/usr/bin/env python3
"""Build a shader seed on the PS Vita: GPU programs compiled ahead of time,
packed into the VPK (CMake CWOC_SHADER_SEED, default ./shader-seed) so that a
first start compiles nothing the seed has.

For every level (CWOC_WARP index, with Crash running, jumping and spinning:
bench_vita.py's lvlN_mv scenarios) the game runs once with its shader caches
in one fresh folder on the Vita and the shipped seed ignored (CWOC_SHSEED=0),
so every shader the level shows is compiled into that folder. A level that
fails (an index that is no real level) is noted and skipped. Then the folder
is downloaded into the seed, added to what is already there:

    shader-seed/vgl/v2/{v,f}/<hash>.gxp   vitaGL's shader cache (keyed by the
                                          shader's source: valid for any build
                                          with the same generator and vitaGL)
    shader-seed/prog/<hash>.bin           the port's program cache (keyed also
                                          by PORT_VITAGL_ID)

The seed is compiled output of the port's own shaders, not game data, but it
stays out of the repository (.gitignore) like every generated file.

Usage: scripts/shader_seed.py OUTDIR [--install VPK] [--levels hub,0-37,39-43]
       [--cachedir ux0:data/crashwoc/shc_seed] [--seed shader-seed]
       [--frames 2400] [--harvest-only]
Needs `serve` running (as bench_vita.py).
"""
import argparse
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bench_vita  # noqa: E402  (vau, the run helpers)


def parse_levels(spec):
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if part == "hub":
            out.append("hub_mv")
        elif "-" in part:
            a, b = part.split("-")
            out += ["lvl%d_mv" % i for i in range(int(a), int(b) + 1)]
        else:
            out.append("lvl%d_mv" % int(part))
    return out


def remote_files(path):
    r = bench_vita.vau("fs", "list", path)
    res = r.get("result") or {}
    return [e["name"] for e in res.get("entries", []) if e.get("kind") == "file"]


def harvest(cachedir, seed):
    """Download the cache folder's shaders and programs that the seed lacks."""
    got = {}
    for sub, dst, ext in (("vgl/v2/v", "vgl/v2/v", ".gxp"), ("vgl/v2/f", "vgl/v2/f", ".gxp"), ("", "prog", ".bin")):
        src = cachedir + ("/" + sub if sub else "")
        d = seed / dst
        d.mkdir(parents=True, exist_ok=True)
        names = [n for n in remote_files(src) if n.endswith(ext)]
        new = 0
        for n in names:
            if (d / n).exists():
                continue
            bench_vita.vau("fs", "download", src + "/" + n, str(d / n))
            new += 1
        got[dst] = (len(names), new)
        print("%-10s %4d on the Vita, %4d new, %4d in the seed" % (dst, len(names), new, len(list(d.glob("*" + ext)))),
              flush=True)
    return got


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--install")
    ap.add_argument("--levels", default="hub,0-37,39-43")
    ap.add_argument("--cachedir", default="ux0:data/crashwoc/shc_seed")
    ap.add_argument("--seed", default=str(HERE.parent / "shader-seed"))
    ap.add_argument("--frames", type=int, default=2400)
    ap.add_argument("--harvest-only", action="store_true")
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    seed = pathlib.Path(a.seed)
    report = {"levels": {}}
    if not a.harvest_only:
        install = a.install
        env = ["CWOC_SHCACHEDIR=" + a.cachedir, "CWOC_SHSEED=0"]
        for name in parse_levels(a.levels):
            try:
                res = bench_vita.run_one(out, name, bench_vita.scenario_level(name), 1, a.frames, install, env)
                phase = res.get("phase")
            except Exception as e:  # a level that hangs or a transfer error: note it and go on
                phase = "error: %s" % e
            install = None
            report["levels"][name] = phase
            print("%-10s %s" % (name, phase), flush=True)
            if phase != "completed":
                # a level that hung (CWOC_WARP 38, the game over room, hangs in
                # its load when warped to) keeps the game running: close it,
                # or every later run and install fails
                try:
                    bench_vita.vau("app", "close", "CRWC00001")
                except Exception as e:
                    print("could not close the game: %s" % e, flush=True)
        off = out / "env_off.txt"
        off.write_text("# Opciones de prueba del port (KEY=VALUE por linea). Vacio = juego normal.\n"
                       "# Lo reescribe scripts/bench_vita.py para las mediciones.\n")
        bench_vita.upload_env(off, out)
    report["harvest"] = harvest(a.cachedir, seed)
    (out / "shader_seed.json").write_text(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
