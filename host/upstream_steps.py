"""The parts of upstream's web build that the Switch build needs, without Emscripten.

Upstream's scripts/recomp/lift/build_boot.py patches the lifted C, generates
the stubs and the dispatch table, and then compiles all of it to WebAssembly.
It refuses to start without emcc, and the wasm module is of no use here, so
the image carried 1.7 GB of emsdk for the first half. `patch` repeats that
half, in the same order (build_boot.py, "source-level overrides" through
"generate dispatch + loud stubs"), with ISAAC_CALL_SLIM=0 as build_all.py
always ran it.

Likewise upstream's assets/ship.py assembles the browser distribution and
needs the wasm module; the Switch build only reads the file index it writes.
`index` writes that index the same way.

    python upstream_steps.py patch <upstream checkout>
    python upstream_steps.py index <upstream checkout>
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def patch(up: Path) -> int:
    lift = up / "scripts" / "recomp" / "lift"
    gu = up / "output" / "recomp" / "lift" / "gu"
    if not any(gu.glob("lifted_*.c")):
        print("no lifted_*.c in %s" % gu)
        return 2
    os.environ["ISAAC_CALL_SLIM"] = "0"
    sys.path.insert(0, str(lift))
    from lift_patches import (apply_lift_patches, apply_purge_patches, apply_wrap_patches,  # noqa: E402
                              apply_block_patches, apply_entry_first, apply_call_slim)
    from split_giants import apply_split_giants  # noqa: E402
    patched = (set(apply_lift_patches(gu)) | set(apply_purge_patches(gu)) | set(apply_wrap_patches(gu))
               | set(apply_block_patches(gu)) | set(apply_entry_first(gu)))
    patched |= set(apply_split_giants(gu))
    patched |= set(apply_call_slim(gu))
    print("lift-patches: %d TU(s) rewritten" % len(patched), flush=True)

    for script, extra in (("check_lifted.py", ["--module", "lifted"]),
                          ("mkstubs.py", ["--no-import-stubs"]),
                          ("mkdispatch.py", [])):
        r = subprocess.run([sys.executable, str(lift / script), "--dir", str(gu)] + extra, cwd=up)
        if r.returncode:
            print("%s failed (%d)" % (script, r.returncode))
            return 1
    return 0


def index(up: Path) -> int:
    sys.path.insert(0, str(up / "scripts" / "recomp" / "assets"))
    import ship  # noqa: E402
    bundle = up / ".scratch" / "game-bundle"
    dist = up / ".scratch" / "game-dist"
    dist.mkdir(parents=True, exist_ok=True)
    rows = ship.instance_index(str(bundle))
    with open(dist / ship.INDEX_NAME, "w", encoding="utf-8") as f:
        json.dump(rows, f, separators=(",", ":"))
    print("%d files indexed -> %s" % (len(rows), dist / ship.INDEX_NAME))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=("patch", "index"))
    ap.add_argument("upstream", type=Path)
    args = ap.parse_args(argv)
    up = args.upstream.resolve()
    return patch(up) if args.step == "patch" else index(up)


if __name__ == "__main__":
    sys.exit(main())
