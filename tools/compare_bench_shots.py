#!/usr/bin/env python3
"""Run the PC benchmark scenarios with a fixed epoch and compare their
screenshots with a reference set: the oracle for performance work, since
with ISAAC_EPOCH fixed the deterministic clock makes every run's frames
bit-identical. The footstep readback runs synchronously (it is async in
play, and its timing nudges the player tint), and each run writes to its own
directory so concurrent benches do not mix.

    python3 tools/compare_bench_shots.py --exe build/<out>/gl/isaac_native [--save-ref]
"""
import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REF = ROOT / "build/perf-baseline/shots.json"
SCENARIOS = ["horde:3b", "swarm:5c", "tears:1"]
SHOT = 2600


def run(exe: str, scenario: str) -> tuple[str, dict]:
    out = tempfile.mkdtemp(prefix="shots-", dir=ROOT / "build")
    env = dict(os.environ, ISAAC_EPOCH="1700000000", ISAAC_READPIXELS_SYNC="1", ISAAC_BENCH_OUT=out)
    subprocess.run([sys.executable, str(ROOT / "tools/bench_native.py"), "pc", scenario, "--shot", str(SHOT),
                    "--exe", exe], env=env, check=False, capture_output=True)
    new = sorted(glob.glob(out + "/pc-*"))
    if not new:
        return "", {}
    d = Path(new[-1])
    shot = d / ("frame_%05d.ppm" % SHOT)
    digest = hashlib.sha256(shot.read_bytes()).hexdigest() if shot.exists() else ""
    res = json.loads((d / "result.json").read_text())
    return digest, res.get("bench") or {}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--save-ref", action="store_true")
    args = ap.parse_args()
    exe = str(Path(args.exe).resolve())
    got = {}
    for sc in SCENARIOS:
        digest, b = run(exe, sc)
        got[sc] = digest
        print("%-9s shot %s  mean %.3f ms  ticks %s" % (sc, digest[:16] or "MISSING", b.get("mean_ms", 0), b.get("ticks")))
    if args.save_ref:
        REF.parent.mkdir(parents=True, exist_ok=True)
        REF.write_text(json.dumps(got, indent=2))
        print("reference saved")
        return 0
    ref = json.loads(REF.read_text())
    bad = [sc for sc in SCENARIOS if got.get(sc) != ref.get(sc)]
    print("IDENTICAL" if not bad else "DIFFERENT: " + " ".join(bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
