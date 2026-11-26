#!/usr/bin/env python3
"""Exercise issue #1 on a built Linux GLES executable using fresh test saves."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

FIXTURE = Path(__file__).resolve().parent / "fixtures" / "vade_retro_room_clear"
# Confirm the startup screens, move from Normal to Greed, then start the run.
CONFIRM = "405:enter,445:enter,485:enter,525:enter"
GREED_INPUT = "120:enter,160:enter,200:enter,240:enter,340:down,365:down," + CONFIRM
# A save from the Greed control run starts with Greed selected.
GREEDIER_INPUT = "120:enter,160:enter,200:enter,240:enter,340:down," + CONFIRM
PERSISTENT = Path("documents/my games/binding of isaac repentance+/persistentgamedata1.dat")


def run_case(exe: Path, image: Path, bundle: Path, work: Path, scenario: str) -> bool:
    greedier = scenario.startswith("greedier-")
    input_script, frames, waves = (GREEDIER_INPUT, 3200, 12) if greedier else (GREED_INPUT, 2800, 11)
    env = dict(os.environ, ISAAC_SAVE_DIR=str(work / scenario), ISAAC_INPUT=input_script,
               ISAAC_MAX_FRAMES=str(frames), ISAAC_NATIVE_BENCH=scenario, ISAAC_NATIVE_AUDIO="0",
               ISAAC_NATIVE_BENCH_MOD=str(FIXTURE), ISAAC_NATIVE_WINDOW="0")
    env.setdefault("EGL_PLATFORM", "surfaceless")
    env.setdefault("LIBGL_ALWAYS_SOFTWARE", "1")
    args = [str(exe), str(image), str(bundle), "main"]
    try:
        run = subprocess.run(args, cwd=work, env=env, timeout=180, text=True,
                             encoding="utf-8", errors="replace",
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        print(f"{scenario}: timed out before the run finished")
        return False
    marker = f"[vade-retro-test] cleared all {waves} waves ({scenario})"
    if run.returncode == 0 and marker in run.stdout:
        print(f"{scenario}: cleared all {waves} waves")
        return True
    print(f"{scenario}: clear marker missing or game failed (exit {run.returncode})")
    lines = run.stdout.splitlines()
    significant = [line for line in lines if "[vade-retro-test]" in line or "recomp:" in line]
    print("\n".join(significant[-15:] or lines[-20:]))
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True, help="Linux GLES isaac_native binary")
    parser.add_argument("--upstream", type=Path, required=True, help="prepared upstream checkout")
    parser.add_argument("--greedier", action="store_true", help="also test Greedier (requires Node.js on the host)")
    args = parser.parse_args()
    exe = args.exe.resolve()
    upstream = args.upstream.resolve()
    image = upstream / "output/recomp/host/isaac.segs.bin"
    bundle = upstream / ".scratch/game-bundle"
    unlock = upstream / "scripts/recomp/assets/make_unlocked_save.mjs"
    if not exe.is_file() or not image.is_file() or not bundle.is_dir():
        parser.error("build the Linux GLES executable and prepare the game bundle first")
    if args.greedier and (not unlock.is_file() or not shutil.which("node")):
        parser.error("--greedier requires Node.js and the upstream make_unlocked_save.mjs script")
    with tempfile.TemporaryDirectory(prefix="isaac-vade-retro-") as tmp:
        work = Path(tmp)
        results = [run_case(exe, image, bundle, work, scenario)
                   for scenario in ("control", "equipped", "use")]
        if args.greedier:
            seed = work / "control" / PERSISTENT
            if not results[0] or not seed.is_file():
                print("Greedier: the Greed control run did not create a usable save")
                return 1
            for scenario in ("greedier-equipped", "greedier-use"):
                save = work / scenario / PERSISTENT
                save.parent.mkdir(parents=True)
                unlocked = subprocess.run(["node", str(unlock), str(seed), str(save)],
                                          stdout=subprocess.DEVNULL)
                if unlocked.returncode:
                    print(f"{scenario}: failed to unlock Greedier in the test save")
                    return 1
                results.append(run_case(exe, image, bundle, work, scenario))
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
