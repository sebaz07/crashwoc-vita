#!/usr/bin/env python3
"""Generate native-endian C definitions for GameCube data the port still needs.

The decompiled C references many globals whose initial contents only exist in
the original DOL (the matching build links them from dtk-generated assembly).
The native port cannot use big-endian blobs or 0x80xxxxxx pointers, so this tool:

  1. reads the user's own main.dol and the dtk assembly of its data sections
     (for object boundaries, directive widths and symbolic relocations);
  2. finds which data symbols the port objects leave undefined (or only have as
     zero-initialised commons while the DOL has real contents);
  3. uses the DWARF type information of the *port* objects (compiled for the
     target, so layout matches what the C code expects) to byte-swap every
     scalar leaf, convert big-endian bitfields, and record pointer fields;
  4. emits a C file with byte arrays in native layout plus a relocation table
     applied by port_dol_data_init() at startup.

The output contains game data and must never be committed or redistributed;
it is regenerated locally from the user's own files.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
from collections import defaultdict

from elftools.elf.elffile import ELFFile
import elf_compat  # noqa: E402,F401  (tolerate unknown relocation types)

# ---------------------------------------------------------------- DOL reader


class Dol:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        offs = struct.unpack(">18I", d[0:72])
        addrs = struct.unpack(">18I", d[72:144])
        sizes = struct.unpack(">18I", d[144:216])
        self.bss_addr, self.bss_size = struct.unpack(">2I", d[216:224])
        self.sections = [(a, s, o) for o, a, s in zip(offs, addrs, sizes) if s]

    def read(self, addr, size):
        for a, s, o in self.sections:
            if a <= addr and addr + size <= a + s:
                return self.data[o + addr - a: o + addr - a + size]
        if self.bss_addr <= addr < self.bss_addr + self.bss_size:
            return bytes(size)
        raise KeyError("address %08X not in DOL" % addr)


# ---------------------------------------------------------- asm data parsing

OBJ_HDR = re.compile(r"^# \.(\w+):0x[0-9A-Fa-f]+ \| 0x([0-9A-Fa-f]+) \| size: 0x([0-9A-Fa-f]+)")
DATA_SECTIONS = {"data", "sdata", "rodata", "data5", "bss", "sbss", "sdata2"}


class AsmObj:
    __slots__ = ("name", "scope", "addr", "size", "section", "file", "items", "relocs")

    def __init__(self):
        self.items = []    # (offset, width, kind) kind: 'int','float','bytes'
        self.relocs = []   # (offset, target_name, addend)


def parse_int(s):
    s = s.strip()
    return int(s, 0)


def parse_asm(asm_dir):
    objs = []
    # Function names: a data word that dtk wrote as "function+offset" outside a
    # jump table (@NNN objects) is a number that happens to fall inside .text,
    # e.g. the 64-bit level masks of CrashAnim (0x80100000 for levels 20 and
    # 31 became SIInterruptHandler+0x120, so "c\\mechrun" never loaded).
    funcs = set()
    for fn in sorted(os.listdir(asm_dir)):
        if fn.endswith(".s"):
            for line in open(os.path.join(asm_dir, fn), encoding="utf-8", errors="replace"):
                if line.startswith(".fn "):
                    funcs.add(line[4:].split(",")[0].strip())
    for fn in sorted(os.listdir(asm_dir)):
        if not fn.endswith(".s"):
            continue
        cur = None
        hdr = None
        off = 0
        for line in open(os.path.join(asm_dir, fn), encoding="utf-8", errors="replace"):
            line = line.rstrip("\n")
            m = OBJ_HDR.match(line)
            if m:
                hdr = (m.group(1), int(m.group(2), 16), int(m.group(3), 16))
                continue
            st = line.strip()
            if st.startswith(".obj "):
                parts = st[5:].split(",")
                name = parts[0].strip().strip('"')
                scope = parts[1].strip() if len(parts) > 1 else "global"
                if hdr is None or hdr[0] not in DATA_SECTIONS:
                    cur = None
                    continue
                cur = AsmObj()
                cur.name, cur.scope = name, scope
                cur.section, cur.addr, cur.size = hdr
                cur.file = fn
                off = 0
                hdr = None
                continue
            if st.startswith(".endobj"):
                if cur is not None:
                    objs.append(cur)
                cur = None
                continue
            if cur is None or not st.startswith("."):
                continue
            d, _, arg = st.partition(" ")
            arg = arg.split("#")[0].strip()
            if d == ".4byte":
                for a in [x.strip() for x in arg.split(",")]:
                    if re.match(r"^-?(0x[0-9A-Fa-f]+|\d+)$", a):
                        cur.items.append((off, 4, "int"))
                    else:
                        a = a.strip('"')
                        m2 = re.match(r'^"?([^"+]+)"?(?:\+(0x[0-9A-Fa-f]+|\d+))?$', a)
                        tgt, add = m2.group(1), int(m2.group(2) or "0", 0)
                        if tgt in funcs and add != 0 and not cur.name.startswith("@"):
                            cur.items.append((off, 4, "int"))
                        else:
                            cur.relocs.append((off, tgt, add))
                            cur.items.append((off, 4, "ptr"))
                    off += 4
            elif d == ".2byte":
                for _ in arg.split(","):
                    cur.items.append((off, 2, "int"))
                    off += 2
            elif d == ".byte":
                n = len(arg.split(","))
                cur.items.append((off, n, "bytes"))
                off += n
            elif d == ".float":
                cur.items.append((off, 4, "float"))
                off += 4
            elif d == ".double":
                cur.items.append((off, 8, "float"))
                off += 8
            elif d in (".string", ".ascii"):
                s = bytes(arg[1:-1], "utf-8").decode("unicode_escape").encode("latin-1")
                n = len(s) + (1 if d == ".string" else 0)
                cur.items.append((off, n, "bytes"))
                off += n
            elif d == ".skip":
                n = parse_int(arg)
                cur.items.append((off, n, "bytes"))
                off += n
    return objs


# --------------------------------------------------------------- DWARF types


class TypeDB:
    """Collect DWARF variable types from the port's own object files."""

    def __init__(self, objfiles, wanted, typenames=()):
        self.layouts = {}  # name -> (size, leaves) leaves: (off, size, kind, bitinfo)
        self.named = {}    # 'struct x' / typedef name -> (size, leaves)
        self.hinted = {}   # DOL addr -> layout (from the --types hints)
        self.defined_layout = set()  # names whose layout came from a definition
        typenames = set(typenames)
        for path in objfiles:
            with open(path, "rb") as f:
                elf = ELFFile(f)
                if not elf.has_dwarf_info():
                    continue
                dw = elf.get_dwarf_info()
                for cu in dw.iter_CUs():
                    for die in cu.iter_DIEs():
                        if typenames and die.tag in ("DW_TAG_structure_type", "DW_TAG_typedef") \
                                and "DW_AT_name" in die.attributes:
                            tn = die.attributes["DW_AT_name"].value.decode()
                            if die.tag == "DW_TAG_structure_type":
                                tn = "struct " + tn
                            if tn in typenames and tn not in self.named and \
                                    (die.tag == "DW_TAG_typedef" or "DW_AT_byte_size" in die.attributes):
                                try:
                                    self.named[tn] = self._layout(die, 0)
                                except Exception:
                                    pass
                            continue
                        if die.tag != "DW_TAG_variable":
                            continue
                        na = die.attributes.get("DW_AT_name")
                        if not na:
                            continue
                        name = na.value.decode()
                        is_decl = "DW_AT_declaration" in die.attributes
                        if name not in wanted:
                            continue
                        # the defining unit's type wins over `extern` declarations
                        # (decomp files declare private, sometimes stub, copies
                        # of shared structs, e.g. a 4-byte objtab_s in ai.c)
                        if name in self.layouts and (is_decl or name in self.defined_layout):
                            continue
                        t = self._type(die)
                        if t is None:
                            continue
                        try:
                            size, leaves = self._layout(t, 0)
                        except Exception:
                            continue
                        self.layouts[name] = (size, leaves, self._elem(t))
                        if not is_decl:
                            self.defined_layout.add(name)

    @staticmethod
    def _type(die):
        if "DW_AT_type" not in die.attributes:
            return None
        return die.get_DIE_from_attribute("DW_AT_type")

    def _strip(self, t):
        while t is not None and t.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
            t = self._type(t)
        return t

    def pointee_layout(self, die):
        """Layout of an object reached through a typed pointer: treated as an
        array of the pointee type (the C code indexes these tables)."""
        if die is None:
            return None
        t = self._strip(die)
        if t is None or t.tag in ("DW_TAG_subroutine_type", "DW_TAG_base_type") and False:
            return None
        try:
            esz, eleaves = self._layout(t, 0)
        except Exception:
            return None
        if not esz:
            return None
        return (None, [("unbounded", esz, eleaves)], None)

    def _elem(self, t):
        t = self._strip(t)
        if t is not None and t.tag == "DW_TAG_array_type":
            e = self._strip(self._type(t))
            return self._layout(e, 0)
        return None

    def _layout(self, t, base):
        t = self._strip(t)
        if t is None:
            raise ValueError("void")
        tag = t.tag
        if tag == "DW_TAG_base_type":
            sz = t.attributes["DW_AT_byte_size"].value
            enc = t.attributes["DW_AT_encoding"].value
            kind = "float" if enc == 4 else "int"
            return sz, [(base, sz, kind, None)]
        if tag == "DW_TAG_pointer_type":
            return 4, [(base, 4, "ptr", self._type(t))]
        if tag == "DW_TAG_subroutine_type":
            return 4, [(base, 4, "ptr", None)]
        if tag == "DW_TAG_enumeration_type":
            sz = t.attributes["DW_AT_byte_size"].value
            return sz, [(base, sz, "int", None)]
        if tag == "DW_TAG_array_type":
            esz, eleaves = self._layout(self._type(t), 0)
            count = None
            for ch in t.iter_children():
                if ch.tag == "DW_TAG_subrange_type":
                    if "DW_AT_upper_bound" in ch.attributes:
                        ub = ch.attributes["DW_AT_upper_bound"].value
                        c = ub + 1
                    elif "DW_AT_count" in ch.attributes:
                        c = ch.attributes["DW_AT_count"].value
                    else:
                        c = None
                    count = c if count is None else (None if c is None else count * c)
            if count is None:
                return None, [("unbounded", esz, eleaves)]
            leaves = []
            for i in range(count):
                leaves.extend((base + i * esz + o, s, k, b) for o, s, k, b in eleaves)
            return esz * count, leaves
        if tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            if "DW_AT_byte_size" not in t.attributes:
                raise ValueError("incomplete struct")
            sz = t.attributes["DW_AT_byte_size"].value
            leaves = []
            first = True
            for m in t.iter_children():
                if m.tag != "DW_TAG_member":
                    continue
                if tag == "DW_TAG_union_type" and not first:
                    break
                first = False
                mt = self._type(m)
                if "DW_AT_bit_size" in m.attributes:
                    bs = m.attributes["DW_AT_bit_size"].value
                    if "DW_AT_data_bit_offset" in m.attributes:
                        bo = m.attributes["DW_AT_data_bit_offset"].value
                    else:
                        loc = m.attributes.get("DW_AT_data_member_location")
                        bo = (loc.value if loc else 0) * 8 + m.attributes.get("DW_AT_bit_offset").value
                    ut = self._strip(mt)
                    unit = ut.attributes["DW_AT_byte_size"].value if ut is not None and "DW_AT_byte_size" in ut.attributes else 4
                    leaves.append((base, 0, "bitfield", (bo, bs, unit)))
                    continue
                loc = m.attributes.get("DW_AT_data_member_location")
                moff = loc.value if loc is not None else 0
                msz, mleaves = self._layout(mt, base + moff)
                if msz is None:  # flexible member
                    continue
                leaves.extend(mleaves)
            return sz, leaves
        raise ValueError("unhandled tag " + tag)


# ------------------------------------------------------------------- helpers


PLATFORM_OBJ = re.compile(r"cwoc_objs\.dir/(?:\./)*src/(?!game/)")


UNDEF_REFS = defaultdict(set)  # symbol -> object basenames referencing it
LOCAL_DEFS = defaultdict(set)  # symbol -> object basenames defining it as static


def nm_symbols(objfiles):
    undefined, defined, common = set(), {}, set()
    out = subprocess.run([os.environ.get("NM", "nm"), "-A"] + objfiles, capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0].endswith(":U") is False and parts[1]:
            # "file: U sym" yields 3 parts normally; handle odd output
            pass
        if len(parts) < 3:
            if len(parts) == 2 and parts[1] and line.split(":")[-1].strip().startswith("U"):
                undefined.add(parts[-1])
            continue
        t, sym = parts[-2], parts[-1]
        if t == "U":
            # Undefined references from the port's own platform code are
            # libc/SDL/GL symbols, never game data.
            if PLATFORM_OBJ.search(parts[0]):
                continue
            undefined.add(sym)
            UNDEF_REFS[sym].add(os.path.basename(parts[0].rstrip(":")).split(".")[0])
        elif t == "C":
            common.add(sym)
        elif t in "TDBRVW":
            defined[sym] = t
        elif t in "dbrt":
            LOCAL_DEFS[sym].add(os.path.basename(parts[0].rstrip(":")).split(".")[0])
    return undefined - set(defined) - common, defined, common


C_NAMED = set()  # addresses of objects the C code refers to by name
OVERRIDE = set()  # addresses of DOL objects copied over C-defined variables at startup


def cname(obj):
    if obj.addr in C_NAMED and re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", obj.name):
        return obj.name
    # only reachable through pointers: use an address-based name so DOL symbol
    # names can never collide with libc/SDL/GL symbols (e.g. 'stat')
    return "dol_%08X" % obj.addr


def bits_get_be(buf, bitoff, width):
    v = int.from_bytes(buf, "big")
    total = len(buf) * 8
    return (v >> (total - bitoff - width)) & ((1 << width) - 1)


def bits_set_le(buf, bitoff, width, value):
    v = int.from_bytes(buf, "little")
    mask = ((1 << width) - 1) << bitoff
    v = (v & ~mask) | ((value << bitoff) & mask)
    buf[:] = v.to_bytes(len(buf), "little")


def resolve_leaves(obj, rawlen, layout, warn):
    """Expand a layout to concrete leaves covering rawlen bytes."""
    if layout is None:
        return None
    size, leaves, elem = layout
    if size is None and leaves and leaves[0][0] == "unbounded":
        _, esz, eleaves = leaves[0]
        n = rawlen // esz if esz else 0
        return [(i * esz + o, s, k, b) for i in range(n) for o, s, k, b in eleaves]
    if size is not None and size < rawlen and elem is not None:
        esz, eleaves = elem
        n = rawlen // esz
        return [(i * esz + o, s, k, b) for i in range(n) for o, s, k, b in eleaves]
    if size is not None and size != rawlen:
        warn.append("%s: C size %d != DOL size %d" % (obj.name, size, rawlen))
    return leaves


def looks_like_string(raw):
    """dtk sometimes emits rodata strings as .4byte words (e.g. the source of an
    inlined strcpy, or several strings merged in one object); those must keep
    their byte order. Accept objects made only of NUL-terminated text runs."""
    if not raw or b"\0" not in raw or raw[0] == 0:
        return False
    # float vectors such as {0.75, 0.75, 0.75} (3F400000 = "?@\0\0") read as
    # text too: words that are all "round" floats of ordinary magnitude are
    # numbers (the buggy race scale constant was left big-endian: invisible
    # opponents)
    if len(raw) % 4 == 0 and all(raw[i + 2] == 0 and raw[i + 3] == 0 and raw[i] != 0 and
                                 1e-6 <= abs(struct.unpack(">f", raw[i:i + 4])[0]) <= 1e7
                                 for i in range(0, len(raw), 4)):
        return False
    pr = sum(1 for c in raw if 0x20 <= c < 0x7F or c in (9, 10, 13))
    if pr + raw.count(0) != len(raw) or pr * 2 < len(raw):
        return False
    return all(len(run) == 0 or len(run) >= 2 for run in raw.split(b"\0")[:-1] if run) and raw[-1] == 0


def text_words(raw, obj):
    """offsets of .4byte words that hold string bytes (dtk merges strings and
    numbers into one object when it cannot tell them apart)"""
    pr = lambda c: 0x20 <= c < 0x7F or c in (9, 10, 13)
    out = set()
    prev_text = False
    for off, w, kind in obj.items:
        if kind != "int" or w != 4 or off + 4 > len(raw):
            prev_text = False
            continue
        b = raw[off:off + 4]
        ok = all(pr(c) or c == 0 for c in b) and any(pr(c) for c in b)
        if ok and b[0] == 0 and not prev_text:
            ok = False
        # a NUL followed by text inside one word is fine ("ab\0c"), but text after
        # trailing NULs of the previous word must start a new string
        if ok:
            out.add(off)
        prev_text = ok and b[3] != 0 or (ok and b[3] == 0 and False)
        if ok and b[3] == 0:
            prev_text = False
    # Floats such as 0.5 (3F000000 = "?\0\0\0"), 2.0, 3.0 ("@@\0\0") or 10.0
    # ("A \0\0") also pass the text test: a run of words that all read as
    # "round" floats of ordinary magnitude is numbers, not a string.
    def round_float(b):
        v = struct.unpack(">f", b)[0]
        return (b[2] == 0 and b[3] == 0 and v == v and 1e-6 <= abs(v) <= 1e7)
    runs, cur = [], []
    for off in sorted(out):
        if cur and off != cur[-1] + 4:
            runs.append(cur)
            cur = []
        cur.append(off)
    if cur:
        runs.append(cur)
    for run in runs:
        if all(round_float(raw[o:o + 4]) for o in run):
            out.difference_update(run)
    return out


WIDTHS = {}  # DOL addr -> integer access widths seen by ppc2c


def convert(obj, raw, leaves, warn):
    """Return native bytes for obj given big-endian raw bytes and typed leaves."""
    if not leaves and obj.section in ("rodata", "sdata2", "data", "sdata") and looks_like_string(raw):
        return bytes(raw)
    out = bytearray(raw)
    textwords = set()
    if not leaves and obj.section in ("rodata", "sdata2", "data", "sdata"):
        textwords = text_words(raw, obj)
    covered = bytearray(len(raw))
    reloc_offs = {o for o, _, _ in obj.relocs}

    def swap(off, sz):
        if off + sz > len(raw):
            return
        out[off:off + sz] = raw[off:off + sz][::-1]
        for i in range(off, off + sz):
            covered[i] = 1

    if leaves:
        bitfield_units = defaultdict(list)
        for off, sz, kind, b in leaves:
            if kind == "bitfield":
                bitfield_units[off].append(b)
                continue
            if off + sz > len(raw):
                continue
            if kind == "ptr":
                if off not in reloc_offs and any(raw[off:off + 4]):
                    warn.append("%s+0x%X: pointer field holds non-relocated 0x%s" % (obj.name, off, raw[off:off+4].hex()))
                for i in range(off, off + 4):
                    covered[i] = 1
                out[off:off + 4] = bytes(4)
                continue
            if sz == 8 and kind == "int":
                # 64-bit integers keep PowerPC word order on the port (see PORT_W64)
                swap(off, 4)
                swap(off + 4, 4)
            elif sz > 1:
                swap(off, sz)
            else:
                covered[off] = 1
        for base, fields in bitfield_units.items():
            # Bitfield structs are declared in reverse under PORT so each storage
            # unit holds the same value as on GameCube: swap whole units.
            for bo, bs, unit in fields:
                uoff = base + (bo // (unit * 8)) * unit
                if unit > 1:
                    swap(uoff, unit)
                else:
                    covered[uoff] = 1
    # anything not described by C types: fall back to the asm directive widths
    for off, w, kind in obj.items:
        if kind == "ptr":
            out[off:off + 4] = bytes(4)
            continue
        if all(covered[off:off + w]):
            continue
        if kind == "int" and w == 4 and off in textwords:
            continue
        if kind in ("int", "float") and w in (2, 4, 8):
            swap(off, w)
    for off, _, _ in obj.relocs:
        out[off:off + 4] = bytes(4)
    return bytes(out)


# ---------------------------------------------------------------------- main


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dol", required=True)
    ap.add_argument("--asm", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--report")
    ap.add_argument("--types", help="file of `regex C-type` hints for DOL objects the C code reaches untyped")
    ap.add_argument("--extra", help="file with extra DOL data addresses to emit as dol_ADDR (ppc2c)")
    ap.add_argument("objs", nargs="+")
    a = ap.parse_args()

    objs = [o for o in a.objs if o.endswith(".o") or o.endswith(".obj")]
    dol = Dol(a.dol)
    asm = parse_asm(a.asm)
    by_addr = {}
    globals_ = {}
    locals_ = defaultdict(dict)
    for o in asm:
        by_addr.setdefault(o.addr, o)
        if o.scope == "global":
            globals_.setdefault(o.name, o)
        locals_[o.file].setdefault(o.name, o)

    undefined, defined, common = nm_symbols(objs)

    needed = []
    seen = set()
    warn = []

    def want(o):
        if o.addr in seen:
            return
        seen.add(o.addr)
        needed.append(o)

    local_by_name = defaultdict(list)
    for o in asm:
        if o.scope != "global":
            local_by_name[o.name].append(o)
    for name in sorted(undefined):
        o = globals_.get(name)
        if o is None and name.startswith("lbl_"):
            o = globals_.get("lbl_" + name[4:].upper())
            if o is not None:
                o.name = name  # emit under the C spelling
        if o is None and local_by_name.get(name):
            # file-local in the original, but the decompiled C refers to it via extern;
            # prefer the copy from the asm unit with the same name as the referencing object
            cands = local_by_name[name]
            pick = [c for c in cands if c.file.split(".")[0] in UNDEF_REFS.get(name, ())]
            if len(cands) == 1 or len(pick) == 1:
                o = (pick or cands)[0]
                o.scope = "global"
            else:
                warn.append("%s: ambiguous file-local symbol (%d candidates)" % (name, len(cands)))
        if o is not None:
            C_NAMED.add(o.addr)
            want(o)
    extra_objs = []
    if a.extra and os.path.exists(a.extra):
        for line in open(a.extra):
            parts = line.split()
            if parts:
                line = parts[0]
                o = by_addr.get(int(line, 16))
                for x in parts[1:]:
                    if x.startswith("w=") and o is not None:
                        WIDTHS[o.addr] = {int(w) for w in x[2:].split(",")}
                if o is None:
                    warn.append("extra address %s not a DOL object" % line)
                else:
                    want(o)
                    extra_objs.append(o)
    # commons that the DOL initialises with non-zero data: override with real contents
    for name in sorted(common - set(defined)):
        o = globals_.get(name)
        if o is None and len(local_by_name.get(name, ())) == 1:
            # file-local in the DOL, made non-static for the port (ppc2c)
            o = local_by_name[name][0]
            o.scope = "global"
        if o is not None and o.section not in ("bss", "sbss"):
            raw = dol.read(o.addr, o.size)
            if any(raw):
                C_NAMED.add(o.addr)
                want(o)
                warn.append("%s: C common overridden with DOL contents" % name)
        elif o is not None:
            pass
    # C-initialised globals: the decompiled initialisers are not always right
    # (e.g. FONT3DSIZE 1.0 vs 0.07 in the DOL). Copy the DOL contents over the C
    # object at startup (port_dol_data_init) when the sizes agree.
    OVERRIDE.clear()
    for name, t in sorted(defined.items()):
        o = globals_.get(name)
        if o is None or o.section in ("bss", "sbss") or t not in "DB":
            continue
        if not any(dol.read(o.addr, o.size)):
            continue
        OVERRIDE.add(o.addr)
        C_NAMED.add(o.addr)
        want(o)

    missing_funcs = set()

    def close_needed():
        i = 0
        while i < len(needed):
            o = needed[i]
            i += 1
            for off, tgt, add in o.relocs:
                t = locals_[o.file].get(tgt) or globals_.get(tgt)
                if t is None:
                    if tgt not in defined and tgt not in common:
                        missing_funcs.add(tgt)
                    continue
                if t.scope == "global" and (t.name in defined or t.name in common):
                    continue
                want(t)
    close_needed()

    hints = []
    follow = set()
    if a.types and os.path.exists(a.types):
        for line in open(a.types):
            line = line.split("#")[0].strip()
            if not line:
                continue
            if line.startswith("@follow "):
                follow.add(line.split(None, 1)[1].strip())
                continue
            pat, tname = line.split(None, 1)
            hints.append((re.compile(pat + "$"), tname.strip()))
    # '@follow NAME': the original code runs off the end of NAME into the
    # object that follows it in the DOL (unterminated lists). Objects are not
    # adjacent in the port, so NAME gets a copy of that neighbour appended.
    for o in needed:
        if o.name not in follow:
            continue
        nb = next((x for x in asm if x.section == o.section and x.addr == o.addr + o.size), None)
        if nb is None:
            warn.append("@follow %s: no adjacent object" % o.name)
            continue
        d = nb.addr - o.addr
        o.relocs.extend((d + off, t, add) for off, t, add in nb.relocs)
        o.items.extend((d + off, w, k) for off, w, k in nb.items)
        o.size += nb.size
        warn.append("%s: followed by a copy of %s (%d bytes)" % (o.name, nb.name, nb.size))
    if follow:
        close_needed()  # the appended neighbours' pointer targets
    types = TypeDB(objs, {o.name for o in needed if o.scope == "global"}, {t for _, t in hints})
    for o in needed:
        if o.scope == "global" and o.name in types.layouts:
            continue
        for pat, tname in hints:
            if pat.match(o.name):
                if tname in types.named:
                    esz, eleaves = types.named[tname]
                    types.hinted[o.addr] = (None, [("unbounded", esz, eleaves)], None)
                else:
                    warn.append("type hint %s for %s: type not found in DWARF" % (tname, o.name))
                break

    # Extend objects whose C declaration is larger than the dtk-inferred size.
    for o in needed:
        if o.scope != "global" or o.name not in types.layouts:
            continue
        csize = types.layouts[o.name][0]
        if csize and csize > o.size and o.section not in ("bss", "sbss"):
            for nb in asm:
                if o.addr < nb.addr < o.addr + csize and nb.section == o.section:
                    d = nb.addr - o.addr
                    # only what falls inside the C object (the neighbour's tail
                    # stays with the neighbour, e.g. time_string[64] vs months)
                    o.relocs.extend((d + off, t, add) for off, t, add in nb.relocs if d + off + 4 <= csize)
                    o.items.extend((d + off, w, k) for off, w, k in nb.items if d + off < csize)
            warn.append("%s: extended from DOL size %d to C size %d" % (o.name, o.size, csize))
            o.size = csize
        elif csize and csize > o.size:
            o.size = csize

    def target_of(o, tgt):
        return locals_[o.file].get(tgt) or globals_.get(tgt)

    # Type inference through typed pointer fields (BFS order: referrers first).
    inferred = {}
    leaves_of = {}
    for o in needed:
        if o.section in ("bss", "sbss"):
            continue
        layout = types.layouts.get(o.name) if o.scope == "global" else None
        if layout is None:
            layout = types.hinted.get(o.addr)
        if layout is None:
            layout = inferred.get(o.addr)
        if layout is None and o.addr in WIDTHS and o.section not in ("bss", "sbss"):
            ws = WIDTHS[o.addr]
            relo = {off for off, _, _ in o.relocs}
            if ws == {2}:
                items = []
                for off, w, kind in o.items:
                    if kind == "int" and w == 4 and off not in relo:
                        items += [(off, 2, "int"), (off + 2, 2, "int")]
                    else:
                        items.append((off, w, kind))
                o.items = items
                warn.append("%s: untyped, accessed as 16-bit: swapped per halfword" % o.name)
            elif ws == {1}:
                o.items = [(off, w, kind) for off, w, kind in o.items if kind != "int"]
                warn.append("%s: untyped, accessed as bytes: kept in memory order" % o.name)
            elif ws - {4}:
                warn.append("%s: untyped, mixed access widths %s" % (o.name, sorted(ws)))
        lv = resolve_leaves(o, o.size, layout, warn)
        leaves_of[o.addr] = lv
        if not lv:
            continue
        rel = {off: tgt for off, tgt, _ in o.relocs}
        for off, sz, kind, b in lv:
            if kind == "ptr" and b is not None and off in rel:
                t = target_of(o, rel[off])
                if t is None or t.addr in inferred:
                    continue
                if t.scope == "global" and t.name in types.layouts:
                    continue
                pl = types.pointee_layout(b)
                if pl is not None:
                    inferred[t.addr] = pl

    # dtk takes any word that looks like a DOL address for a pointer, even
    # inside byte lists (GoldRushModelList holds the character ids 80 21 89 4B).
    # Where the C type says the bytes are small scalars, keep them as data.
    for o in needed:
        lv = leaves_of.get(o.addr)
        if not lv or not o.relocs:
            continue
        small = set()
        ptrs = set()
        for off, sz, kind, b in lv:
            if kind == "ptr":
                ptrs.add(off)
            elif kind in ("int", "float") and 0 < sz < 4:
                small.update(range(off, off + sz))
        keep = []
        dropped = set()
        for r in o.relocs:
            off = r[0]
            if off not in ptrs and any(i in small for i in range(off, off + 4)):
                warn.append("%s+0x%X: dtk reloc to %s dropped (C type: byte/halfword data)" % (o.name, off, r[1]))
                dropped.add(off)
                continue
            keep.append(r)
        o.relocs = keep
        # the asm item for that word still says "ptr", which convert() zeroes:
        # keep the original bytes (the C leaves say how to order them)
        if dropped:
            o.items = [(i, w, "bytes" if (k == "ptr" and i in dropped) else k) for i, w, k in o.items]

    lines = ["/* GENERATED by port/tools/gen_dol_data.py from the user's own main.dol.",
             " * Contains game data: never commit or redistribute. */",
             "#include <stdint.h>", ""]
    decl = set()
    relocs = []
    overrides = []
    ext_override = set()
    total = 0
    untyped = []
    for o in needed:
        cn = cname(o)
        if o.section in ("bss", "sbss"):
            body = None
        else:
            raw = dol.read(o.addr, o.size)
            lv = leaves_of.get(o.addr)
            if not lv:
                untyped.append(cn)
            body = convert(o, raw, lv, warn)
            if body is not None and len(body) > o.size:
                warn.append("%s: converted body %d > size %d, truncated" % (o.name, len(body), o.size))
                body = body[:o.size]
        total += o.size
        attr = "__attribute__((aligned(32)))"
        if o.addr in OVERRIDE:
            csize = types.layouts.get(o.name, (None,))[0]
            if body is None or csize != o.size:
                warn.append("%s: not overridden (C size %s, DOL size %d)" % (o.name, csize, o.size))
                continue
            hexs = ",".join(str(b) for b in body)
            lines.append("static const unsigned char dolinit_%s[%d] = {%s};" % (cn, o.size, hexs))
            overrides.append((cn, o.size))
            ext_override.add(cn)
            for off, tgt, add in o.relocs:
                t = target_of(o, tgt)
                if t is not None and not (t.scope == "global" and (t.name in defined or t.name in common)):
                    tn = cname(t)
                else:
                    tn = tgt
                relocs.append((cn, off, tn, add))
            continue
        decl.add(cn)
        if body is None:
            lines.append("%s unsigned char %s[%d];" % (attr, cn, max(o.size, 1)))
        else:
            hexs = ",".join(str(b) for b in body)
            lines.append("%s unsigned char %s[%d] = {%s};" % (attr, cn, max(o.size, 1), hexs))
        for off, tgt, add in o.relocs:
            t = target_of(o, tgt)
            if t is not None and not (t.scope == "global" and (t.name in defined or t.name in common)):
                tn = cname(t)
            else:
                tn = tgt
            relocs.append((cn, off, tn, add))

    externs = sorted({tn for _, _, tn, _ in relocs if tn not in decl} | ext_override)
    # Targets with no C definition yet (asm-only functions) are weak: they
    # resolve to NULL until reimplemented, and are listed in the report.
    ext_lines = ["extern unsigned char %s[]%s;" % (n, " __attribute__((weak))" if n in missing_funcs else "")
                 for n in externs]
    lines[4:4] = ext_lines + [""]
    # translated code (ppc2c) refers to DOL objects as dol_ADDR
    for o in extra_objs:
        cn = cname(o)
        if cn != "dol_%08X" % o.addr:
            if cn in decl:
                lines.append('extern unsigned char dol_%08X[%d] __attribute__((alias("%s")));' % (o.addr, max(o.size, 1), cn))
            else:
                warn.append("extra object %s defined outside the generated file" % cn)
    lines.append("")
    lines.append("struct port_reloc { unsigned char *where; unsigned char *target; int addend; };")
    lines.append("static const struct port_reloc port_relocs[] = {")
    for cn, off, tn, add in relocs:
        lines.append("  {%s + %d, %s, %d}," % (cn, off, tn, add))
    lines.append("  {0, 0, 0}};")
    lines.append("")
    lines.append("struct port_override { unsigned char *where; const unsigned char *init; int size; };")
    lines.append("static const struct port_override port_overrides[] = {")
    for cn, n in overrides:
        lines.append("  {%s, dolinit_%s, %d}," % (cn, cn, n))
    lines.append("  {0, 0, 0}};")
    lines.append("")
    lines.append("void port_dol_data_init(void) {")
    lines.append("  const struct port_reloc *r;")
    lines.append("  const struct port_override *ov;")
    lines.append("  for (ov = port_overrides; ov->where; ov++) __builtin_memcpy(ov->where, ov->init, ov->size);")
    lines.append("  for (r = port_relocs; r->where; r++) {")
    lines.append("    uintptr_t v = (uintptr_t)r->target + r->addend;")
    lines.append("    __builtin_memcpy(r->where, &v, sizeof(v));")
    lines.append("  }")
    lines.append("}")
    text = "\n".join(lines) + "\n"
    try:
        same = open(a.out).read() == text
    except OSError:
        same = False
    if not same:  # keep the mtime when unchanged (avoids recompiling)
        open(a.out, "w").write(text)

    if a.report:
        with open(a.report, "w") as f:
            f.write("objects: %d bytes: %d relocs: %d\n" % (len(needed), total, len(relocs)))
            f.write("typed: %d\n" % len(types.layouts))
            f.write("untyped objects (directive-width fallback): %d\n%s\n" % (len(untyped), " ".join(untyped)))
            f.write("inferred through pointers: %d\n" % len(inferred))
            f.write("unresolved reloc targets (need C definitions): %s\n" % " ".join(sorted(missing_funcs)))
            st = sorted(t for t in missing_funcs if t in LOCAL_DEFS)
            f.write("reloc targets that are static in C (remove 'static' under PORT): %s\n" % " ".join(st))
            f.write("C-initialised globals overwritten with DOL contents: %d\n%s\n" % (len(overrides), " ".join(c for c, _ in overrides)))
            for w in warn:
                f.write("warn: %s\n" % w)
    print("gen_dol_data: %d objects, %d relocs, %d typed, %d warnings" % (len(needed), len(relocs), len(types.layouts), len(warn)))


if __name__ == "__main__":
    main()
