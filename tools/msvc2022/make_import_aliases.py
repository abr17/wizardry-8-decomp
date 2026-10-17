"""Write an import archive whose caller symbols differ from the DLL export names.

msvc2022 port helper. Every alias becomes one member built by the decomp's own
stdlib-only tools/wiz8decomp/binary/coff_archive.py::named_iat_archive, the
same routine that produces sr-assert-import.lib: the member defines only
``__imp_<caller>`` (an IAT slot, no code thunk) and makes the PE import the
DLL's ``<provider>`` by name. LIB then combines the archive with an ordinary
DEF-generated library of the same DLL, which supplies the import descriptor
and the terminators.

Usage:
    python -I make_import_aliases.py <output.lib> <dll> <caller>=<provider> ...

``caller`` is the full decorated symbol after ``__imp_`` (for a C function
``foo`` that is ``_foo``).
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ARCHIVE = HERE.parent / "wiz8decomp" / "binary" / "coff_archive.py"


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    dll = sys.argv[2]
    spec = importlib.util.spec_from_file_location("wiz8_coff_archive", ARCHIVE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    magic = module.ARCHIVE_MAGIC
    payload = bytearray(magic)
    for alias in sys.argv[3:]:
        caller, provider = alias.split("=", 1)
        member = module.named_iat_archive(dll, caller, provider)
        assert member.startswith(magic)
        payload.extend(member[len(magic):])
    payload = bytes(payload)
    if not out.is_file() or out.read_bytes() != payload:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
