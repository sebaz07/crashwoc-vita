"""pyelftools: ignore relocation types it does not know (sanitizer builds emit
R_386_GOTOFF etc. in code sections that end up referenced from DWARF)."""
from elftools.elf import relocation as _rel
from elftools.common.exceptions import ELFRelocationError

_orig = _rel.RelocationHandler._do_apply_relocation


def _tolerant(self, stream, reloc, symtab):
    try:
        return _orig(self, stream, reloc, symtab)
    except ELFRelocationError:
        return None


_rel.RelocationHandler._do_apply_relocation = _tolerant
