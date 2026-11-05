#!/usr/bin/env python3
"""Mechanical PowerPC -> C translator for selected functions of the original DOL.

Why: many decompiled functions are not matching (permuter output, commented-out
code, missing asm-only functions). On the GameCube build those are linked from
assembly so nobody notices, but the native port runs the C. For functions whose
C is unreliable, this tool translates the dtk assembly instruction by
instruction into C that runs natively, so behaviour is exactly the original.

Model:
  * Memory is the native port's memory (native layout, native endianness).
    Pointers are 32-bit on both the PC (-m32) and Vita targets, so PPC registers
    hold host addresses directly. Typed loads/stores of the same width as the C
    field are endian-agnostic. (Known limits: accessing one field with a
    different width than C declared it, and C bitfields, whose bit order
    differs between big- and little-endian ABIs.)
  * Registers are C locals; calls between translated functions pass arguments
    through the global `ppc` context following the PPC SysV ABI. Calls to
    native (C) functions are emitted as typed C calls built from the DWARF of
    the port objects; native callers reach translated functions through
    generated wrappers with the original name.
  * The stack is an emulated area (r1). Doubles on the stack keep the PPC
    word order (needed for the int<->float conversion idioms).
  * Read-only constants (.sdata2/.rodata) loaded directly are inlined from the
    user's main.dol.

usage: ppc2c.py --asm DIR --dol main.dol --list recomp.txt --out port_recomp.c
                --wanted recomp_wanted.txt objs...
"""
import argparse
import os
import bisect
import re
import struct
import subprocess
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_dol_data as gdd  # noqa: E402
import ppc_types  # noqa: E402
from elftools.elf.elffile import ELFFile  # noqa: E402
import elf_compat  # noqa: E402,F401  (tolerate unknown relocation types)

INSN = re.compile(r"^/\* ([0-9A-F]{8}) [0-9A-F]{8}  (?:[0-9A-F]{2} ){4}\*/\t(\S+)\s*(.*)$")
LABEL = re.compile(r"^\.L_([0-9A-F]{8}):")
RO_SECTIONS = {"rodata", "sdata2"}


class Func:
    def __init__(self, name, scope, file):
        self.name, self.scope, self.file = name, scope, file
        self.insns = []  # (addr, mnem, ops)
        self.labels = set()
        self.addr = None


def parse_funcs(asm_dir):
    funcs = {}           # (file, name) -> Func
    globals_ = {}        # name -> Func
    by_file = defaultdict(dict)
    for fn in sorted(os.listdir(asm_dir)):
        if not fn.endswith(".s"):
            continue
        cur = None
        for line in open(os.path.join(asm_dir, fn), encoding="utf-8", errors="replace"):
            line = line.rstrip("\n")
            if line.startswith(".fn "):
                parts = line[4:].split(",")
                cur = Func(parts[0].strip(), parts[1].strip() if len(parts) > 1 else "global", fn)
                continue
            if line.startswith(".endfn"):
                if cur is not None:
                    funcs[(fn, cur.name)] = cur
                    by_file[fn][cur.name] = cur
                    if cur.scope in ("global", "weak"):
                        globals_.setdefault(cur.name, cur)
                cur = None
                continue
            if cur is None:
                continue
            m = LABEL.match(line)
            if m:
                cur.labels.add(int(m.group(1), 16))
                continue
            m = INSN.match(line)
            if m:
                addr = int(m.group(1), 16)
                if cur.addr is None:
                    cur.addr = addr
                ops = m.group(3).split("#")[0].strip()
                cur.insns.append((addr, m.group(2), ops))
    return funcs, globals_, by_file


# ------------------------------------------------------------------ DWARF


class Proto:
    __slots__ = ("name", "ret", "params", "variadic", "prototyped")

    def __init__(self):
        self.params = []
        self.variadic = False
        self.prototyped = True
        self.ret = None


def _strip(die):
    while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type",
                                          "DW_TAG_restrict_type"):
        die = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
    return die


def _hfa(t):
    """count of float members if t is a homogeneous float aggregate (<=4), else 0"""
    try:
        size, leaves = gdd.TypeDB._layout(gdd.TypeDB.__new__(gdd.TypeDB), t, 0)
    except Exception:
        return 0
    if not leaves or any(k != "float" or s != 4 for _, s, k, _ in leaves):
        return 0
    return len(leaves) if len(leaves) <= 4 and size == 4 * len(leaves) else 0


def kind_of(die):
    t = _strip(die)
    if t is None:
        return None
    tag = t.tag
    if tag == "DW_TAG_base_type":
        sz = t.attributes["DW_AT_byte_size"].value
        enc = t.attributes["DW_AT_encoding"].value
        if enc == 4:
            return "f" if sz == 4 else "d"
        signed = enc in (5, 6)  # DW_ATE_signed, signed_char
        if sz == 8:
            return "q" if signed else "Q"
        return ("i", sz, signed)
    if tag in ("DW_TAG_pointer_type", "DW_TAG_array_type", "DW_TAG_subroutine_type"):
        return "p"
    if tag == "DW_TAG_enumeration_type":
        return ("i", t.attributes["DW_AT_byte_size"].value, True)
    if tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
        sz = t.attributes["DW_AT_byte_size"].value if "DW_AT_byte_size" in t.attributes else 0
        return ("s", sz, _hfa(t))
    return ("i", 4, True)


def load_protos(objs):
    protos = {}
    decls = {}
    for path in objs:
        with open(path, "rb") as f:
            elf = ELFFile(f)
            if not elf.has_dwarf_info():
                continue
            for cu in elf.get_dwarf_info().iter_CUs():
                for die in cu.iter_DIEs():
                    if die.tag != "DW_TAG_subprogram":
                        continue
                    na = die.attributes.get("DW_AT_name")
                    if not na:
                        continue
                    name = na.value.decode()
                    isdecl = "DW_AT_declaration" in die.attributes
                    if "DW_AT_specification" in die.attributes or "DW_AT_abstract_origin" in die.attributes:
                        continue
                    tgt = decls if isdecl else protos
                    if name in tgt:
                        continue
                    p = Proto()
                    p.name = name
                    p.prototyped = "DW_AT_prototyped" in die.attributes
                    p.ret = kind_of(die.get_DIE_from_attribute("DW_AT_type")) if "DW_AT_type" in die.attributes else None
                    for ch in die.iter_children():
                        if ch.tag == "DW_TAG_formal_parameter":
                            k = kind_of(ch.get_DIE_from_attribute("DW_AT_type")) if "DW_AT_type" in ch.attributes else ("i", 4, True)
                            if not p.prototyped and k == "f":
                                k = "d"
                            p.params.append(k)
                        elif ch.tag == "DW_TAG_unspecified_parameters":
                            p.variadic = True
                    tgt[name] = p
    for n, p in decls.items():
        protos.setdefault(n, p)
    return protos


# C library functions: always called natively (never translated or wrapped).
LIBC = {
    "memcpy": ("p", ["p", "p", ("i", 4, False)]), "memset": ("p", ["p", ("i", 4, True), ("i", 4, False)]),
    "memmove": ("p", ["p", "p", ("i", 4, False)]), "memcmp": (("i", 4, True), ["p", "p", ("i", 4, False)]),
    "strcpy": ("p", ["p", "p"]), "strncpy": ("p", ["p", "p", ("i", 4, False)]), "strcat": ("p", ["p", "p"]),
    "strncat": ("p", ["p", "p", ("i", 4, False)]),
    "strcmp": (("i", 4, True), ["p", "p"]), "strncmp": (("i", 4, True), ["p", "p", ("i", 4, False)]),
    "strlen": (("i", 4, False), ["p"]), "strchr": ("p", ["p", ("i", 4, True)]), "strrchr": ("p", ["p", ("i", 4, True)]),
    "strstr": ("p", ["p", "p"]), "toupper": (("i", 4, True), [("i", 4, True)]),
    "tolower": (("i", 4, True), [("i", 4, True)]), "atoi": (("i", 4, True), ["p"]), "atof": ("d", ["p"]),
    "abs": (("i", 4, True), [("i", 4, True)]), "rand": (("i", 4, True), []), "srand": (None, [("i", 4, False)]),
    "sqrt": ("d", ["d"]), "sqrtf": ("f", ["f"]), "fmod": ("d", ["d", "d"]), "fabs": ("d", ["d"]),
    "sin": ("d", ["d"]), "cos": ("d", ["d"]), "tan": ("d", ["d"]), "atan": ("d", ["d"]), "atan2": ("d", ["d", "d"]),
    "asin": ("d", ["d"]), "acos": ("d", ["d"]), "floor": ("d", ["d"]), "ceil": ("d", ["d"]), "pow": ("d", ["d", "d"]),
    "exp": ("d", ["d"]), "log": ("d", ["d"]), "log10": ("d", ["d"]), "modf": ("d", ["d", "p"]),
    "sinf": ("f", ["f"]), "cosf": ("f", ["f"]), "atan2f": ("f", ["f", "f"]), "fmodf": ("f", ["f", "f"]),
    "malloc": ("p", [("i", 4, False)]), "free": (None, ["p"]),
    "strcasecmp": (("i", 4, True), ["p", "p"]), "strncasecmp": (("i", 4, True), ["p", "p", ("i", 4, False)]),
}
# variadic C library functions: handled by runtime helpers reading the ppc context
# single-precision shadow registers (see Translator.fsall); PPC2C_FLOAT=0 turns
# them off, PPC2C_FMA=0 keeps fused single ops in double (exact rounding)
FLOAT_SHADOW = os.environ.get("PPC2C_FLOAT", "1") != "0"
FLOAT_FMA = os.environ.get("PPC2C_FMA", "1") != "0"
VOLATILE_F = {"f%d" % i for i in range(14)}
VARARGS = {"sprintf": "ppc_va_sprintf", "printf": "ppc_va_printf", "OSReport": "ppc_va_printf"}


def libc_proto(name):
    r, ps = LIBC[name]
    p = Proto()
    p.name, p.ret, p.params = name, r, list(ps)
    return p


def ctype(k):
    if k is None:
        return "void"
    if k == "p":
        return "void *"
    if k == "f":
        return "float"
    if k == "d":
        return "double"
    if k == "q":
        return "long long"
    if k == "Q":
        return "unsigned long long"
    if k[0] == "i":
        base = {1: "char", 2: "short", 4: "int"}[k[1]]
        return ("signed " if k[2] else "unsigned ") + base
    if k[0] == "s":
        return "struct ppc_hfa%d" % k[2] if k[2] else "struct ppc_blob%d" % k[1]
    raise ValueError(k)


# -------------------------------------------------------------- translator

CRBITS = {"lt": 0, "gt": 1, "eq": 2, "so": 3, "un": 3}
BRCOND = {  # mnemonic stem -> (bit-in-field, value)
    "lt": (0, 1), "ge": (0, 0), "nl": (0, 0), "gt": (1, 1), "le": (1, 0), "ng": (1, 0),
    "eq": (2, 1), "ne": (2, 0), "so": (3, 1), "ns": (3, 0), "un": (3, 1), "nu": (3, 0),
}


# Calls from translated code that go to a port hook instead (port/src/game/port_hooks.c)
HOOKS = {"firstscreens": "port_hook_firstscreens", "PlayCutMovie": "port_hook_PlayCutMovie",
         "LoadIconC8": "port_hook_LoadIconC8",
         "NuHGobjRndrMtxDwa": "port_hook_NuHGobjRndrMtxDwa",
         "LoadBuggyData": "port_hook_LoadBuggyData"}
# Native names of translated functions that the port renames (main.c is built with -Dmain=cwoc_game_main)
RENAME = {"main": "cwoc_game_main"}
# lwz that reads two adjacent u16 fields of a native (little-endian) struct as
# one big-endian word: load them as halves so the upper/lower parts match.
# CrateCollisions: obj->attack (u16) + vulnerable tested with andis. 0x480
# (mech stomp / slam bits of attack), so the mech could not break steel crates.
LWZ_HALVES = {0x8003FF60, 0x800400A4}

TRACE = set(filter(None, os.environ.get("PPC2C_TRACE", "").split(",")))


class Unsupported(Exception):
    pass


def crbit(s):
    s = s.strip()
    m = re.match(r"^(?:4\s*\*\s*)?cr(\d)\s*\+?\s*(lt|gt|eq|so|un)$", s)
    if m:
        return int(m.group(1)) * 4 + CRBITS[m.group(2)]
    if s in CRBITS:
        return CRBITS[s]
    return int(s, 0)


def crfield(s):
    m = re.match(r"^cr(\d)$", s.strip())
    if not m:
        raise Unsupported("crfield " + s)
    return int(m.group(1))


def imm(s):
    return int(s.strip(), 0)


class Ctx:
    """Global translation context (symbol resolution etc.)."""

    def __init__(self, a):
        self.a = a
        self.dol = gdd.Dol(a.dol)
        self.funcs, self.fglobals, self.fby_file = parse_funcs(a.asm)
        self.data = gdd.parse_asm(a.asm)
        self.dglobals = {}
        self.dby_file = defaultdict(dict)
        for o in self.data:
            if o.scope == "global":
                self.dglobals.setdefault(o.name, o)
            self.dby_file[o.file].setdefault(o.name, o)
        self.data_sorted = sorted((o.addr, o.size, o.file, o.name) for o in self.data if o.size)
        self.data_starts = [d[0] for d in self.data_sorted]
        self.undefined, self.defined, self.common = gdd.nm_symbols(a.objs)
        # local (static) definitions in C objects
        self.cstatic = defaultdict(set)
        out = subprocess.run([os.environ.get("NM", "nm"), "-A"] + a.objs, capture_output=True, text=True).stdout
        self.weak = set()
        for line in out.splitlines():
            parts = line.split()
            if len(parts) < 3:
                continue
            t, n = parts[-2], parts[-1]
            if t in "bdrt":
                self.cstatic[n].add(os.path.basename(parts[0].split(":")[0]))
            if t in "WV":
                self.weak.add(n)
            elif t in "wv" and n not in self.defined:
                # weak reference without any definition (#pragma weak on a
                # function only the translation provides): needs a wrapper
                self.undefined.add(n)
        self.protos = load_protos(a.objs)
        self.ti = ppc_types.TypeInfo(a.objs)
        self.narrow_fixes = 0
        self.units = set()
        for o in a.objs:
            if "/port/src/" in o.replace("\\", "/"):
                continue
            self.units.add(os.path.basename(o).split(".")[0])
        for n in LIBC:
            self.protos[n] = libc_proto(n)
        self._tdb = None
        self.wanted = set()     # DOL data objects the translation needs (dol_ADDR)
        self.widths = defaultdict(set)  # DOL data addr -> integer access widths seen
        self.externs = {}       # C identifier -> 'data' | 'func'
        self.errors = []
        self.warns = []
        self.recomp = []        # function keys in order
        self.recomp_set = set()
        self.native_calls = set()
        self.indirect_shims = set()

    # -- symbols
    def find(self, file, name):
        f = self.fby_file[file].get(name) or self.fglobals.get(name)
        if f is not None:
            return ("func", f)
        o = self.dby_file[file].get(name) or self.dglobals.get(name)
        if o is not None:
            return ("data", o)
        return (None, None)

    def is_c_func(self, name):
        """strong C definition usable natively"""
        if name in LIBC:
            return True
        t = self.defined.get(name)
        return t is not None and t in "T" and name not in self.weak

    def allowed(self, f):
        """may this asm function be translated? (game units and compiler helpers only;
        SDK / hardware code must never run translated)"""
        b = f.file[:-2]
        return b in self.units or "libgcc" in b

    def want_func(self, f):
        key = (f.file, f.name)
        if key not in self.recomp_set:
            self.recomp_set.add(key)
            self.recomp.append(key)
        return "rec_" + mangle(f)

    def sym_addr(self, file, name):
        """C expression (u32) for the address of asm symbol `name` in `file`."""
        kind, obj = self.find(file, name)
        if kind == "func":
            f = obj
            if self.is_c_func(f.name) and (f.file, f.name) not in self.recomp_set:
                self.externs[f.name] = "func"
                self.native_calls.add(f.name)
                return "((u32)(uintptr_t)%s)" % f.name
            self.want_func(f)
            # address-taken translated function: point at its native wrapper
            self.need_wrapper.add((f.file, f.name))
            return "((u32)(uintptr_t)%s)" % wrapper_name(f)
        if kind == "data":
            o = obj
            n = o.name
            m = re.match(r"^(\w+)_([0-9A-F]{8})$", n)
            if m and int(m.group(2), 16) == o.addr and n not in self.defined and n not in self.common \
                    and (m.group(1) in self.cstatic or m.group(1) in self.defined or m.group(1) in self.common):
                n = m.group(1)  # dtk disambiguated a name that the C code has once
            cstat = self.cstatic.get(n, set())
            unit = o.file.split(".")[0]
            if any(x.split(".")[0] == unit for x in cstat):
                self.errors.append("%s (%s): static in C; make it non-static under PORT" % (n, o.file))
            elif cstat and n not in self.defined and n not in self.common:
                # dtk may place a C file's static bss/data in another unit's
                # split: the translation would get its own dol_ copy
                self.errors.append("%s (%s): static in C (%s); make it non-static under PORT"
                                   % (n, o.file, ",".join(sorted(cstat))))
            if re.match(r"^[A-Za-z_]\w*$", n) and (n in self.defined or n in self.common or n in self.undefined):
                self.externs[n] = "data"
                return "((u32)(uintptr_t)%s)" % n
            cn = "dol_%08X" % o.addr
            self.wanted.add(o.addr)
            self.externs[cn] = "data"
            return "((u32)(uintptr_t)%s)" % cn
        self.errors.append("unknown symbol %s in %s" % (name, file))
        return "0u /* %s */" % name

    def scalar_at(self, file, name, add):
        """(leaf_off, leaf_size) of the scalar containing byte `add` of data
        symbol `name`, or None. Used to remap narrow (byte/halfword) accesses
        into wider scalars: big-endian sub-word offsets differ on the port."""
        kind, o = self.find(file, name)
        if kind != "data":
            return None
        if self._tdb is None:
            names = {x.name for x in self.data if x.scope == "global"}
            self._tdb = gdd.TypeDB(self.a.objs, names)
        lay = self._tdb.layouts.get(o.name) if o.scope == "global" else None
        if lay is not None:
            size, leaves, elem = lay
            if size is None and leaves and leaves[0][0] == "unbounded":
                _, esz, eleaves = leaves[0]
                base = (add // esz) * esz
                leaves = [(base + lo, ls, k, b) for lo, ls, k, b in eleaves]
            elif elem is not None and size is not None and add >= size:
                return None
            for lo, ls, k, b in leaves:
                if k == "bitfield":
                    b0 = lo + (b[0] // (b[2] * 8)) * b[2]
                    if b0 <= add < b0 + b[2]:
                        return (b0, b[2])
                    continue
                if lo <= add < lo + ls:
                    if ls == 8 and k == "int":
                        return (lo + (add - lo) // 4 * 4, 4)
                    return (lo, ls) if k in ("int", "float", "ptr") else None
            return None
        for io, iw, ik in o.items:
            if io <= add < io + iw:
                if ik in ("int", "float", "ptr") and iw in (2, 4):
                    return (io, iw)
                if ik in ("int",) and iw == 8:
                    return (io + (add - io) // 4 * 4, 4)
                return None
        if not o.items and o.size in (2, 4):
            return (0, o.size)
        return None

    def narrow(self, file, name, add, width):
        """byte offset to use on the port for a `width`-byte access at sym+add"""
        sc = self.scalar_at(file, name, add)
        if sc is None:
            return add
        lo, ls = sc
        if ls <= width or add + width > lo + ls:
            return add
        rel = add - lo
        return lo + (ls - width - rel)

    def ro_const(self, file, name, add, size):
        kind, o = self.find(file, name)
        if kind == "data" and o.section in RO_SECTIONS:
            raw = self.dol.read(o.addr + add, size)
            return raw
        return None


def mangle(f):
    if f.scope in ("global", "weak"):
        return re.sub(r"\W", "_", f.name)
    return re.sub(r"\W", "_", f.name) + "_" + re.sub(r"\W", "_", f.file.split(".")[0])


def wrapper_name(f):
    if f.name in RENAME:
        return RENAME[f.name]
    return f.name if f.scope in ("global", "weak") and re.match(r"^[A-Za-z_]\w*$", f.name) else "recw_" + mangle(f)


# Biased base addresses that dtk names relative to an unrelated neighbour:
# the original code indexes the *next* object with a negative bias (e.g.
# &Font3DObjTab[c - 'a']). In the port the objects are not adjacent, so the
# expression is rebuilt against the real target: (sym, add) -> (sym, add).
REBASE = {
    ("H2OElec", 0x268): ("Font3DObjTab", -0x48C),  # Text3D / Text3D2
}

SYMOP = re.compile(r"^(?P<sym>[^@]+?)(?:\+(?P<add>0x[0-9A-Fa-f]+|\d+))?@(?P<rel>ha|h|l|sda21)$")


class FnTr:
    def __init__(self, cx, f):
        self.cx, self.f = cx, f
        self.out = []
        self.used_labels = set()
        self.idx = 0

        self.fix_raw_lis()

        def symtype(sym):
            kind, o = cx.find(f.file, sym)
            if kind != "data":
                return None
            return cx.ti.vars.get(o.name) if o.scope == "global" else None
        try:
            self.inf = ppc_types.Inference(cx.ti, f, symtype).run()
        except Exception as ex:  # inference is best-effort
            cx.warns.append("type inference failed in %s: %s" % (f.name, ex))
            self.inf = None

    def e(self, s):
        self.out.append("  " + s)

    def sym_dol_addr(self, sym):
        kind, o = self.cx.find(self.f.file, sym)
        return o.addr if o is not None else None

    def data_at(self, addr):
        """(file, name, offset) of the DOL data object containing addr"""
        i = bisect.bisect_right(self.cx.data_starts, addr) - 1
        if i < 0:
            return None
        a, sz, file, name = self.cx.data_sorted[i]
        if addr < a + sz:
            return file, name, addr - a
        return None

    def sym_ref(self, file, name, off):
        """assembler spelling of name+off usable from this function's unit"""
        kind, o = self.cx.find(self.f.file, name)
        if o is None or o.file != file:
            return None
        return name if not off else "%s+0x%X" % (name, off)

    def raw_lis_uses(self, i, rd, v):
        """Follow the raw value of `lis rd, v` (through mr / mtctr / mfctr) to
        loads and stores with a raw numeric displacement. Linear scan in code
        order (approximate data flow; ctr is long-lived)."""
        live = {rd}
        uses = []
        for j in range(i + 1, len(self.f.insns)):
            _, mn, ops = self.f.insns[j]
            o = [x.strip() for x in ops.split(",")]
            m = re.match(r"^(-?(?:0x[0-9A-Fa-f]+|\d+))\((r\d+)\)$", o[-1]) if len(o) >= 2 else None
            if m and m.group(2) in live and (mn.startswith("l") or mn.startswith("st")):
                uses.append((j, int(m.group(1), 0)))
            if mn == "mr" and len(o) == 2:
                if o[1] in live:
                    live.add(o[0])
                else:
                    live.discard(o[0])
                continue
            if mn == "mtctr":
                if o[0] in live:
                    live.add("ctr")
                else:
                    live.discard("ctr")
                continue
            if mn == "mfctr":
                if "ctr" in live:
                    live.add(o[0])
                else:
                    live.discard(o[0])
                continue
            if mn.startswith("st") or mn.startswith("cmp") or mn.startswith("b") or mn.startswith("mt"):
                if mn.endswith("u") and m:
                    live.discard(m.group(2))
                continue
            if o and re.match(r"^r\d+$", o[0]):
                live.discard(o[0])
            if not live:
                break
        return uses

    def fix_raw_lis(self):
        """dtk leaves some `lis rX, 0x80NN` unsymbolized when the @l half is far
        away (shared across paths). Recover the symbol from a matching @l use."""
        f = self.f
        new = list(f.insns)
        for i, (addr, mn, ops) in enumerate(f.insns):
            if mn != "lis":
                continue
            o = [x.strip() for x in ops.split(",")]
            v = _try_int(o[1])
            if v is None or not (0x8000 <= v < 0x8200):
                continue
            rd = o[0]
            # 1) the raw value reaches loads/stores with a raw displacement
            #    (e.g. lis r9,0x8012; mtctr r9; ... mfctr r9; lfd f27,0x17c8(r9))
            raw_uses = []
            ok = True
            for j, d in self.raw_lis_uses(i, rd, v):
                hit = self.data_at(((v << 16) + d) & 0xFFFFFFFF)
                ref = self.sym_ref(*hit) if hit else None
                if ref is None:
                    ok = False
                    break
                raw_uses.append((j, ref))
            if raw_uses and ok:
                base = raw_uses[0][1]
                new[i] = (addr, mn, "%s, %s@ha" % (rd, base))
                for j, ref in raw_uses:
                    a2, mn2, ops2 = f.insns[j]
                    o2 = [x.strip() for x in ops2.split(",")]
                    reg = re.match(r"^.*\((r\d+)\)$", o2[-1]).group(1)
                    # same @ha for every use: only valid if they share the high half
                    full = self.sym_dol_addr(ref.split("+")[0]) + (int(ref.split("+")[1], 0) if "+" in ref else 0)
                    fb = self.sym_dol_addr(base.split("+")[0]) + (int(base.split("+")[1], 0) if "+" in base else 0)
                    if ((full + 0x8000) >> 16) != ((fb + 0x8000) >> 16):
                        ok = False
                        break
                    o2[-1] = "%s@l(%s)" % (ref, reg)
                    new[j] = (a2, mn2, ", ".join(o2))
                if ok:
                    continue
                new[i] = f.insns[i]
                for j, _ in raw_uses:
                    new[j] = f.insns[j]
                self.cx.warns.append("%s @%08X: raw lis uses with different @ha" % (f.name, addr))
            pat = re.compile(r"(?:^|,\s*)([^\s,()]+?)@l\(%s\)|^\s*\S+,\s*%s,\s*([^\s,]+?)@l$" % (rd, rd))
            found = None
            for _, mn2, ops2 in f.insns:
                m = pat.search(ops2)
                if not m:
                    continue
                symx = m.group(1) or m.group(2)
                mm = re.match(r"^(.+?)(?:\+(0x[0-9A-Fa-f]+|\d+))?$", symx)
                base_addr = self.sym_dol_addr(mm.group(1))
                if base_addr is None:
                    continue
                full = base_addr + int(mm.group(2) or "0", 0)
                if ((full + 0x8000) >> 16) & 0xFFFF == v:
                    found = symx
                    break
            if found:
                new[i] = (addr, mn, "%s, %s@ha" % (rd, found))
            else:
                self.cx.warns.append("%s @%08X: raw address lis %s not resolved" % (f.name, addr, ops))
        f.insns = new

    # ---- operands
    def r(self, s):
        s = s.strip()
        if not re.match(r"^r\d+$", s):
            raise Unsupported("reg " + s)
        return s

    def fr(self, s):
        s = s.strip()
        if not re.match(r"^f\d+$", s):
            raise Unsupported("freg " + s)
        return s

    # single-precision shadows: inside a basic block, an FPR holding a value
    # rounded to single (lfs, single ops, frsp) is also kept in float sN, so
    # chains of single ops run in float (no double ops, no f64<->f32
    # conversions on the Vita's VFP). fN is always written too (unused
    # conversions are dead code for the compiler).
    def fsall(self, *regs):
        return FLOAT_SHADOW and all(self.fr(x) in self.fs_in for x in regs)

    def fsv(self, x):
        x = self.fr(x)
        return "s" + x[1:] if FLOAT_SHADOW and x in self.fs_in else x

    def fset(self, d, fexpr):
        d = self.fr(d)
        if FLOAT_SHADOW:
            self.e("s%s = %s; %s = (double)s%s;" % (d[1:], fexpr, d, d[1:]))
            self.fs.add(d)
        else:
            self.e("%s = (double)(float)(%s);" % (d, fexpr))

    def ra0(self, s):
        """rA where r0 means literal 0"""
        s = self.r(s)
        return "0u" if s == "r0" else s

    def symval(self, s):
        m = SYMOP.match(s.strip())
        if not m:
            return None
        sym, add, rel = m.group("sym"), int(m.group("add") or "0", 0), m.group("rel")
        if (sym, add) in REBASE:
            sym, add = REBASE[(sym, add)]
            a = "(%s - %du)" % (self.cx.sym_addr(self.f.file, sym), -add) if add < 0 else \
                "(%s + %du)" % (self.cx.sym_addr(self.f.file, sym), add)
            return a, rel, sym, add
        a = self.cx.sym_addr(self.f.file, sym)
        if add:
            a = "(%s + %du)" % (a, add)
        return a, rel, sym, add

    def immval(self, s, signed=True):
        """value of an immediate operand (int literal or symbol relocation)."""
        sv = self.symval(s)
        if sv:
            a, rel, _, _ = sv
            if rel == "ha":
                return "PPC_HA(%s)" % a
            if rel == "h":
                return "PPC_HI(%s)" % a
            if rel == "l":
                return "PPC_LO(%s)" % a if signed else "PPC_LOU(%s)" % a
            # sda21: full address; an address into the middle of a scalar is
            # used for a narrow access (byte for odd offsets, halfword for +2)
            _, _, sym, add = sv
            sc = self.cx.scalar_at(self.f.file, sym, add)
            if sc is not None and add != sc[0]:
                w = 2 if (add - sc[0]) % 2 == 0 else 1
                d = self.cx.narrow(self.f.file, sym, add, w) - add
                if d:
                    return "(%s + (u32)%d)" % (a, d)
            return a
        v = imm(s)
        if signed:
            return "%du" % (v & 0xFFFFFFFF) if v >= 0 else "(u32)%d" % v
        return "%du" % (v & 0xFFFF)

    def mem(self, s, width=4):
        """D-form memory operand -> (C address expr, sym info or None)"""
        m = re.match(r"^(.+)\((r\d+)\)$", s.strip())
        if not m:
            raise Unsupported("mem " + s)
        disp, base = m.group(1), m.group(2)
        sv = self.symval(disp)
        if sv:
            a, rel, sym, add = sv
            delta = 0
            if width < 4:
                delta = self.cx.narrow(self.f.file, sym, add, width) - add
            dx = " + (u32)%d" % delta if delta else ""
            if rel == "sda21":
                return "(%s%s)" % (a, dx), (sym, add)
            if rel == "l":
                b = "0u" if base == "r0" else base
                return "(%s + PPC_LO(%s)%s)" % (b, a, dx), (sym, add)
            raise Unsupported("mem rel " + s)
        d = imm(disp)
        b = "0u" if base == "r0" else base
        if width < 4 and self.inf is not None and base != "r1":
            c = self.inf.container(self.idx, base, d)
            if c is not None:
                r = ppc_types.find(c[0], c[1])
                if r is not None and r[1] in (2, 4, 8) and r[1] > width:
                    lo, ls = r[0], r[1]
                    if ls == 8:  # 64-bit ints keep PPC word order
                        lo, ls = lo + (c[1] - lo) // 4 * 4, 4
                    rel = c[1] - lo
                    if ls > width and rel + width <= ls:
                        nd = d + (ls - width - rel) - rel
                        if nd != d:
                            self.cx.narrow_fixes += 1
                            d = nd
        return ("(%s + (u32)%d)" % (b, d) if d else b), None

    # ---- CR helpers
    def setcr(self, field, expr):
        self.e("cr = PPC_SETCR(cr, %d, %s);" % (field, expr))

    def rec(self, rd):
        self.setcr(0, "PPC_CMPS(%s, 0)" % rd)

    # ---- calls
    def call(self, name, tail=False):
        self.fs -= VOLATILE_F
        cx = self.cx
        kind, obj = cx.find(self.f.file, name)
        if kind != "func":
            cx.errors.append("call to unknown %s from %s" % (name, self.f.name))
            self.e("ppc_fatal(\"call to unknown %s\");" % name)
            return
        f = obj
        if f.name in HOOKS and cx.funcs.get((self.f.file, self.f.name)) is not None:
            p = cx.protos.get(f.name)
            if p is not None:
                cx.protos[HOOKS[f.name]] = p
            self.native_call(HOOKS[f.name])
            if tail:
                self.e("PPC_RETURN();")
            return
        if f.name in VARARGS:
            self.e("PPC_SAVE_ARGS(); g_ppc_sp = r1; %s(); PPC_LOAD_RET();" % VARARGS[f.name])
        elif cx.is_c_func(f.name) and (f.file, f.name) not in cx.recomp_set:
            self.native_call(f.name)
        elif not cx.allowed(f) and (f.file, f.name) not in cx.recomp_set:
            cx.warns.append("call to non-game function %s (%s) from %s: runtime stub" % (f.name, f.file, self.f.name))
            self.e("ppc_missing(\"%s\"); r3 = 0; f1 = 0.0;" % f.name)
        else:
            rn = cx.want_func(f)
            self.e("PPC_SAVE_ARGS(); %s(); PPC_LOAD_RET();" % rn)
        if tail:
            self.e("PPC_RETURN();")

    def native_call(self, name):
        cx = self.cx
        p = cx.protos.get(name)
        cx.externs[name] = "func"
        cx.native_calls.add(name)
        if p is None:
            cx.errors.append("no prototype for native %s" % name)
            self.e("ppc_fatal(\"no prototype for %s\");" % name)
            return
        self.e("g_ppc_sp = r1;")
        args = []
        gi, fi = 3, 1
        stack_off = 8
        ret = p.ret
        if ret is not None and ret[0] == "s" and ret != "p":
            gi = 4  # hidden struct-return pointer in r3
        for k in p.params:
            if k in ("f", "d"):
                if fi > 8:
                    raise Unsupported("fp stack arg")
                args.append("(%s)%s" % (ctype(k), self.fsv("f%d" % fi) if k == "f" else "f%d" % fi))
                fi += 1
                continue
            if k in ("q", "Q"):
                if gi % 2 == 0:
                    gi += 1
                if gi + 1 > 10:
                    raise Unsupported("ll stack arg")
                args.append("(%s)(((unsigned long long)r%d << 32) | r%d)" % (ctype(k), gi, gi + 1))
                gi += 2
                continue
            if gi <= 10:
                src = "r%d" % gi
            else:
                src = "LD32(r1 + %d)" % stack_off
                stack_off += 4
            gi += 1
            if k == "p":
                args.append("(void *)(uintptr_t)%s" % src)
            elif k[0] == "s":
                args.append("*(%s *)(uintptr_t)%s" % (ctype(k), src))
            else:
                args.append("(%s)%s" % (ctype(k), src))
        if p.variadic:
            cx.warns.append("variadic native call %s from %s" % (name, self.f.name))
            for g in range(gi, 11):
                args.append("r%d" % g)
        call = "%s(%s)" % (name, ", ".join(args))
        self.fs -= VOLATILE_F
        if ret == "f" and FLOAT_SHADOW:
            self.e("s1 = %s; f1 = (double)s1;" % call)
            self.fs.add("f1")
        elif ret is None:
            self.e(call + ";")
        elif ret in ("f", "d"):
            self.e("f1 = (double)%s;" % call)
        elif ret in ("q", "Q"):
            self.e("{ unsigned long long _v = (unsigned long long)%s; r3 = (u32)(_v >> 32); r4 = (u32)_v; }" % call)
        elif ret == "p":
            self.e("r3 = (u32)(uintptr_t)%s;" % call)
        elif ret[0] == "s":
            self.e("*(%s *)(uintptr_t)r3 = %s;" % (ctype(ret), call))
        else:
            self.e("r3 = (u32)(%s)%s;" % (ctype(ret), call))

    def indirect(self, reg, tail=False):
        self.fs -= VOLATILE_F
        self.e("PPC_SAVE_ARGS(); g_ppc_sp = r1; ppc_call_indirect(%s); PPC_LOAD_RET();" % reg)
        if tail:
            self.e("PPC_RETURN();")

    # ---- branches
    def target(self, s):
        s = s.strip()
        m = re.match(r"^\.L_([0-9A-F]{8})$", s)
        if m:
            a = int(m.group(1), 16)
            self.used_labels.add(a)
            return "L_%08X" % a, None
        return None, s

    def cond_expr(self, stem, crf):
        bit, val = BRCOND[stem]
        b = crf * 4 + bit
        return "PPC_CRBIT(cr, %d)" % b if val else "!PPC_CRBIT(cr, %d)" % b

    def branch(self, mn, ops):
        mn0 = mn.rstrip("+-")
        o = [x.strip() for x in ops.split(",")] if ops else []
        if mn0 == "b":
            lab, fn = self.target(o[0])
            if lab:
                self.e("goto %s;" % lab)
            else:
                self.call(fn, tail=True)
            return True
        if mn0 == "bl":
            self.call(o[0])
            return True
        if mn0 == "blr":
            self.e("PPC_RETURN();")
            return True
        if mn0 == "blrl":
            self.indirect("lr")
            return True
        if mn0 == "bctr":
            self.indirect("ctr", tail=True)
            return True
        if mn0 == "bctrl":
            self.indirect("ctr")
            return True
        if mn0 in ("bdnz", "bdz"):
            lab, fn = self.target(o[0])
            if not lab:
                raise Unsupported("bdnz to fn")
            self.e("if (%s) goto %s;" % ("--ctr != 0" if mn0 == "bdnz" else "--ctr == 0", lab))
            return True
        m = re.match(r"^b(lt|le|eq|ge|gt|nl|ne|ng|so|ns|un|nu)(lr|ctr)?(l)?$", mn0)
        if m:
            stem, kind, link = m.group(1), m.group(2), m.group(3)
            crf = 0
            rest = o
            if rest and re.match(r"^cr\d$", rest[0]):
                crf = crfield(rest[0])
                rest = rest[1:]
            c = self.cond_expr(stem, crf)
            if kind == "lr" and not link:
                self.e("if (%s) PPC_RETURN();" % c)
            elif kind == "ctr" and not link:
                self.e("if (%s) { PPC_SAVE_ARGS(); g_ppc_sp = r1; ppc_call_indirect(ctr); PPC_LOAD_RET(); PPC_RETURN(); }" % c)
            elif kind is None and not link:
                lab, fn = self.target(rest[0])
                if lab:
                    self.e("if (%s) goto %s;" % (c, lab))
                else:
                    self.e("if (%s) {" % c)
                    self.call(fn, tail=True)
                    self.e("}")
            else:
                raise Unsupported("branch " + mn)
            return True
        return False

    # ---- main translator
    def ins(self, addr, mn, ops):
        e = self.e
        rec = mn.endswith(".") and mn not in ("stwcx.",)
        base = mn[:-1] if rec else mn
        o = [x.strip() for x in ops.split(",")] if ops else []
        self.fs_in = set(self.fs)  # operand states before this instruction
        if o and re.match(r"^f\d+$", o[0]) and (base[0] == "f" or base.startswith("lf")):
            self.fs.discard(o[0])  # written below; S-aware cases add it back
        if self.branch(mn, ops):
            return
        r, fr = self.r, self.fr
        # ---- loads / stores
        LD = {"lbz": "LD8", "lhz": "LD16", "lha": "LDS16", "lwz": "LD32"}
        ST = {"stb": "ST8", "sth": "ST16", "stw": "ST32"}
        mm = re.match(r"^(lbz|lhz|lha|lwz|stb|sth|stw|lfs|lfd|stfs|stfd)(u?)(x?)$", base)
        if mm:
            op, upd, idx = mm.groups()
            if idx:
                a = "(%s + %s)" % (self.ra0(o[1]) if not upd else r(o[1]), r(o[2]))
                syminfo = None
            else:
                width = {"lbz": 1, "stb": 1, "lhz": 2, "lha": 2, "sth": 2}.get(op, 4)
                a, syminfo = self.mem(o[1], width)
            ea = "ea"
            e("{ u32 ea = %s;" % a)
            if op in LD:
                # (no inlining for integer loads: rodata words are often string bytes
                # copied by an inlined strcpy, which must keep memory byte order)
                if op == "lwz" and addr in LWZ_HALVES:
                    e("  %s = ((u32)LD16(%s) << 16) | LD16(%s + 2);" % (r(o[0]), ea, ea))
                else:
                    e("  %s = %s(%s);" % (r(o[0]), LD[op], ea))
            elif op in ST:
                e("  %s(%s, %s);" % (ST[op], ea, r(o[0])))
            elif op == "lfs":
                if syminfo and not upd:
                    raw = self.cx.ro_const(self.f.file, syminfo[0], syminfo[1], 4)
                    if raw is not None:
                        d = fr(o[0])
                        e("  s%s = ppc_f32(0x%08Xu); %s = (double)s%s; /* %r */" % (d[1:], struct.unpack(">I", raw)[0], d, d[1:], struct.unpack(">f", raw)[0]))
                        self.fs.add(d)
                        e("}")
                        return
                d = fr(o[0])
                e("  s%s = LDF(%s); %s = (double)s%s;" % (d[1:], ea, d, d[1:]))
                self.fs.add(d)
            elif op == "lfd":
                if syminfo and not upd:
                    raw = self.cx.ro_const(self.f.file, syminfo[0], syminfo[1], 8)
                    if raw is not None:
                        e("  %s = ppc_f64(0x%016Xull);" % (fr(o[0]), struct.unpack(">Q", raw)[0]))
                        e("}")
                        return
                stack = (not idx and o[1].endswith("(r1)")) or (idx and "r1" in (o[1], o[2]))
                e("  %s = %s(%s);" % (fr(o[0]), "LDD_STK" if stack else "LDD", ea))
            elif op == "stfs":
                e("  STF(%s, %s);" % (ea, self.fsv(o[0])))
            elif op == "stfd":
                stack = (not idx and o[1].endswith("(r1)")) or (idx and "r1" in (o[1], o[2]))
                e("  %s(%s, %s);" % ("STD_STK" if stack else "STD", ea, fr(o[0])))
            if upd:
                rA = o[1] if idx else re.match(r"^.+\((r\d+)\)$", o[1]).group(1)
                e("  %s = ea;" % rA)
            e("}")
            return
        if base == "stfiwx":
            e("ST32(%s + %s, (u32)ppc_dbits(%s));" % (self.ra0(o[1]), r(o[2]), fr(o[0])))
            return
        if base in ("lmw", "stmw"):
            rs = int(o[0][1:])
            a, _ = self.mem(o[1])
            e("{ u32 ea = %s;" % a)
            for i, n in enumerate(range(rs, 32)):
                if base == "lmw":
                    e("  r%d = LD32(ea + %d);" % (n, 4 * i))
                else:
                    e("  ST32(ea + %d, r%d);" % (4 * i, n))
            e("}")
            return
        if base in ("lhbrx", "lwbrx", "sthbrx", "stwbrx"):
            a = "(%s + %s)" % (self.ra0(o[1]), r(o[2]))
            if base == "lhbrx":
                e("%s = __builtin_bswap16(LD16(%s));" % (r(o[0]), a))
            elif base == "lwbrx":
                e("%s = __builtin_bswap32(LD32(%s));" % (r(o[0]), a))
            elif base == "sthbrx":
                e("ST16(%s, __builtin_bswap16((u16)%s));" % (a, r(o[0])))
            else:
                e("ST32(%s, __builtin_bswap32(%s));" % (a, r(o[0])))
            self.cx.warns.append("byte-reversed access in %s" % self.f.name)
            return
        # ---- integer arithmetic
        d = None
        if base == "li":
            e("%s = %s;" % (r(o[0]), self.immval(o[1])))
            return
        if base == "lis":
            sv = self.symval(o[1])
            if sv:
                e("%s = %s << 16;" % (r(o[0]), self.immval(o[1])))
            else:
                e("%s = 0x%08Xu;" % (r(o[0]), (imm(o[1]) << 16) & 0xFFFFFFFF))
            return
        if base == "nop":
            return
        if base == "mr":
            d = r(o[0]); e("%s = %s;" % (d, r(o[1])))
        elif base == "not":
            d = r(o[0]); e("%s = ~%s;" % (d, r(o[1])))
        elif base in ("addi", "subi") and "@sda21" in o[2]:
            d = r(o[0])
            e("%s = %s;" % (d, self.immval(o[2])))
        elif base in ("addi", "subi"):
            d = r(o[0])
            v = self.immval(o[2])
            if base == "subi":
                v = "(u32)-(s32)%s" % v
            e("%s = %s + %s;" % (d, self.ra0(o[1]), v))
        elif base in ("addis", "subis"):
            d = r(o[0])
            v = self.immval(o[2])
            e("%s = %s %s (%s << 16);" % (d, self.ra0(o[1]), "+" if base == "addis" else "-", v))
        elif base == "add":
            d = r(o[0]); e("%s = %s + %s;" % (d, r(o[1]), r(o[2])))
        elif base == "subf":
            d = r(o[0]); e("%s = %s - %s;" % (d, r(o[2]), r(o[1])))
        elif base == "sub":
            d = r(o[0]); e("%s = %s - %s;" % (d, r(o[1]), r(o[2])))
        elif base == "neg":
            d = r(o[0]); e("%s = (u32)-(s32)%s;" % (d, r(o[1])))
        elif base in ("addic", "subic"):
            d = r(o[0])
            v = imm(o[2]) * (-1 if base == "subic" else 1)
            e("{ u32 a = %s, b = (u32)%d; %s = a + b; ca = %s < a; }" % (r(o[1]), v, d, d))
        elif base == "addc":
            d = r(o[0]); e("{ u32 a = %s, b = %s; %s = a + b; ca = %s < a; }" % (r(o[1]), r(o[2]), d, d))
        elif base == "subfc":
            d = r(o[0]); e("{ u32 a = %s, b = %s; %s = b - a; ca = b >= a; }" % (r(o[1]), r(o[2]), d))
        elif base == "subfic":
            d = r(o[0]); e("{ u32 a = %s, b = (u32)%d; %s = b - a; ca = b >= a; }" % (r(o[1]), imm(o[2]), d))
        elif base == "adde":
            d = r(o[0]); e("{ unsigned long long t = (unsigned long long)%s + %s + ca; %s = (u32)t; ca = (u32)(t >> 32); }" % (r(o[1]), r(o[2]), d))
        elif base == "addze":
            d = r(o[0]); e("{ unsigned long long t = (unsigned long long)%s + ca; %s = (u32)t; ca = (u32)(t >> 32); }" % (r(o[1]), d))
        elif base == "addme":
            d = r(o[0]); e("{ unsigned long long t = (unsigned long long)%s + ca + 0xFFFFFFFFull; %s = (u32)t; ca = (u32)(t >> 32); }" % (r(o[1]), d))
        elif base == "subfe":
            d = r(o[0]); e("{ unsigned long long t = (unsigned long long)(u32)~%s + %s + ca; %s = (u32)t; ca = (u32)(t >> 32); }" % (r(o[1]), r(o[2]), d))
        elif base == "subfze":
            d = r(o[0]); e("{ unsigned long long t = (unsigned long long)(u32)~%s + ca; %s = (u32)t; ca = (u32)(t >> 32); }" % (r(o[1]), d))
        elif base == "subfme":
            d = r(o[0]); e("{ unsigned long long t = (unsigned long long)(u32)~%s + ca + 0xFFFFFFFFull; %s = (u32)t; ca = (u32)(t >> 32); }" % (r(o[1]), d))
        elif base == "mulli":
            d = r(o[0]); e("%s = (u32)((s32)%s * (s32)%d);" % (d, r(o[1]), imm(o[2])))
        elif base == "mullw":
            d = r(o[0]); e("%s = (u32)((s32)%s * (s32)%s);" % (d, r(o[1]), r(o[2])))
        elif base == "mulhw":
            d = r(o[0]); e("%s = (u32)(((long long)(s32)%s * (s32)%s) >> 32);" % (d, r(o[1]), r(o[2])))
        elif base == "mulhwu":
            d = r(o[0]); e("%s = (u32)(((unsigned long long)%s * %s) >> 32);" % (d, r(o[1]), r(o[2])))
        elif base == "divw":
            d = r(o[0]); e("%s = ppc_divw(%s, %s);" % (d, r(o[1]), r(o[2])))
        elif base == "divwu":
            d = r(o[0]); e("%s = %s ? %s / %s : 0;" % (d, r(o[2]), r(o[1]), r(o[2])))
        elif base in ("and", "or", "xor", "andc", "orc", "nand", "nor", "eqv"):
            d = r(o[0])
            a, b = r(o[1]), r(o[2])
            ex = {"and": "%s & %s", "or": "%s | %s", "xor": "%s ^ %s", "andc": "%s & ~%s", "orc": "%s | ~%s",
                  "nand": "~(%s & %s)", "nor": "~(%s | %s)", "eqv": "~(%s ^ %s)"}[base] % (a, b)
            e("%s = %s;" % (d, ex))
        elif base in ("andi", "andis"):
            d = r(o[0])
            v = self.immval(o[2], signed=False)
            if base == "andis":
                v = "(%s << 16)" % v
            e("%s = %s & %s;" % (d, r(o[1]), v))
            rec = True
        elif base in ("ori", "oris", "xori", "xoris"):
            d = r(o[0])
            v = self.immval(o[2], signed=False)
            if base.endswith("s"):
                v = "(%s << 16)" % v
            e("%s = %s %s %s;" % (d, r(o[1]), "|" if base.startswith("o") else "^", v))
        elif base == "extsb":
            d = r(o[0]); e("%s = (u32)(s32)(signed char)%s;" % (d, r(o[1])))
        elif base == "extsh":
            d = r(o[0]); e("%s = (u32)(s32)(short)%s;" % (d, r(o[1])))
        elif base == "cntlzw":
            d = r(o[0]); e("%s = %s ? (u32)__builtin_clz(%s) : 32u;" % (d, r(o[1]), r(o[1])))
        elif base == "slw":
            d = r(o[0]); e("%s = (%s & 0x20) ? 0u : %s << (%s & 0x1F);" % (d, r(o[2]), r(o[1]), r(o[2])))
        elif base == "srw":
            d = r(o[0]); e("%s = (%s & 0x20) ? 0u : %s >> (%s & 0x1F);" % (d, r(o[2]), r(o[1]), r(o[2])))
        elif base == "sraw":
            d = r(o[0]); e("{ u32 s = %s, n = %s & 0x3F; s32 v = (s32)s; if (n > 31) { %s = (u32)(v >> 31); ca = v < 0; } else { %s = (u32)(v >> n); ca = v < 0 && (s & ((1u << n) - 1)) != 0; } }" % (r(o[1]), r(o[2]), d, d))
        elif base == "srawi":
            d = r(o[0]); n = imm(o[2])
            e("{ u32 s = %s; %s = (u32)((s32)s >> %d); ca = (s32)s < 0 && (s & 0x%Xu) != 0; }" % (r(o[1]), d, n, (1 << n) - 1))
        elif base in ("rlwinm", "rlwnm", "rlwimi", "slwi", "srwi", "clrlwi", "clrrwi", "clrlslwi", "extlwi", "extrwi",
                      "inslwi", "insrwi", "rotlwi", "rotrwi", "rotlw"):
            d = r(o[0]); s = r(o[1])
            if base == "rlwnm" or base == "rotlw":
                mb, me = (imm(o[3]), imm(o[4])) if base == "rlwnm" else (0, 31)
                e("%s = PPC_ROTL(%s, %s & 31) & 0x%08Xu;" % (d, s, r(o[2]), mask(mb, me)))
            else:
                if base == "rlwinm" or base == "rlwimi":
                    sh, mb, me = imm(o[2]), imm(o[3]), imm(o[4])
                elif base == "slwi":
                    n = imm(o[2]); sh, mb, me = n, 0, 31 - n
                elif base == "srwi":
                    n = imm(o[2]); sh, mb, me = (32 - n) & 31, n, 31
                elif base == "clrlwi":
                    n = imm(o[2]); sh, mb, me = 0, n, 31
                elif base == "clrrwi":
                    n = imm(o[2]); sh, mb, me = 0, 0, 31 - n
                elif base == "clrlslwi":
                    b, n = imm(o[2]), imm(o[3]); sh, mb, me = n, b - n, 31 - n
                elif base == "extlwi":
                    n, b = imm(o[2]), imm(o[3]); sh, mb, me = b, 0, n - 1
                elif base == "extrwi":
                    n, b = imm(o[2]), imm(o[3]); sh, mb, me = (b + n) & 31, 32 - n, 31
                elif base == "inslwi":
                    n, b = imm(o[2]), imm(o[3]); sh, mb, me = (32 - b) & 31, b, b + n - 1
                    base = "rlwimi"
                elif base == "insrwi":
                    n, b = imm(o[2]), imm(o[3]); sh, mb, me = (32 - (b + n)) & 31, b, b + n - 1
                    base = "rlwimi"
                elif base == "rotlwi":
                    sh, mb, me = imm(o[2]), 0, 31
                elif base == "rotrwi":
                    sh, mb, me = (32 - imm(o[2])) & 31, 0, 31
                m = mask(mb, me)
                rot = "PPC_ROTL(%s, %d)" % (s, sh) if sh else s
                if base == "rlwimi":
                    e("%s = (%s & 0x%08Xu) | (%s & 0x%08Xu);" % (d, rot, m, d, (~m) & 0xFFFFFFFF))
                else:
                    e("%s = %s & 0x%08Xu;" % (d, rot, m))
        # ---- compares
        elif base in ("cmpw", "cmplw", "cmpwi", "cmplwi", "cmp", "cmpl", "cmpi", "cmpli"):
            crf = 0
            if o and re.match(r"^cr\d$", o[0]):
                crf = crfield(o[0]); o = o[1:]
            if base in ("cmp", "cmpl", "cmpi", "cmpli"):
                o = o[1:]  # L field
            a = r(o[0])
            if base in ("cmpw", "cmp"):
                e("cr = PPC_SETCR(cr, %d, PPC_CMPS(%s, %s));" % (crf, a, r(o[1])))
            elif base in ("cmplw", "cmpl"):
                e("cr = PPC_SETCR(cr, %d, PPC_CMPU(%s, %s));" % (crf, a, r(o[1])))
            elif base in ("cmpwi", "cmpi"):
                e("cr = PPC_SETCR(cr, %d, PPC_CMPS(%s, (u32)%d));" % (crf, a, imm(o[1])))
            else:
                e("cr = PPC_SETCR(cr, %d, PPC_CMPU(%s, %du));" % (crf, a, imm(o[1]) & 0xFFFF))
            return
        # ---- CR / SPR moves
        elif base in ("crand", "cror", "crxor", "crnand", "crnor", "creqv", "crandc", "crorc"):
            bt, ba, bb = crbit(o[0]), crbit(o[1]), crbit(o[2])
            A, B = "PPC_CRBIT(cr, %d)" % ba, "PPC_CRBIT(cr, %d)" % bb
            ex = {"crand": "%s & %s", "cror": "%s | %s", "crxor": "%s ^ %s", "crnand": "!(%s & %s)",
                  "crnor": "!(%s | %s)", "creqv": "!(%s ^ %s)", "crandc": "%s & !%s", "crorc": "%s | !%s"}[base] % (A, B)
            e("cr = PPC_SETCRBIT(cr, %d, %s);" % (bt, ex))
            return
        elif base in ("crset", "crclr", "crnot", "crmove"):
            bt = crbit(o[0])
            ex = {"crset": "1", "crclr": "0"}.get(base)
            if base == "crnot":
                ex = "!PPC_CRBIT(cr, %d)" % crbit(o[1])
            elif base == "crmove":
                ex = "PPC_CRBIT(cr, %d)" % crbit(o[1])
            e("cr = PPC_SETCRBIT(cr, %d, %s);" % (bt, ex))
            return
        elif base == "mcrf":
            e("cr = PPC_SETCR(cr, %d, PPC_GETCR(cr, %d));" % (crfield(o[0]), crfield(o[1])))
            return
        elif base == "mfcr":
            e("%s = cr;" % r(o[0]))
            return
        elif base == "mtcrf":
            m = 0
            fx = imm(o[0])
            for i in range(8):
                if fx & (0x80 >> i):
                    m |= 0xF << (28 - 4 * i)
            e("cr = (cr & 0x%08Xu) | (%s & 0x%08Xu);" % ((~m) & 0xFFFFFFFF, r(o[1]), m))
            return
        elif base == "mflr":
            e("%s = lr;" % r(o[0])); return
        elif base == "mtlr":
            e("lr = %s;" % r(o[0])); return
        elif base == "mfctr":
            e("%s = ctr;" % r(o[0])); return
        elif base == "mtctr":
            e("ctr = %s;" % r(o[0])); return
        # ---- floating point
        elif base in ("fadds", "fsubs", "fmuls", "fdivs", "fadd", "fsub", "fmul", "fdiv"):
            op = {"add": "+", "sub": "-", "mul": "*", "div": "/"}[base[1:4]]
            single = base.endswith("s")
            if rec:
                raise Unsupported("fp record")
            if single:
                if self.fsall(o[1], o[2]):
                    ex = "s%s %s s%s" % (fr(o[1])[1:], op, fr(o[2])[1:])
                else:
                    ex = "(float)(%s %s %s)" % (fr(o[1]), op, fr(o[2]))
                self.fset(o[0], ex)
            else:
                e("%s = %s %s %s;" % (fr(o[0]), fr(o[1]), op, fr(o[2])))
            return
        elif base in ("fmadds", "fmsubs", "fnmadds", "fnmsubs", "fmadd", "fmsub", "fnmadd", "fnmsub"):
            single = base.endswith("s")
            st = base[:-1] if single else base
            A, C, B = fr(o[1]), fr(o[2]), fr(o[3])
            pat = {"fmadd": "%s * %s + %s", "fmsub": "%s * %s - %s", "fnmadd": "-(%s * %s + %s)",
                   "fnmsub": "-(%s * %s - %s)"}[st]
            if single and FLOAT_FMA and self.fsall(o[1], o[2], o[3]):
                self.fset(o[0], pat % ("s" + A[1:], "s" + C[1:], "s" + B[1:]))
            elif single:
                self.fset(o[0], "(float)(%s)" % (pat % (A, C, B)))
            else:
                e("%s = %s;" % (fr(o[0]), pat % (A, C, B)))
            return
        elif base in ("fmr", "fneg", "fabs", "fnabs", "frsp", "fres") and \
                (o[1] in self.fs_in or base in ("frsp", "fres")):
            a, sa = fr(o[1]), "s" + fr(o[1])[1:]
            S = o[1] in self.fs_in
            if base == "fmr":
                e("s%s = %s; %s = %s;" % (o[0][1:], sa, fr(o[0]), a))
                self.fs.add(o[0])
            elif base in ("fneg", "fabs", "fnabs"):
                ex = {"fneg": "-%s", "fabs": "fabsf(%s)", "fnabs": "-fabsf(%s)"}[base] % sa
                self.fset(o[0], ex)
            elif base == "frsp":
                self.fset(o[0], sa if S else "(float)%s" % a)
            else:  # fres
                self.fset(o[0], "1.0f / %s" % sa if S else "(float)(1.0 / %s)" % a)
            return
        elif base in ("fmr", "fneg", "fabs", "fnabs", "frsp", "fres", "frsqrte", "fctiwz", "fctiw"):
            a = fr(o[1])
            ex = {"fmr": a, "fneg": "-" + a, "fabs": "fabs(%s)" % a, "fnabs": "-fabs(%s)" % a,
                  "frsp": "(double)(float)%s" % a, "fres": "(double)(float)(1.0 / %s)" % a,
                  "frsqrte": "1.0 / sqrt(%s)" % a, "fctiwz": "ppc_fctiwz(%s)" % a, "fctiw": "ppc_fctiw(%s)" % a}[base]
            e("%s = %s;" % (fr(o[0]), ex))
            return
        elif base == "fsel":
            if self.fsall(o[2], o[3]):
                self.fset(o[0], "(%s >= 0.0f) ? s%s : s%s" % (self.fsv(o[1]), o[2][1:], o[3][1:]))
            else:
                e("%s = (%s >= 0.0) ? %s : %s;" % (fr(o[0]), fr(o[1]), fr(o[2]), fr(o[3])))
            return
        elif base in ("fcmpu", "fcmpo"):
            if self.fsall(o[1], o[2]):
                e("cr = PPC_SETCR(cr, %d, PPC_FCMP(s%s, s%s));" % (crfield(o[0]), o[1][1:], o[2][1:]))
            else:
                e("cr = PPC_SETCR(cr, %d, PPC_FCMP(%s, %s));" % (crfield(o[0]), fr(o[1]), fr(o[2])))
            return
        elif base in ("mffs", "mtfsf", "mtfsb0", "mtfsb1", "mtfsfi"):
            if base == "mffs":
                e("%s = 0.0;" % fr(o[0]))
            return
        else:
            raise Unsupported("insn %s %s" % (mn, ops))
        if rec and d is not None:
            self.rec(d)

    # ---- access widths of DOL data objects (lets gen_dol_data byte-swap
    # untyped tables per element: e.g. jeepsfx is read with lha)
    def _note_width(self, sym, width):
        kind, o = self.cx.find(self.f.file, sym)
        if kind == "data" and o is not None:
            self.cx.widths[o.addr].add(width)

    def track(self, mn, ops):
        P = self.prov
        o = [x.strip() for x in ops.split(",")] if ops else []
        base = mn[:-1] if mn.endswith(".") else mn
        m = re.match(r"^(lbz|lhz|lha|lwz|stb|sth|stw)(u?)(x?)$", base)
        if m:
            op, upd, idx = m.groups()
            width = {"lbz": 1, "stb": 1, "lhz": 2, "lha": 2, "sth": 2}.get(op, 4)
            if idx:
                regs = [o[1], o[2]]
            else:
                mm = re.match(r"^(.+)\((r\d+)\)$", o[1])
                regs = [mm.group(2)] if mm else []
                sv = SYMOP.match(mm.group(1)) if mm else None
                if sv and sv.group("rel") in ("l", "sda21"):
                    self._note_width(sv.group("sym"), width)
                    regs = []
            for rg in regs:
                if P.get(rg, (None, None))[1] == "addr":
                    self._note_width(P[rg][0], width)
            if op[0] == "l":
                P.pop(o[0], None)
            if upd and regs:
                P.pop(regs[0], None)
            return
        if not o or not re.match(r"^r\d+$", o[0]):
            return
        if base.startswith("st") or base.startswith("cmp") or base.startswith("b") or base.startswith("mt") \
                or base in ("lfs", "lfd", "lfsx", "lfdx", "lfsu", "lfdu", "dcbz", "dcbf", "dcbst", "icbi"):
            return
        rd = o[0]
        if base == "li" and len(o) == 2:
            sv = SYMOP.match(o[1])
            if sv and sv.group("rel") == "sda21":
                P[rd] = (sv.group("sym"), "addr")
                return
        if base == "lis" and len(o) == 2:
            sv = SYMOP.match(o[1])
            if sv and sv.group("rel") == "ha":
                P[rd] = (sv.group("sym"), "ha")
                return
        if base in ("addi", "la") and len(o) == 3:
            sv = SYMOP.match(o[2])
            if sv and sv.group("rel") == "l" and P.get(o[1], (None,))[0] == sv.group("sym"):
                P[rd] = (sv.group("sym"), "addr")
                return
            if sv and sv.group("rel") == "sda21":
                P[rd] = (sv.group("sym"), "addr")
                return
            if not sv and P.get(o[1], (None, None))[1] == "addr":
                P[rd] = P[o[1]]
                return
        if base == "mr" and len(o) == 2 and o[1] in P:
            P[rd] = P[o[1]]
            return
        if base == "add" and len(o) == 3:
            src = [x for x in o[1:] if P.get(x, (None, None))[1] == "addr"]
            if src:
                P[rd] = P[src[0]]
                return
        P.pop(rd, None)

    def translate(self):
        f = self.f
        body = []
        self.out = body
        self.prov = {}
        self.fs = set()
        self.fs_in = set()
        for self.idx, (addr, mn, ops) in enumerate(f.insns):
            if addr in f.labels:
                self.fs = set()  # join point: single-precision shadows unknown
                body.append("L_%08X:;" % addr)
                if f.name in TRACE:
                    body.append("  ppc_trace(0x%08Xu);" % addr)
            try:
                self.track(mn, ops)
                self.ins(addr, mn, ops)
            except Unsupported as ex:
                self.cx.errors.append("%s @%08X: %s" % (f.name, addr, ex))
                body.append("  ppc_fatal(\"untranslated %s at %08X\");" % (mn, addr))
        # falling off the end (should not happen)
        body.append("  PPC_RETURN();")
        hdr = ["/* %s (%s) @ %08X */" % (f.name, f.file, f.addr),
               "void rec_%s(void) {" % mangle(f),
               "  PPC_REGS();"]
        if f.name in TRACE:
            hdr.append("  ppc_trace(0x%08Xu);" % f.addr)
        return "\n".join(hdr + body + ["}", ""])


def _try_int(x):
    try:
        return int(x, 0)
    except ValueError:
        return None


def mask(mb, me):
    m = 0
    if mb <= me:
        for b in range(mb, me + 1):
            m |= 1 << (31 - b)
    else:
        for b in list(range(mb, 32)) + list(range(0, me + 1)):
            m |= 1 << (31 - b)
    return m


# ----------------------------------------------------------- wrappers/shims


def marshal_in(p, prefix_struct_ret):
    """Code that copies native args a0.. into ppc ctx; returns lines."""
    lines = []
    gi, fi = 3, 1
    stack_off = 8
    if prefix_struct_ret:
        gi = 4
    for i, k in enumerate(p.params):
        if k in ("f", "d"):
            lines.append("  ppc.f[%d] = (double)a%d;" % (fi, i))
            fi += 1
        elif k in ("q", "Q"):
            if gi % 2 == 0:
                gi += 1
            lines.append("  ppc.r[%d] = (u32)((unsigned long long)a%d >> 32); ppc.r[%d] = (u32)a%d;" % (gi, i, gi + 1, i))
            gi += 2
        else:
            if k != "p" and k[0] == "s":
                val = "ppc_copy_to_stack(&a%d, sizeof(a%d))" % (i, i)
            elif k == "p":
                val = "(u32)(uintptr_t)a%d" % i
            else:
                val = "(u32)a%d" % i
            if gi <= 10:
                lines.append("  ppc.r[%d] = %s;" % (gi, val))
            else:
                lines.append("  ppc_outarg(%d, %s);" % (stack_off, val))
                stack_off += 4
            gi += 1
    return lines


def gen_wrapper(f, p):
    """native-callable function with the original name -> rec_X"""
    if p is None:  # unknown prototype: pass 8 integer arguments, return r3
        p = Proto()
        p.name, p.ret, p.params = f.name, ("i", 4, False), [("i", 4, False)] * 8
    ret = p.ret if p else None
    params = p.params if p else []
    sret = ret is not None and ret != "p" and ret[0] == "s"
    sig = ", ".join("%s a%d" % (ctype(k), i) for i, k in enumerate(params)) or "void"
    lines = ["%s %s(%s) {" % (ctype(ret), wrapper_name(f), sig),
             "  u32 _sp = g_ppc_sp;",
             "  ppc_enter_native_frame();"]
    if sret:
        lines.append("  %s _ret;" % ctype(ret))
        lines.append("  u32 _rp = ppc_alloc_stack(sizeof(_ret));")
        lines.append("  ppc.r[3] = _rp;")
    if p:
        lines += marshal_in(p, sret)
    lines.append("  ppc_finish_native_frame();")
    lines.append("  rec_%s();" % mangle(f))
    lines.append("  g_ppc_sp = _sp;")
    if ret is None:
        pass
    elif sret:
        lines.append("  __builtin_memcpy(&_ret, (void *)(uintptr_t)_rp, sizeof(_ret));")
        lines.append("  return _ret;")
    elif ret in ("f", "d"):
        lines.append("  return (%s)ppc.f[1];" % ctype(ret))
    elif ret in ("q", "Q"):
        lines.append("  return (%s)(((unsigned long long)ppc.r[3] << 32) | ppc.r[4]);" % ctype(ret))
    elif ret == "p":
        lines.append("  return (void *)(uintptr_t)ppc.r[3];")
    else:
        lines.append("  return (%s)ppc.r[3];" % ctype(ret))
    lines.append("}")
    return "\n".join(lines) + "\n"


def gen_shim(name, p):
    """ppc-ctx -> native call (for indirect calls from translated code)"""
    lines = ["static void shim_%s(void) {" % name]
    args = []
    gi, fi = 3, 1
    off = 8
    ret = p.ret
    sret = ret is not None and ret != "p" and ret[0] == "s"
    if sret:
        gi = 4
    for k in p.params:
        if k in ("f", "d"):
            args.append("(%s)ppc.f[%d]" % (ctype(k), fi)); fi += 1
        elif k in ("q", "Q"):
            if gi % 2 == 0:
                gi += 1
            args.append("(%s)(((unsigned long long)ppc.r[%d] << 32) | ppc.r[%d])" % (ctype(k), gi, gi + 1)); gi += 2
        else:
            src = "ppc.r[%d]" % gi if gi <= 10 else "LD32(ppc.r[1] + %d)" % off
            if gi > 10:
                off += 4
            gi += 1
            if k == "p":
                args.append("(void *)(uintptr_t)%s" % src)
            elif k[0] == "s":
                args.append("*(%s *)(uintptr_t)%s" % (ctype(k), src))
            else:
                args.append("(%s)%s" % (ctype(k), src))
    call = "%s(%s)" % (name, ", ".join(args))
    if ret is None:
        lines.append("  %s;" % call)
    elif ret in ("f", "d"):
        lines.append("  ppc.f[1] = (double)%s;" % call)
    elif ret in ("q", "Q"):
        lines.append("  { unsigned long long v = (unsigned long long)%s; ppc.r[3] = (u32)(v >> 32); ppc.r[4] = (u32)v; }" % call)
    elif ret == "p":
        lines.append("  ppc.r[3] = (u32)(uintptr_t)%s;" % call)
    elif sret:
        lines.append("  *(%s *)(uintptr_t)ppc.r[3] = %s;" % (ctype(ret), call))
    else:
        lines.append("  ppc.r[3] = (u32)(%s)%s;" % (ctype(ret), call))
    lines.append("}")
    return "\n".join(lines) + "\n"


def decl(name, p):
    sig = ", ".join(ctype(k) for k in p.params)
    if p.variadic:
        sig = (sig + ", ..." if sig else "...")
    return "extern %s %s(%s);" % (ctype(p.ret), name, sig or "void")


def write_if_changed(path, text):
    """keep the old file (and its mtime) when nothing changed, so ninja does not
    recompile the big generated sources"""
    try:
        if open(path).read() == text:
            return
    except OSError:
        pass
    open(path, "w").write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--asm", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--list", required=True, help="functions to translate (one per line, # comments)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--wanted", required=True, help="output: DOL data addresses the translation references")
    ap.add_argument("--report")
    ap.add_argument("objs", nargs="+")
    a = ap.parse_args()
    a.objs = [o for o in a.objs if o.endswith((".o", ".obj"))]
    cx = Ctx(a)
    cx.need_wrapper = set()

    seeds = []
    for line in open(a.list):
        line = line.split("#")[0].split("!")[0].strip()
        if not line:
            continue
        name, _, file = line.partition("@")
        f = None
        if file:
            f = cx.fby_file.get(file.strip(), {}).get(name.strip())
        else:
            f = cx.fglobals.get(name)
            if f is None:
                c = [x for k, x in cx.funcs.items() if k[1] == name]
                if len(c) == 1:
                    f = c[0]
        if f is None:
            cx.errors.append("seed %s not found" % line)
            continue
        seeds.append(f)
        cx.want_func(f)
        cx.need_wrapper.add((f.file, f.name))

    seed_keys = {(f.file, f.name) for f in seeds}
    addr_taken_all = set()
    for fn in os.listdir(a.asm):
        if fn.endswith(".s"):
            for line in open(os.path.join(a.asm, fn), errors="replace"):
                for m in re.finditer(r"([A-Za-z_][\w.$]*)(?:\+0x[0-9A-Fa-f]+)?@(?:ha|l)\b", line):
                    addr_taken_all.add(m.group(1))
                if ".4byte " in line:
                    addr_taken_all.update(re.findall(r"\b([A-Za-z_][\w.$]*)\b", line.split(".4byte", 1)[1]))
    # translated functions that C references (undefined, weak stubs) need wrappers
    done = {}
    i = 0
    while i < len(cx.recomp):
        key = cx.recomp[i]
        i += 1
        f = cx.funcs[key]
        done[key] = FnTr(cx, f).translate()
    # seeds only need a native wrapper when native code can reach them
    for key in list(cx.need_wrapper):
        f = cx.funcs[key]
        if key in seed_keys and not (f.name in cx.undefined or f.name in cx.weak or f.name in cx.defined
                                     or f.name in addr_taken_all):
            cx.need_wrapper.discard(key)
    for key in cx.recomp:
        f = cx.funcs[key]
        if f.scope in ("global", "weak") and (f.name in cx.undefined or f.name in cx.weak):
            cx.need_wrapper.add(key)
    # asm-local translations whose (unstaticed) C counterpart is a weak
    # definition: export the wrapper under the C name so the translation
    # actually replaces the decomp body. Only when the name is unique among
    # the asm's functions, otherwise we cannot tell which one C means.
    name_count = {}
    for k in cx.funcs:
        name_count[k[1]] = name_count.get(k[1], 0) + 1
    aliases = []
    for key in cx.recomp:
        f = cx.funcs[key]
        if f.scope in ("global", "weak") or f.name not in cx.weak or f.name in RENAME:
            continue
        if name_count.get(f.name, 0) != 1 or not re.match(r"^[A-Za-z_]\w*$", f.name):
            cx.warns.append("local %s: weak C version kept (name not unique)" % f.name)
            continue
        cx.need_wrapper.add(key)
        aliases.append(key)

    # indirect-call targets: every function whose address the original code or
    # data takes and that exists natively or as a translation
    addr_taken = set()
    for fn in os.listdir(a.asm):
        if not fn.endswith(".s"):
            continue
        for line in open(os.path.join(a.asm, fn), errors="replace"):
            for m in re.finditer(r"([A-Za-z_][\w.$]*)(?:\+0x[0-9A-Fa-f]+)?@(?:ha|l)\b", line):
                addr_taken.add(m.group(1))
            if "\t.4byte " in line or line.lstrip().startswith(".4byte "):
                for t in re.findall(r"\b([A-Za-z_][\w.$]*)\b", line.split(".4byte", 1)[1]):
                    addr_taken.add(t)
    shims = []
    shim_entries = []
    for name in sorted(addr_taken):
        f = cx.fglobals.get(name)
        if f is None:
            # asm-local function whose address C may hand to translated code
            # (callbacks): needs a shim under its C name, which must not be static
            loc = [x for k, x in cx.funcs.items() if k[1] == name]
            if len(loc) != 1:
                continue
            f = loc[0]
            if (f.file, f.name) not in cx.recomp_set and not cx.is_c_func(name):
                if any(x.split(".")[0] == f.file.split(".")[0] for x in cx.cstatic.get(name, ())):
                    cx.warns.append("callback %s is static in C: no shim (unstatic it under PORT)" % name)
                continue
        key = (f.file, f.name)
        if key in cx.recomp_set:
            if key in cx.need_wrapper:
                shim_entries.append("  {(void *)%s, rec_%s}," % (wrapper_name(f), mangle(f)))
            continue
        if cx.is_c_func(name) and name in cx.protos and not cx.protos[name].variadic:
            if re.match(r"^[A-Za-z_]\w*$", name):
                cx.externs[name] = "func"
                cx.native_calls.add(name)
                shims.append(gen_shim(name, cx.protos[name]))
                shim_entries.append("  {(void *)%s, shim_%s}," % (name, name))

    lines = ["/* GENERATED by port/tools/ppc2c.py from the dtk disassembly of the user's",
             " * own main.dol. Derived from game code: never commit or redistribute. */",
             '#include "ppc_rt.h"', ""]
    # struct types used by value
    stypes = set()
    for n in cx.native_calls | {cx.funcs[k].name for k in cx.need_wrapper}:
        p = cx.protos.get(n)
        if not p:
            continue
        for k in p.params + [p.ret]:
            if k is not None and k != "p" and k[0] == "s":
                stypes.add(k)
    for k in sorted(stypes, key=str):
        if k[2]:
            lines.append("struct ppc_hfa%d { float v[%d]; };" % (k[2], k[2]))
        else:
            lines.append("struct ppc_blob%d { unsigned char b[%d]; } __attribute__((aligned(4)));" % (k[1], max(k[1], 1)))
    for n, kind in sorted(cx.externs.items()):
        if kind == "data":
            lines.append("extern unsigned char %s[];" % n)
    for n in sorted(cx.native_calls):
        p = cx.protos.get(n)
        if p:
            lines.append(decl(n, p))
    for key in cx.recomp:
        lines.append("void rec_%s(void);" % mangle(cx.funcs[key]))
    lines.append("")
    for key in sorted(cx.need_wrapper):
        f = cx.funcs[key]
        p = cx.protos.get(RENAME.get(f.name, f.name))
        if p is None:
            cx.warns.append("wrapper %s: no C prototype, assuming 8 int args" % f.name)
        lines.append(gen_wrapper(f, p))
    for key in aliases:
        f = cx.funcs[key]
        lines.append('__typeof__(%s) %s __attribute__((alias("%s")));' % (wrapper_name(f), f.name, wrapper_name(f)))
    for key in cx.recomp:
        lines.append(done[key])
    lines += shims
    lines.append("const struct ppc_shim ppc_shims[] = {")
    lines += shim_entries
    lines.append("  {0, 0}};")
    write_if_changed(a.out, "\n".join(lines) + "\n")
    write_if_changed(a.wanted, "".join(
        "%08X%s\n" % (ad, " w=" + ",".join(str(w) for w in sorted(cx.widths[ad])) if cx.widths.get(ad) else "")
        for ad in sorted(cx.wanted)))
    rep = ["translated: %d functions (%d seeds), %d narrow accesses remapped" % (len(cx.recomp), len(seeds), cx.narrow_fixes),
           "wrappers: %d  shims: %d" % (len(cx.need_wrapper), len(shims)),
           "functions: " + " ".join(cx.funcs[k].name for k in cx.recomp)]
    rep += ["ERROR: " + x for x in sorted(set(cx.errors))]
    rep += ["warn: " + x for x in sorted(set(cx.warns))]
    if a.report:
        open(a.report, "w").write("\n".join(rep) + "\n")
    print("ppc2c: %d functions translated, %d errors, %d warnings" % (len(cx.recomp), len(set(cx.errors)), len(set(cx.warns))))
    for x in sorted(set(cx.errors))[:40]:
        print("  ERROR:", x)
    if cx.errors and not os.environ.get("PPC2C_ALLOW_ERRORS"):
        os.unlink(a.out)
        sys.exit(1)


if __name__ == "__main__":
    main()
