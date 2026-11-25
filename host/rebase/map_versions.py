#!/usr/bin/env python3
"""Map guest addresses of one isaac-ng.exe build onto another.

Every address the build hard-codes (lift patches, shims, hand-written
functions, fast paths, our hooks) belongs to one exact executable. When Steam
ships a new build, this tool works out where each old function and global
went, and says whether the code there is still the same.

Method:
  * Each old function (from the old Ghidra export) is disassembled and turned
    into a byte pattern in which every dword listed in .reloc (absolute
    addresses) and every rel32 of call/jmp/jcc is a wildcard.
  * The pattern is searched in the new .text; a unique hit is an *identical*
    function (same code, only addresses moved).
  * Identical pairs vote: each masked dword and rel32 at the same offset gives
    old target -> new target. That maps functions that are called from
    identical code but changed themselves, and every global, vtable, string
    and jump table those functions reference.
  * Changed functions get an instruction-level alignment (difflib over
    normalised instructions) so a mid-function address can still be placed,
    flagged as `changed`.

Usage:
  map_versions.py --old OLD.exe --new NEW.exe --old-functions functions.jsonl \
      --out map.json [--query 0x9312f1 ...]
"""
from __future__ import annotations

import argparse
import bisect
import difflib
import json
import struct
from collections import Counter, defaultdict
from pathlib import Path

import capstone
import pefile


class Image:
    def __init__(self, path: Path):
        self.pe = pefile.PE(str(path), fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = bytearray(self.pe.get_memory_mapped_image())
        self.relocs = set()
        for block in getattr(self.pe, "DIRECTORY_ENTRY_BASERELOC", []):
            for e in block.entries:
                if e.type == 3:  # HIGHLOW
                    self.relocs.add(self.base + e.rva)
        self.sections = {}
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            self.sections[name] = (self.base + s.VirtualAddress, self.base + s.VirtualAddress + s.Misc_VirtualSize)
        self.end = self.base + len(self.mem)

    def section_of(self, va: int) -> str | None:
        for n, (a, b) in self.sections.items():
            if a <= va < b:
                return n
        return None

    def bytes(self, va: int, n: int) -> bytes:
        o = va - self.base
        return bytes(self.mem[o:o + n])

    def u32(self, va: int) -> int:
        return struct.unpack_from("<I", self.mem, va - self.base)[0]


CS = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def decode(img: Image, start: int, end: int):
    """Instructions (va, size, mnemonic, op_str) from start to end."""
    code = img.bytes(start, end - start)
    out = []
    for (addr, size, mn, ops) in CS.disasm_lite(code, start):
        out.append((addr, size, mn, ops))
    return out


def rel32_field(img: Image, va: int, size: int) -> int | None:
    b = img.bytes(va, min(size, 2))
    if size == 5 and b[0] in (0xE8, 0xE9):
        return va + 1
    if size == 6 and b[0] == 0x0F and 0x80 <= b[1] <= 0x8F:
        return va + 2
    return None


def pattern(img: Image, start: int, end: int):
    """(masked bytes as list[int|None], refs list[(offset, kind, old_target)], insns)."""
    insns = decode(img, start, end)
    data = list(img.bytes(start, end - start))
    refs = []
    for (va, size, mn, ops) in insns:
        r = rel32_field(img, va, size)
        if r is not None:
            tgt = (va + size + struct.unpack("<i", img.bytes(r, 4))[0]) & 0xFFFFFFFF
            refs.append((r - start, "rel", tgt))
            for k in range(4):
                data[r - start + k] = None
        for p in range(va, va + size - 3):
            if p in img.relocs:
                refs.append((p - start, "abs", img.u32(p)))
                for k in range(4):
                    data[p - start + k] = None
    return data, refs, insns


def anchor(data):
    """Longest run of concrete bytes (offset, bytes), capped at 48."""
    best = (0, 0)
    i = 0
    n = len(data)
    while i < n:
        if data[i] is None:
            i += 1
            continue
        j = i
        while j < n and data[j] is not None:
            j += 1
        if j - i > best[1]:
            best = (i, j - i)
        i = j
    off, ln = best
    ln = min(ln, 48)
    return off, bytes(data[off:off + ln])


def matches_at(new: Image, start: int, data, refs) -> bool:
    if start < new.base or start + len(data) > new.end:
        return False
    blob = new.bytes(start, len(data))
    for i, b in enumerate(data):
        if b is not None and blob[i] != b:
            return False
    for (off, kind, _t) in refs:
        if kind == "abs" and (start + off) not in new.relocs:
            return False
    return True


def norm_insn(mn: str, ops: str) -> str:
    """Instruction text with immediates/displacements that look like addresses erased."""
    import re
    return mn + " " + re.sub(r"0x[0-9a-f]{5,8}", "A", ops)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", type=Path, required=True)
    ap.add_argument("--new", type=Path, required=True)
    ap.add_argument("--old-functions", type=Path, required=True)
    ap.add_argument("--new-functions", type=Path, help="new Ghidra functions.jsonl (improves changed-function bounds)")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)

    old, new = Image(args.old), Image(args.new)
    text_new = new.sections[".text"]
    tnew = new.bytes(text_new[0], text_new[1] - text_new[0])

    funcs = []
    for line in args.old_functions.read_text().splitlines():
        f = json.loads(line)
        if f.get("external") or not f.get("inText", True):
            continue
        a, e = int(f["va"], 16), int(f["endVa"], 16)
        funcs.append((a, e, f.get("bodyRanges", 1)))
    funcs.sort()
    new_funcs = []
    if args.new_functions:
        for line in args.new_functions.read_text().splitlines():
            f = json.loads(line)
            if f.get("external") or not f.get("inText", True):
                continue
            new_funcs.append((int(f["va"], 16), int(f["endVa"], 16)))
        new_funcs.sort()

    # 1. exact matches of whole functions
    fmap: dict[int, int] = {}      # old entry -> new entry (identical code)
    ambiguous: dict[int, list[int]] = {}
    pats = {}
    for (a, e, nr) in funcs:
        if e - a < 3:
            continue
        data, refs, insns = pattern(old, a, e)
        pats[a] = (e, data, refs, insns)
        off, anc = anchor(data)
        if len(anc) < 4 and len(data) < 8:
            # tiny thunks: resolved by votes only
            ambiguous[a] = []
            continue
        hits = []
        pos = tnew.find(anc)
        while pos != -1:
            s = text_new[0] + pos - off
            if matches_at(new, s, data, refs):
                hits.append(s)
                if len(hits) > 8:
                    break
            pos = tnew.find(anc, pos + 1)
        if len(hits) == 1:
            fmap[a] = hits[0]
        else:
            ambiguous[a] = hits

    # 2. votes from identical pairs
    def collect_votes():
        votes: dict[int, Counter] = defaultdict(Counter)
        for a, b in fmap.items():
            e, data, refs, _ = pats[a]
            for (off, kind, tgt) in refs:
                if kind == "rel":
                    p = b + off
                    nt = (p + 4 + struct.unpack("<i", new.bytes(p, 4))[0]) & 0xFFFFFFFF
                else:
                    nt = new.u32(b + off)
                votes[tgt][nt] += 1
        return votes

    def table_votes(votes):
        """Pointer tables (vtables, jump tables, callback arrays) in data: once a
        table's start is mapped, walk both copies while the dwords are relocated."""
        extra: dict[int, Counter] = defaultdict(Counter)
        for tgt, c in list(votes.items()):
            nt, _n = c.most_common(1)[0]
            if old.section_of(tgt) not in (".rdata", ".data", ".text") or tgt not in old.relocs:
                continue
            k = 0
            while k < 4096 and (tgt + k) in old.relocs and (nt + k) in new.relocs:
                extra[old.u32(tgt + k)][new.u32(nt + k)] += 1
                k += 4
        return extra

    def merge(v1, v2):
        out: dict[int, Counter] = defaultdict(Counter)
        for v in (v1, v2):
            for k, c in v.items():
                out[k].update(c)
        return out

    def locality():
        """Pick, among several identical hits, the one between the mapped neighbours."""
        keys = sorted(fmap)
        added = 0
        for a, hits in list(ambiguous.items()):
            if len(hits) < 2:
                continue
            i = bisect.bisect_left(keys, a)
            lo = fmap[keys[i - 1]] if i > 0 else text_new[0]
            hi = fmap[keys[i]] if i < len(keys) else text_new[1]
            inside = [h for h in hits if lo < h < hi]
            if len(inside) == 1:
                fmap[a] = inside[0]
                del ambiguous[a]
                added += 1
        return added

    for _round in range(12):
        votes = collect_votes()
        votes = merge(votes, table_votes(votes))
        added = 0
        for a, hits in list(ambiguous.items()):
            v = votes.get(a)
            if not v:
                continue
            cand, _n = v.most_common(1)[0]
            e, data, refs, _ = pats[a]
            if (hits and cand in hits) or (not hits and matches_at(new, cand, data, refs)):
                fmap[a] = cand
                del ambiguous[a]
                added += 1
        added += locality()
        if not added:
            break
    votes = collect_votes()
    votes = merge(votes, table_votes(votes))

    # 3. global address map: votes (functions, data, labels inside identical functions)
    amap: dict[int, dict] = {}
    for a, b in fmap.items():
        amap[a] = {"new": b, "kind": "function", "identical": True}
    for tgt, c in votes.items():
        if tgt in amap:
            continue
        best, n = c.most_common(1)[0]
        total = sum(c.values())
        amap[tgt] = {"new": best, "kind": "ref", "votes": n, "total": total,
                     "identical": None, "conflict": n != total}

    # 4a. gap alignment: unmatched old functions between two mapped neighbours
    #     against the unclaimed new functions between their images.
    gap_pairs = {}
    if new_funcs:
        new_end = dict(new_funcs)
        claimed = set(fmap.values()) | {d["new"] for d in amap.values()}
        new_starts = [s for s, _e in new_funcs]
        old_starts = [a for (a, _e, _n) in funcs]
        anchors = sorted((a, b) for a, b in fmap.items())
        anc_old = [a for a, _b in anchors]
        runs = defaultdict(list)
        for a in old_starts:
            if a in amap or a not in pats:
                continue
            i = bisect.bisect_left(anc_old, a)
            lo = anchors[i - 1] if i > 0 else (text_new[0], text_new[0])
            hi = anchors[i] if i < len(anchors) else (text_new[1], text_new[1])
            runs[(lo[1], hi[1])].append(a)
        norm_cache = {}

        def ntext(img, s, e):
            key = (id(img), s)
            if key not in norm_cache:
                norm_cache[key] = [norm_insn(m, o) for (_, _, m, o) in decode(img, s, e)]
            return norm_cache[key]

        for (lo, hi), olds in runs.items():
            i = bisect.bisect_right(new_starts, lo)
            cands = []
            while i < len(new_starts) and new_starts[i] < hi:
                if new_starts[i] not in claimed:
                    cands.append(new_starts[i])
                i += 1
            if not cands or len(olds) * len(cands) > 4000:
                continue
            j0 = 0
            for a in olds:
                e = pats[a][0]
                oa = ntext(old, a, e)
                best, bj = 0.0, None
                for j in range(j0, len(cands)):
                    b = cands[j]
                    nb = ntext(new, b, new_end[b])
                    if min(len(oa), len(nb)) < 0.5 * max(len(oa), len(nb)):
                        continue
                    sm = difflib.SequenceMatcher(a=oa, b=nb, autojunk=False)
                    if sm.quick_ratio() < best:
                        continue
                    r = sm.ratio()
                    if r > best:
                        best, bj = r, j
                if bj is not None and best >= 0.5:
                    gap_pairs[a] = (cands[bj], best)
                    amap[a] = {"new": cands[bj], "kind": "function", "identical": False, "gap": True}
                    j0 = bj + 1

    # 4b. changed functions whose entry is known: instruction alignment
    changed = {}
    for (a, e, nr) in funcs:
        if a in fmap or a not in amap or a not in pats:
            continue
        b = amap[a]["new"]
        # new end: next new function start (from new export) or same length + slack
        if new_funcs:
            i = bisect.bisect_left(new_funcs, (b, 0))
            ne = new_funcs[i][1] if i < len(new_funcs) and new_funcs[i][0] == b else b + (e - a) + 0x400
        else:
            ne = b + (e - a) + 0x400
        oi = pats[a][3]
        ni = decode(new, b, min(ne, text_new[1]))
        sm = difflib.SequenceMatcher(a=[norm_insn(m, o) for (_, _, m, o) in oi],
                                     b=[norm_insn(m, o) for (_, _, m, o) in ni], autojunk=False)
        imap = {}
        for (i, j, n) in sm.get_matching_blocks():
            for k in range(n):
                imap[oi[i + k][0]] = ni[j + k][0]
        changed[a] = {"new": b, "ratio": round(sm.ratio(), 4), "insn_map": imap,
                      "old_len": e - a, "old_insns": len(oi), "new_insns": len(ni)}
        amap[a]["kind"] = "function"
        amap[a]["identical"] = False
        amap[a]["ratio"] = round(sm.ratio(), 4)

    # 5. instruction addresses inside identical functions map by offset
    starts = sorted(a for (a, _e, _n) in funcs)

    result = {
        "old_sha_note": str(args.old), "new_sha_note": str(args.new),
        "stats": {"old_functions": len(funcs), "identical": len(fmap), "changed_with_entry": len(changed),
                  "unmatched": len([1 for (a, _e, _n) in funcs if a not in amap]),
                  "addresses": len(amap)},
        "functions": {("0x%08x" % a): {"new": "0x%08x" % fmap[a], "end_old": "0x%08x" % pats[a][0]}
                      for a in sorted(fmap)},
        "changed": {("0x%08x" % a): {"new": "0x%08x" % v["new"], "ratio": v["ratio"],
                                     "old_insns": v["old_insns"], "new_insns": v["new_insns"],
                                     "insn_map": {("0x%08x" % k): ("0x%08x" % x) for k, x in v["insn_map"].items()}}
                    for a, v in sorted(changed.items())},
        "refs": {("0x%08x" % a): {**{k: v for k, v in d.items() if k != "new"}, "new": "0x%08x" % d["new"]}
                 for a, d in sorted(amap.items()) if d["kind"] == "ref"},
        "old_function_starts": ["0x%08x" % a for a in starts],
        "old_function_ends": {("0x%08x" % a): "0x%08x" % e for (a, e, _n) in funcs},
    }
    args.out.write_text(json.dumps(result))
    print(json.dumps(result["stats"], indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
