#!/usr/bin/env python3
"""Compare, function by function, the calls made by the original DOL (dtk asm)
with the calls made by the decompiled C as compiled for the port.

Decompiled functions often "match" or compile while having calls commented
out (e.g. NudxFw_BeginScene without GS_BeginScene). On the GameCube matching
build those functions are linked from assembly, so nobody notices; the native
port runs the C and silently loses behaviour. This report lists functions whose
C version is missing calls (or makes extra ones) relative to the original.

usage: audit_calls.py --asm build/GCBE7D/asm --objs <port objects...> [--only-missing]
"""
import argparse
import collections
import os
import re
import subprocess
import sys

IGNORE = {
    # helpers that the port compiler may inline/expand or that ProDG emitted
    "__save_gpr", "__restore_gpr", "_savegpr_14", "_restgpr_14", "__div2i", "__mod2i", "__div2u",
    "__mod2u", "__shl2i", "__shr2i", "__ashrdi3", "__lshrdi3", "__ashldi3", "__cvt_fp2unsigned",
    "memcpy", "memset", "__floatdisf", "__fixunssfsi", "__fixsfsi", "__floatsisf", "__cmpdi2",
    "__ucmpdi2", "__divdi3", "__udivdi3", "__moddi3", "__umoddi3", "__muldi3", "__negdi2",
    "sqrtf", "sqrt", "__sinit", "abort",
}


def asm_calls(asm_dir):
    calls = collections.defaultdict(collections.Counter)
    for fn in os.listdir(asm_dir):
        if not fn.endswith(".s"):
            continue
        cur = None
        for line in open(os.path.join(asm_dir, fn), errors="replace"):
            m = re.match(r"^\.fn (\S+?),", line)
            if m:
                cur = m.group(1)
                continue
            if line.startswith(".endfn"):
                cur = None
                continue
            if cur is None:
                continue
            m = re.search(r"\tbl (\S+)$", line.rstrip())
            if m:
                calls[cur][m.group(1)] += 1
    return calls


def obj_calls(objs):
    calls = collections.defaultdict(collections.Counter)
    for o in objs:
        out = subprocess.run(["objdump", "-dr", "--no-show-raw-insn", o], capture_output=True, text=True).stdout
        cur = None
        for line in out.splitlines():
            m = re.match(r"^[0-9a-f]+ <([^>]+)>:", line)
            if m:
                if not m.group(1).startswith("."):  # local labels (.L123) belong to the current function
                    cur = m.group(1)
                continue
            if cur is None:
                continue
            m = re.search(r"R_(?:386_PC32|386_PLT32|ARM_CALL|ARM_THM_CALL|ARM_JUMP24)\s+(\S+)", line)
            if m:
                tgt = m.group(1).split("-")[0]
                if tgt.startswith(".text."):
                    tgt = tgt[6:]  # static function referenced via its section (-ffunction-sections)
                if tgt.startswith(".") or tgt == cur:
                    continue
                calls[cur][tgt] += 1
    return calls


def demangle(name):
    # ProDG C++-ish names like CreateDirect3D__FP6HWND__ -> CreateDirect3D
    return re.sub(r"__F.*$", "", name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--asm", required=True)
    ap.add_argument("--only-missing", action="store_true")
    ap.add_argument("--filter", help="regex on function names")
    ap.add_argument("objs", nargs="+")
    a = ap.parse_args()
    A = asm_calls(a.asm)
    A2 = {demangle(k): v for k, v in A.items()}
    C = obj_calls(a.objs)
    rows = []
    for fn, cc in sorted(C.items()):
        if fn not in A2:
            continue
        if a.filter and not re.search(a.filter, fn):
            continue
        ac = collections.Counter({demangle(k): v for k, v in A2[fn].items()})
        missing = sorted(k for k in ac if k not in cc and k not in IGNORE)
        extra = sorted(k for k in cc if k not in ac and k not in IGNORE and not k.startswith("__"))
        if missing or (extra and not a.only_missing):
            rows.append((fn, missing, extra))
    for fn, missing, extra in rows:
        s = "%-40s missing: %s" % (fn, " ".join(missing) or "-")
        if not a.only_missing:
            s += " | extra: %s" % (" ".join(extra) or "-")
        print(s)
    print("functions with differences: %d" % len(rows), file=sys.stderr)


if __name__ == "__main__":
    main()
