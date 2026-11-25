#!/usr/bin/env python3
"""Builds the program as a single file with PyInstaller, for this system.

    pip install pyinstaller==6.16.0
    python package.py [--out DIR]

Linux gets one binary, isaac-switch-builder-linux (window, or command line with
arguments). Windows gets
two: isaac-switch-builder.exe opens the window without a console, and
isaac-switch-builder-cli.exe is the same program for a terminal.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
HOST = HERE.parent


def pyinstaller(name: str, out: Path, windowed: bool) -> Path:
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [sys.executable, "-m", "PyInstaller", "--onefile", "--clean", "--noconfirm", "--name", name,
               "--paths", str(HOST), "--hidden-import", "build_all", "--hidden-import", "versions",
               "--add-data", "%s%s." % (HOST / "upstream.json", os.pathsep),
               # Only version.json: the overlay and the function list are used inside the image.
               *[arg for v in sorted((HOST / "versions").glob("*/version.json"))
                 for arg in ("--add-data", "%s%sversions/%s" % (v, os.pathsep, v.parent.name))],
               "--add-data", "%s%sisb" % (HERE / "isb" / "image.json", os.pathsep),
               "--distpath", str(out), "--workpath", tmp, "--specpath", tmp]
        if windowed:
            cmd.append("--windowed")
        cmd.append(str(HERE / "isaac_switch_builder.py"))
        subprocess.run(cmd, check=True)
    return out / (name + (".exe" if sys.platform == "win32" else ""))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=HERE / "dist")
    args = ap.parse_args()
    win = sys.platform == "win32"
    made = [pyinstaller("isaac-switch-builder" if win else "isaac-switch-builder-linux", args.out, windowed=win)]
    if sys.platform == "win32":
        made.append(pyinstaller("isaac-switch-builder-cli", args.out, windowed=False))
    for path in made:
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
