#!/usr/bin/env python3
"""Place one old instruction address in the new executable by its context.

For addresses the map leaves as `changed-unaligned`/`unresolved`: takes the
normalised instructions around the old address and slides that window over
the new function the map pairs with the old one (or over a range given with
--near), printing the best positions with both listings side by side.

  locate.py --map map.json --old OLD.exe --new NEW.exe 0x00a2b5c2 [--near 0xa44000:0xa46000]
"""
from __future__ import annotations

import argparse
import bisect
import difflib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from map_versions import Image, decode, norm_insn  # noqa: E402


def window(insns, idx, before, after):
    return insns[max(0, idx - before): idx + after]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", type=Path, required=True)
    ap.add_argument("--old", type=Path, required=True)
    ap.add_argument("--new", type=Path, required=True)
    ap.add_argument("--new-functions", type=Path)
    ap.add_argument("--near", help="new range lo:hi to search")
    ap.add_argument("--before", type=int, default=8)
    ap.add_argument("--after", type=int, default=8)
    ap.add_argument("--top", type=int, default=2)
    ap.add_argument("--brief", action="store_true", help="one line per address")
    ap.add_argument("va", nargs="+")
    a = ap.parse_args(argv)
    old, new = Image(a.old), Image(a.new)
    mp = json.loads(a.map.read_text())
    ends = {int(k, 16): int(v, 16) for k, v in mp["old_function_ends"].items()}
    starts = sorted(ends)
    pairs = {int(k, 16): int(v["new"], 16) for k, v in mp["functions"].items()}
    pairs.update({int(k, 16): int(v["new"], 16) for k, v in mp["changed"].items()})
    nends = {}
    if a.new_functions:
        for line in a.new_functions.read_text().splitlines():
            f = json.loads(line)
            nends[int(f["va"], 16)] = int(f["endVa"], 16)
    for vs in a.va:
        va = int(vs, 16)
        i = bisect.bisect_right(starts, va) - 1
        f = starts[i]
        oi = decode(old, f, ends[f])
        idx = next((k for k, x in enumerate(oi) if x[0] == va), None)
        if idx is None:
            # address not on an instruction boundary of the function: decode from it
            oi = decode(old, va - 0x40, va + 0x60)
            idx = next((k for k, x in enumerate(oi) if x[0] == va), None)
            if idx is None:
                print("== 0x%08x: not an instruction start" % va)
                continue
        ow = window(oi, idx, a.before, a.after)
        on = [norm_insn(m, o) for (_, _, m, o) in ow]
        if a.near:
            lo, hi = (int(x, 16) for x in a.near.split(":"))
        elif f in pairs:
            lo = pairs[f]
            hi = nends.get(lo, lo + (ends[f] - f) * 2 + 0x200)
        else:
            # between the images of the nearest mapped neighbours
            ks = sorted(k for k in pairs if k in ends)
            j = bisect.bisect_left(ks, f)
            lo = pairs[ks[j - 1]] if j > 0 else new.sections[".text"][0]
            hi = pairs[ks[j]] if j < len(ks) else new.sections[".text"][1]
            if hi - lo > 0x20000:
                print("== 0x%08x (in 0x%08x): neighbours too far apart; use --near" % (va, f))
                continue
        ni = decode(new, lo, hi)
        nn = [norm_insn(m, o) for (_, _, m, o) in ni]
        scores = []
        for k in range(len(ni)):
            w = nn[max(0, k - a.before): k + a.after]
            pad = a.before - (k - max(0, k - a.before))
            sm = difflib.SequenceMatcher(a=on, b=w, autojunk=False)
            scores.append((sm.ratio(), k))
        scores.sort(reverse=True)
        if a.brief:
            (r, k), (r2, _k2) = scores[0], (scores[1] if len(scores) > 1 else (0, 0))
            same = oi[idx][2:] == ni[k][2:] or norm_insn(*oi[idx][2:]) == norm_insn(*ni[k][2:])
            print("0x%08x\t0x%08x\t%.3f\t%.3f\t%s\t%s %s" % (va, ni[k][0], r, r2, "same" if same else "DIFF",
                                                        oi[idx][2], oi[idx][3]))
            continue
        print("== 0x%08x (in old 0x%08x, new fn 0x%08x)" % (va, f, lo))
        for (r, k) in scores[:a.top]:
            nw = window(ni, k, a.before, a.after)
            print("  candidate 0x%08x  score %.3f" % (ni[k][0], r))
            for (x, y) in zip(ow + [None] * max(0, len(nw) - len(ow)), nw + [None] * max(0, len(ow) - len(nw))):
                ls = ("%08x %s %s" % (x[0], x[2], x[3])) if x else ""
                rs = ("%08x %s %s" % (y[0], y[2], y[3])) if y else ""
                mark = ">>" if (x and x[0] == va) else "  "
                print("   %s %-46s | %s" % (mark, ls[:46], rs[:60]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
