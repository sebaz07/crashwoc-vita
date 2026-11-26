#!/usr/bin/env python3
"""Rebuild main.dol from the crashwoc.elf that ships on the USA disc.

The GCBE7D disc carries an unstripped copy of the game executable as
files/crashwoc.elf. Converting its LOAD segments the way the original tool did
(one DOL section per non-empty segment, in program-header order, each size
rounded up to 32 bytes, packed back to back after the 0x100-byte header, BSS
spanning every NOBITS segment) gives a byte-identical main.dol, so a build only
needs the extracted files/ tree.

Usage: scripts/elf2dol.py <files/crashwoc.elf> <decomp/orig/GCBE7D/sys/main.dol>
"""
import hashlib
import struct
import sys

WANT_SHA1 = "c9cbd49a9eb0006f55533eb7d0fb5ebe2a73b72f"


def elf_to_dol(elf: bytes) -> bytes:
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 2:
        raise ValueError("not a 32-bit big-endian ELF")
    entry, phoff = struct.unpack_from(">II", elf, 0x18)
    phentsize, phnum = struct.unpack_from(">HH", elf, 0x2A)
    segs = []
    for i in range(phnum):
        p_type, off, vaddr, _paddr, filesz, memsz, flags, _align = struct.unpack_from(
            ">8I", elf, phoff + i * phentsize)
        if p_type == 1:  # PT_LOAD
            segs.append((off, vaddr, filesz, memsz, flags))
    text = [s for s in segs if s[2] and s[4] & 1]
    data = [s for s in segs if s[2] and not s[4] & 1]
    bss = [s for s in segs if s[3] > s[2]]
    if len(text) > 7 or len(data) > 11:
        raise ValueError("too many sections for a DOL")
    hdr = bytearray(0x100)
    body = bytearray()
    pos = 0x100

    def put(s, slot, off_tab, addr_tab, size_tab):
        nonlocal pos
        size = (s[2] + 31) & ~31
        struct.pack_into(">I", hdr, off_tab + 4 * slot, pos)
        struct.pack_into(">I", hdr, addr_tab + 4 * slot, s[1])
        struct.pack_into(">I", hdr, size_tab + 4 * slot, size)
        body.extend(elf[s[0]:s[0] + s[2]])
        body.extend(b"\0" * (size - s[2]))
        pos += size

    for i, s in enumerate(text):
        put(s, i, 0x00, 0x48, 0x90)
    for i, s in enumerate(data):
        put(s, i, 0x1C, 0x64, 0xAC)
    lo = min(s[1] + s[2] for s in bss)
    hi = max(s[1] + s[3] for s in bss)
    struct.pack_into(">III", hdr, 0xD8, lo, hi - lo, entry)
    return bytes(hdr) + bytes(body)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as f:
        dol = elf_to_dol(f.read())
    sha1 = hashlib.sha1(dol).hexdigest()
    if sha1 != WANT_SHA1:
        print(f"SHA-1 {sha1} is not the GCBE7D main.dol ({WANT_SHA1}); not written", file=sys.stderr)
        return 1
    with open(sys.argv[2], "wb") as f:
        f.write(dol)
    print(f"{sys.argv[2]}: {len(dol)} bytes, SHA-1 {sha1} (GCBE7D)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
