#!/usr/bin/env python3
"""Builds the installer NRO (installer/source) with devkitA64.

    build.py --out DIR      ->  DIR/isaac-recomp-installer.nro

Needs DEVKITPRO (the builder image and the devkitA64 image set it).
"""
from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCES = ["main.c", "nca_install.c", "switch_crypto.c"]
NAME = "isaac-recomp-installer"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    dkp = os.environ.get("DEVKITPRO")
    if not dkp:
        sys.exit("DEVKITPRO is not set: run this in the builder or devkitA64 image")
    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)
    cc = dkp + "/devkitA64/bin/aarch64-none-elf-gcc"
    arch = ["-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE"]
    objs = []
    for s in SOURCES:
        o = out / (Path(s).stem + ".o")
        r = subprocess.run([cc, "-c", "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-ffunction-sections",
                            *arch, "-D__SWITCH__", "-I", dkp + "/libnx/include", "-o", str(o),
                            str(HERE / "source" / s)], capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stderr)
        objs.append(str(o))
    elf = out / (NAME + ".elf")
    r = subprocess.run([cc, "-specs=" + dkp + "/libnx/switch.specs", "-g", *arch, "-o", str(elf), *objs,
                        "-L", dkp + "/libnx/lib", "-lnx"], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr)
    nacp, nro = out / (NAME + ".nacp"), out / (NAME + ".nro")
    for cmd in ([dkp + "/tools/bin/nacptool", "--create", "ISAAC RECOMP INSTALLER", "isaacport", "0.1.0", str(nacp)],
                [dkp + "/tools/bin/elf2nro", str(elf), str(nro), "--nacp=" + str(nacp)]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            sys.exit(r.stdout + r.stderr)
    print("installer: %s (%.1f KB), sha256 %s" % (nro, nro.stat().st_size / 1024,
                                                   hashlib.sha256(nro.read_bytes()).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
