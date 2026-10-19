"""Register packaged native dependencies before importing the extension."""

import os
import sys
from pathlib import Path

# Python 3.8+ resolves extension dependencies through explicit DLL directories.
# Keep these handles alive for later plugin loads as well as the first import.
_directories = []
if sys.platform == "win32":
    _package_root = Path(__file__).resolve().parent.parent
    for _directory in (_package_root / "bin", _package_root / "lib"):
        if _directory.is_dir():
            _directories.append(os.add_dll_directory(str(_directory)))
