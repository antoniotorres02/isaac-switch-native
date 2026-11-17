"""Isaac Switch Builder: from your own copy of the game to an SD card folder.

A friendly front for build_all.py, which runs inside the pinned Docker image
(docker/Dockerfile). This package only finds the inputs, checks them, starts
the container and shows its progress; the build itself is build_all.py.
"""
import sys
from pathlib import Path

VERSION = "0.1.0"

# build_all.py lives next to builder/ in the source tree; PyInstaller bundles it
# as a top-level module, so this only matters when running from the source.
_HOST = Path(__file__).resolve().parent.parent.parent   # builder/isb -> the host folder
if (_HOST / "build_all.py").is_file() and str(_HOST) not in sys.path:
    sys.path.insert(0, str(_HOST))
