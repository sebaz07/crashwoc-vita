#!/usr/bin/env python3
"""Print struct layouts (offsets, sizes, bitfields) from DWARF of an object file.
usage: dump_struct.py OBJ NAME [NAME...]"""
import sys
from elftools.elf.elffile import ELFFile

def strip(d):
    while d is not None and d.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
        if "DW_AT_type" not in d.attributes:
            return None
        d = d.get_DIE_from_attribute("DW_AT_type")
    return d

def tname(d):
    if d is None:
        return "void"
    n = d.attributes.get("DW_AT_name")
    nm = n.value.decode() if n else ""
    if d.tag == "DW_TAG_pointer_type":
        return tname(d.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in d.attributes else None) + "*"
    if d.tag == "DW_TAG_array_type":
        return tname(d.get_DIE_from_attribute("DW_AT_type")) + "[]"
    if d.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type") and not nm:
        return tname(d.get_DIE_from_attribute("DW_AT_type"))
    return nm or d.tag

obj, names = sys.argv[1], set(sys.argv[2:])
seen = set()
e = ELFFile(open(obj, "rb"))
for cu in e.get_dwarf_info().iter_CUs():
    for d in cu.iter_DIEs():
        if d.tag not in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            continue
        n = d.attributes.get("DW_AT_name")
        if not n or n.value.decode() not in names or n.value.decode() in seen or "DW_AT_byte_size" not in d.attributes:
            continue
        seen.add(n.value.decode())
        print("%s %s size=0x%x" % (d.tag[7:-5], n.value.decode(), d.attributes["DW_AT_byte_size"].value))
        for m in d.iter_children():
            if m.tag != "DW_TAG_member":
                continue
            mt = m.get_DIE_from_attribute("DW_AT_type")
            st = strip(mt)
            loc = m.attributes.get("DW_AT_data_member_location")
            off = loc.value if loc else 0
            sz = st.attributes["DW_AT_byte_size"].value if st is not None and "DW_AT_byte_size" in st.attributes else "?"
            bits = ""
            if "DW_AT_bit_size" in m.attributes:
                bo = m.attributes.get("DW_AT_data_bit_offset")
                bits = " bits=%d@%d" % (m.attributes["DW_AT_bit_size"].value, bo.value if bo else -1)
                off = bo.value // 8 if bo else off
            mn = m.attributes.get("DW_AT_name")
            print("  +0x%02x %-24s %s size=%s%s" % (off, mn.value.decode() if mn else "?", tname(mt), sz, bits))
