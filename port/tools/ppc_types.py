"""Register type inference for ppc2c: tracks which registers hold pointers to
known C types (from the DWARF of the port objects) so that narrow (byte /
halfword) accesses into wider scalars can be remapped to little-endian offsets.

GCC 2.95 for PowerPC often reads only the low byte of an int field
(`lbz r7, 0x53(r5)` for an int at 0x50). On the big-endian GameCube that is
the least significant byte; in the port's little-endian memory it is at 0x50.
"""
import re

from elftools.elf.elffile import ELFFile
import elf_compat  # noqa: E402,F401  (tolerate unknown relocation types)

STRIP = ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type", "DW_TAG_restrict_type")


def strip(d):
    while d is not None and d.tag in STRIP:
        d = d.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in d.attributes else None
    return d


def tgt(d):
    return d.get_DIE_from_attribute("DW_AT_type") if d is not None and "DW_AT_type" in d.attributes else None


def size_of(t):
    t = strip(t)
    if t is None:
        return None
    if "DW_AT_byte_size" in t.attributes:
        return t.attributes["DW_AT_byte_size"].value
    if t.tag == "DW_TAG_pointer_type":
        return 4
    if t.tag == "DW_TAG_array_type":
        e = size_of(tgt(t))
        n = array_count(t)
        return e * n if e is not None and n is not None else None
    return None


def array_count(t):
    count = None
    for ch in t.iter_children():
        if ch.tag == "DW_TAG_subrange_type":
            if "DW_AT_upper_bound" in ch.attributes:
                c = ch.attributes["DW_AT_upper_bound"].value + 1
            elif "DW_AT_count" in ch.attributes:
                c = ch.attributes["DW_AT_count"].value
            else:
                return None
            count = c if count is None else count * c
    return count


def find(t, off, depth=0):
    """innermost leaf containing byte `off` of an object of type t:
    (leaf_off, leaf_size, kind, leaf_die) with kind 'scalar' | 'ptr', or None"""
    t = strip(t)
    if t is None or off < 0 or depth > 16:
        return None
    tag = t.tag
    if tag in ("DW_TAG_base_type", "DW_TAG_enumeration_type"):
        sz = size_of(t)
        return (0, sz, "scalar", t) if sz and off < sz else None
    if tag == "DW_TAG_pointer_type":
        return (0, 4, "ptr", t) if off < 4 else None
    if tag == "DW_TAG_array_type":
        e = tgt(t)
        esz = size_of(e)
        if not esz:
            return None
        i = off // esz
        n = array_count(t)
        if n is not None and i >= n:
            return None
        r = find(e, off - i * esz, depth + 1)
        return (r[0] + i * esz,) + r[1:] if r else None
    if tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
        for m in t.iter_children():
            if m.tag != "DW_TAG_member":
                continue
            mt = tgt(m)
            loc = m.attributes.get("DW_AT_data_member_location")
            if "DW_AT_bit_size" in m.attributes:
                unit = size_of(mt) or 4
                if "DW_AT_data_bit_offset" in m.attributes:
                    bo = m.attributes["DW_AT_data_bit_offset"].value
                    moff = (bo // (unit * 8)) * unit
                else:
                    moff = loc.value if loc is not None else 0
                if moff <= off < moff + unit:
                    return (moff, unit, "scalar", strip(mt))
                continue
            moff = loc.value if loc is not None else 0
            msz = size_of(mt)
            if msz is None:
                continue
            if moff <= off < moff + msz:
                r = find(mt, off - moff, depth + 1)
                return (r[0] + moff,) + r[1:] if r else None
        return None
    return None


def element_container(t, off):
    """for pointer arithmetic with an unknown index: the innermost array that
    contains `off`, returned as (element_type, offset within element)"""
    t = strip(t)
    base = 0
    best = None
    for _ in range(16):
        if t is None:
            break
        if t.tag == "DW_TAG_array_type":
            e = tgt(t)
            esz = size_of(e)
            if not esz:
                break
            rel = off - base
            best = (strip(e), rel % esz)
            t = e
            base += (rel // esz) * esz
            continue
        if t.tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            nxt = None
            for m in t.iter_children():
                if m.tag != "DW_TAG_member" or "DW_AT_bit_size" in m.attributes:
                    continue
                loc = m.attributes.get("DW_AT_data_member_location")
                moff = loc.value if loc is not None else 0
                msz = size_of(tgt(m))
                if msz and base + moff <= off < base + moff + msz:
                    nxt = (strip(tgt(m)), base + moff)
                    break
            if nxt is None:
                break
            t, base = nxt
            continue
        break
    return best


class TypeInfo:
    def __init__(self, objs):
        self.vars = {}     # global variable name -> type DIE
        self.params = {}   # function name -> [type DIE or None]
        self.ret = {}      # function name -> type DIE or None
        for path in objs:
            f = open(path, "rb")
            elf = ELFFile(f)
            if not elf.has_dwarf_info():
                continue
            for cu in elf.get_dwarf_info().iter_CUs():
                top = cu.get_top_DIE()
                for die in top.iter_children():
                    na = die.attributes.get("DW_AT_name")
                    if not na:
                        continue
                    name = na.value.decode()
                    if die.tag == "DW_TAG_variable":
                        if name not in self.vars and "DW_AT_type" in die.attributes:
                            t = strip(tgt(die))
                            if t is not None and (t.tag != "DW_TAG_array_type" or size_of(t)):
                                self.vars[name] = tgt(die)
                            elif t is not None:
                                self.vars.setdefault(name, tgt(die))
                    elif die.tag == "DW_TAG_subprogram":
                        if "DW_AT_declaration" in die.attributes and name in self.params:
                            continue
                        ps = [tgt(ch) for ch in die.iter_children() if ch.tag == "DW_TAG_formal_parameter"]
                        if name not in self.params or "DW_AT_declaration" not in die.attributes:
                            self.params[name] = ps
                            self.ret[name] = tgt(die)
            # keep the file open: DIEs are parsed lazily


# --------------------------------------------------------------- inference

GPR = re.compile(r"^r(\d+)$")
SYMOP = re.compile(r"^(?P<sym>[^@]+?)(?:\+(?P<add>0x[0-9A-Fa-f]+|\d+))?@(?P<rel>ha|h|l|sda21)$")
NOWRITE = ("st", "cmp", "b", "mt", "cr", "mcrf", "f", "nop", "dcb", "icb", "sync", "isync", "tw")


def _int(s):
    try:
        return int(s, 0)
    except ValueError:
        return None


def _ptr_param(t):
    t = strip(t)
    if t is not None and t.tag == "DW_TAG_pointer_type":
        p = strip(tgt(t))
        return ("P", p, 0) if p is not None else None
    if t is not None and t.tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
        return ("P", t, 0)  # passed by reference in the PPC ABI
    return None


class Inference:
    """Forward dataflow over a function's basic blocks. Register values:
       ("P", type_die, off) pointer to an object of type at byte offset
       ("HA", sym, add)      upper half of a symbol address (lis sym@ha)
    """

    def __init__(self, ti, fn, symtype):
        self.ti = ti
        self.fn = fn
        self.symtype = symtype  # callable(sym) -> type DIE of the data symbol or None
        self.state_at = {}

    def run(self):
        f = self.fn
        insns = f.insns
        n = len(insns)
        idx_of = {a: i for i, (a, _, _) in enumerate(insns)}
        # basic block leaders
        leaders = {0}
        for i, (a, mn, ops) in enumerate(insns):
            if a in f.labels:
                leaders.add(i)
            if mn.startswith("b") and mn not in ("bl",):
                leaders.add(i + 1)
        leaders = sorted(x for x in leaders if x < n)
        block_of = {}
        blocks = []
        for k, s in enumerate(leaders):
            e = leaders[k + 1] if k + 1 < len(leaders) else n
            blocks.append((s, e))
            block_of[s] = k
        entry = {}
        ps = self.ti.params.get(f.name, [])
        gi = 3
        for t in ps:
            st = strip(t)
            if st is not None and st.tag == "DW_TAG_base_type" and st.attributes["DW_AT_encoding"].value == 4:
                continue  # float param in FPR
            if gi > 10:
                break
            v = _ptr_param(t)
            if v:
                entry["r%d" % gi] = v
            gi += 1
        IN = {0: entry}
        work = [0]
        iters = 0
        while work and iters < 20000:
            iters += 1
            b = work.pop()
            s, e = blocks[b]
            st = dict(IN[b])
            for i in range(s, e):
                self.state_at[i] = st
                st = self.step(st, insns[i])
            # successors
            a, mn, ops = insns[e - 1]
            succ = []
            mn0 = mn.rstrip("+-")
            tgtlab = None
            m = re.search(r"\.L_([0-9A-F]{8})", ops)
            if m:
                tgtlab = idx_of.get(int(m.group(1), 16))
            uncond = mn0 in ("b", "blr", "bctr") or (mn0 == "b" and tgtlab is None)
            if tgtlab is not None:
                succ.append(tgtlab)
            if not uncond and e < n:
                succ.append(e)
            for si in succ:
                if si not in block_of:
                    continue
                sb = block_of[si]
                if sb not in IN:
                    IN[sb] = dict(st)
                    work.append(sb)
                else:
                    old = IN[sb]
                    new = {k: v for k, v in old.items() if st.get(k) == v}
                    if len(new) != len(old):
                        IN[sb] = new
                        work.append(sb)
        return self

    def step(self, st, insn):
        a, mn, ops = insn
        o = [x.strip() for x in ops.split(",")] if ops else []
        base = mn.rstrip(".")
        ns = None

        def w(r, v):
            nonlocal ns
            if ns is None:
                ns = dict(st)
            if v is None:
                ns.pop(r, None)
            else:
                ns[r] = v

        if base == "bl" or base == "blrl" or base == "bctrl":
            nsd = {k: v for k, v in st.items() if not (k.startswith("r") and k[1:].isdigit() and
                                                       (int(k[1:]) == 0 or 3 <= int(k[1:]) <= 12))}
            if base == "bl":
                rt = self.ti.ret.get(o[0]) if o else None
                v = _ptr_param(rt)
                if v:
                    nsd["r3"] = v
            return nsd
        if base == "lis" and o:
            m = SYMOP.match(o[1])
            w(o[0], ("HA", m.group("sym"), _int(m.group("add") or "0")) if m and m.group("rel") == "ha" else None)
            return ns or st
        if base in ("addi", "li") and o:
            src = o[1] if base == "addi" else None
            imm = o[2] if base == "addi" else o[1]
            m = SYMOP.match(imm)
            if m:
                add = _int(m.group("add") or "0")
                t = self.symtype(m.group("sym"))
                w(o[0], ("P", t, add) if t is not None else None)
                return ns or st
            v = st.get(src) if src else None
            iv = _int(imm)
            if v and v[0] == "P" and iv is not None:
                w(o[0], ("P", v[1], v[2] + iv))
            else:
                w(o[0], None)
            return ns or st
        if base == "subi" and len(o) == 3:
            v = st.get(o[1])
            iv = _int(o[2])
            w(o[0], ("P", v[1], v[2] - iv) if v and v[0] == "P" and iv is not None else None)
            return ns or st
        if base == "mr" and len(o) == 2:
            w(o[0], st.get(o[1]))
            return ns or st
        if base == "add" and len(o) == 3:
            va, vb = st.get(o[1]), st.get(o[2])
            v = va if (va and va[0] == "P") else vb if (vb and vb[0] == "P") else None
            if v and not (va and vb and va[0] == vb[0] == "P"):
                ec = element_container(v[1], v[2])
                # no enclosing array: p + i * sizeof(*p)
                w(o[0], ("P", ec[0], ec[1]) if ec else v)
            else:
                w(o[0], None)
            return ns or st
        m = re.match(r"^(lwz|lbz|lhz|lha|lfs|lfd|stw|stb|sth|stfs|stfd)(u?)(x?)$", base)
        if m:
            op, upd, idx = m.groups()
            if idx:
                if op.startswith("l") and op not in ("lfs", "lfd"):
                    w(o[0], None)
                if upd:
                    w(o[1], None)
                return ns or st
            mm = re.match(r"^(.+)\((r\d+)\)$", o[1])
            if not mm:
                return st
            disp, rb = mm.group(1), mm.group(2)
            sm = SYMOP.match(disp)
            container = None
            if sm:
                t = self.symtype(sm.group("sym"))
                if t is not None:
                    container = (t, _int(sm.group("add") or "0"))
            else:
                v = st.get(rb)
                dv = _int(disp)
                if rb == "r1" and dv is not None:
                    # stack slots
                    if op == "stw":
                        w("s%d" % dv, st.get(o[0]))
                    elif op == "lwz":
                        w(o[0], st.get("s%d" % dv))
                        return ns or st
                    elif op.startswith("st"):
                        w("s%d" % dv, None)
                if v and v[0] == "P" and dv is not None:
                    container = (v[1], v[2] + dv)
                if upd:
                    w(rb, ("P", v[1], v[2] + dv) if v and v[0] == "P" and dv is not None else None)
            if op == "lwz":
                val = None
                if container is not None:
                    r = find(container[0], container[1])
                    if r and r[2] == "ptr":
                        p = strip(tgt(r[3]))
                        val = ("P", p, 0) if p is not None else None
                w(o[0], val)
            elif op in ("lbz", "lhz", "lha"):
                w(o[0], None)
            return ns or st
        if base in ("lmw",):
            r0 = int(o[0][1:])
            for k in range(r0, 32):
                w("r%d" % k, None)
            return ns or st
        # generic: first operand is a written GPR?
        if o and GPR.match(o[0]) and not base.startswith(NOWRITE):
            w(o[0], None)
        return ns or st

    def container(self, i, rb, disp):
        """(type, offset) of the object accessed by D-form `disp(rb)` at insn i"""
        st = self.state_at.get(i)
        if not st:
            return None
        v = st.get(rb)
        if v and v[0] == "P" and v[1] is not None:
            return (v[1], v[2] + disp)
        return None
