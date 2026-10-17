"""Write sr-assert-import.lib for the msvc2022 port build.

Same payload as tools/wiz8decomp/build.py::_ensure_sr_assert_import, without
the rest of the wiz8decomp package (which needs third-party modules). Loads
coff_archive.py directly by path, stdlib only. Usage:
    python -I make_sr_assert_import.py <output.lib>
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ARCHIVE = HERE.parent / "wiz8decomp" / "binary" / "coff_archive.py"


def main() -> int:
    out = Path(sys.argv[1])
    spec = importlib.util.spec_from_file_location("wiz8_coff_archive", ARCHIVE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    payload = module.named_iat_archive(
        "SR.dll", "?srAssertFail@@YAXPBD0J0@Z", "?srAssertFail@@YAXPBD0J0ZZ"
    )
    if not out.is_file() or out.read_bytes() != payload:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
