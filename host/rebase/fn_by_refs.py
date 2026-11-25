#!/usr/bin/env python3
"""Find an old function in the new executable by what it references.

When a function changed too much for byte or instruction matching, what it
touches usually did not: the globals, strings, vtables and callees it
references. This maps each of those through the version map and ranks the
new functions by how many of the mapped references they contain.

  fn_by_refs.py --map map.json --old OLD.exe --new NEW.exe \
      --old-functions old.jsonl --new-functions new.jsonl 0xa7b6a0 ...
"""
from __future__ import annotations

import argparse
import bisect
import json
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from map_versions import Image, decode, rel32_field  # noqa: E402
from rebase_sources import Resolver  # noqa: E402

TRUSTED = ("function", "interior", "ref", "data")


def refs_of(img: Image, a: int, e: int) -> set[int]:
    out = set()
    for (va, size, _m, _o) in decode(img, a, e):
        r = rel32_field(img, va, size)
        if r is not None:
            out.add((va + size + struct.unpack("<i", img.bytes(r, 4))[0]) & 0xFFFFFFFF)
        for p in range(va, va + size - 3):
            if p in img.relocs:
                out.add(img.u32(p))
    return out


def load_ends(path: Path) -> dict[int, int]:
    d = {}
    for line in path.read_text().splitlines():
        f = json.loads(line)
        if not f.get("external"):
            d[int(f["va"], 16)] = int(f["endVa"], 16)
    return d


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    for k in ("map", "old", "new", "old-functions", "new-functions"):
        ap.add_argument("--" + k, type=Path, required=True)
    ap.add_argument("--top", type=int, default=3)
    ap.add_argument("va", nargs="+")
    a = ap.parse_args(argv)
    old, new = Image(a.old), Image(a.new)
    res = Resolver(json.loads(a.map.read_text()), old, new)
    oe, ne = load_ends(a.old_functions), load_ends(a.new_functions)
    # reverse index: new target -> new functions referencing it
    index: dict[int, set[int]] = {}
    for s, e in ne.items():
        for t in refs_of(new, s, e):
            index.setdefault(t, set()).add(s)
    for vs in a.va:
        va = int(vs, 16)
        if va not in oe:
            print("0x%08x: not an old function start" % va)
            continue
        wanted = []
        for t in refs_of(old, va, oe[va]):
            nt, st = res.resolve(t)
            if nt is not None and st in TRUSTED:
                wanted.append(nt)
        score = Counter()
        for nt in wanted:
            for f in index.get(nt, ()):
                score[f] += 1
        best = score.most_common(a.top)
        print("0x%08x: %d mapped refs; best %s" % (va, len(wanted), ", ".join(
            "0x%08x (%d/%d, %d bytes vs %d)" % (f, n, len(wanted), ne[f] - f, oe[va] - va) for f, n in best)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
