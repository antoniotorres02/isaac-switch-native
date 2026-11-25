#!/usr/bin/env python3
"""Find the guest addresses hard-coded in build sources and rewrite them.

  rebase_sources.py --map map.json --old OLD.exe --new NEW.exe scan FILES...
      prints a TSV: file, line, literal, old va, new va, status
  rebase_sources.py --map map.json --old OLD.exe --new NEW.exe apply FILES... [--allow changed]
      rewrites the literals in place, keeping each literal's spelling
      (width, case, `u` suffix, `L_`/`sub_`/`FUN_`/`DAT_` prefixes).

Status, from most to least certain:
  function      the entry of a function whose code is identical
  interior      inside an identical function (same offset)
  ref           referenced by identical code (global, table, string, callee)
  changed       inside a function that changed; placed by instruction alignment
  data          data near a mapped reference, same bytes at the same delta
  unresolved    no safe placement: needs a human
Only literals inside the old image's address range are considered; values
that are addresses of nothing (constants) come out as `unresolved` and are
left untouched by `apply`.
"""
from __future__ import annotations

import argparse
import bisect
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from map_versions import Image  # noqa: E402

LIT = re.compile(r"(?:(?<=\\n)|(?<![0-9A-Za-z]))(?P<pre>(?:L_|sub_|FUN_|DAT_|LAB_|fn_|va_|lifted_)?)"
                 r"(?P<hex>(?:0[xX])?[0-9a-fA-F]{6,8})(?P<suf>u?)(?![0-9A-Za-z])")


class Resolver:
    def __init__(self, mp: dict, old: Image, new: Image):
        self.old, self.new = old, new
        self.funcs = {int(k, 16): int(v["new"], 16) for k, v in mp["functions"].items()}
        self.changed = {int(k, 16): (int(v["new"], 16), v["ratio"],
                                     {int(a, 16): int(b, 16) for a, b in v["insn_map"].items()})
                        for k, v in mp["changed"].items()}
        self.refs = {int(k, 16): (int(v["new"], 16), v) for k, v in mp["refs"].items()}
        self.ends = {int(k, 16): int(v, 16) for k, v in mp["old_function_ends"].items()}
        self.starts = sorted(self.ends)
        self.ref_keys = sorted(self.refs)

    @staticmethod
    def masked(img, va, n=16):
        """n bytes at va with relocated dwords (pointers) as None."""
        b = list(img.bytes(va - 3, n + 6))
        for p in range(va - 3, va + n):
            if p in img.relocs:
                for k in range(4):
                    if 0 <= p - (va - 3) + k < len(b):
                        b[p - (va - 3) + k] = None
        return b[3:3 + n]

    def containing(self, va):
        i = bisect.bisect_right(self.starts, va) - 1
        if i >= 0 and va < self.ends[self.starts[i]]:
            return self.starts[i]
        return None

    def resolve(self, va: int):
        if va in self.funcs:
            return self.funcs[va], "function"
        f = self.containing(va)
        if f is not None and f in self.funcs:
            return self.funcs[f] + (va - f), "interior"
        if va in self.refs:
            nv, d = self.refs[va]
            if d.get("conflict"):
                return nv, "ref-conflict"
            return nv, "ref"
        if f is not None and f in self.changed:
            nb, ratio, imap = self.changed[f]
            if va in imap:
                return imap[va], "changed(%.2f)" % ratio
            if va == f:
                return nb, "changed(%.2f)" % ratio
            return None, "changed-unaligned"
        sec = self.old.section_of(va)
        if sec and sec != ".text":
            i = bisect.bisect_right(self.ref_keys, va) - 1
            while i >= 0 and va - self.ref_keys[i] < 0x2000:
                r = self.ref_keys[i]
                nr = self.refs[r][0]
                if self.old.section_of(r) == sec and self.new.section_of(nr) == ("%s" % sec):
                    nv = nr + (va - r)
                    # same 16 bytes (or zero-init .bss tail) at the delta?
                    ob, nb = self.masked(self.old, va), self.masked(self.new, nv)
                    if all(b is None for b in ob):
                        return nv, "data-pointers"   # only pointers: a delta proves nothing
                    if ob == nb and any(b for b in ob if b is not None):
                        return nv, "data"
                    if ob == nb:
                        # zero-filled (.bss): only trust a short hop from a mapped reference
                        return nv, ("data-zero" if va - r <= 0x40 else "data-zero-far")
                    return nv, "data-differs"
                i -= 1
        return None, "unresolved"


def in_image(img: Image, va: int) -> bool:
    return img.base + 0x1000 <= va < img.end


def literals(text: str):
    for m in LIT.finditer(text):
        h = m.group("hex")
        if not (h.lower().startswith("0x") or m.group("pre")):
            continue  # bare hex digits only count with a prefix
        yield m, int(h, 16)


def fmt_like(m, nv: int) -> str:
    h = m.group("hex")
    body = h[2:] if h.lower().startswith("0x") else h
    digits = ("%0*x" % (len(body), nv))
    if body.isupper() or (any(c.isalpha() for c in body) and body == body.upper()):
        digits = digits.upper()
    out = (h[:2] if h.lower().startswith("0x") else "") + digits
    return m.group("pre") + out + m.group("suf")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--map", type=Path, required=True)
    ap.add_argument("--old", type=Path, required=True)
    ap.add_argument("--new", type=Path, required=True)
    ap.add_argument("mode", choices=["scan", "apply"])
    ap.add_argument("files", nargs="+", type=Path)
    ap.add_argument("--allow", action="append", default=[],
                    help="extra statuses apply may rewrite (default: function interior ref data)")
    ap.add_argument("--override", action="append", default=[],
                    help="old=new pairs decided by hand (hex)")
    args = ap.parse_args(argv)
    old, new = Image(args.old), Image(args.new)
    res = Resolver(json.loads(args.map.read_text()), old, new)
    overrides = {int(a, 16): int(b, 16) for a, b in (o.split("=") for o in args.override)}
    allowed = {"function", "interior", "ref", "data", "data-zero"} | set(args.allow)
    w = sys.stdout
    for f in args.files:
        text = f.read_text(encoding="utf-8", errors="surrogateescape")
        out, last = [], 0
        for m, va in literals(text):
            if not in_image(old, va):
                continue
            if va in overrides:
                nv, st = overrides[va], "override"
            else:
                nv, st = res.resolve(va)
            line = text.count("\n", 0, m.start()) + 1
            if args.mode == "scan":
                w.write("%s\t%d\t%s\t0x%08x\t%s\t%s\n" % (f, line, m.group(0), va,
                                                         "-" if nv is None else "0x%08x" % nv, st))
            elif nv is not None and (st == "override" or any(st == a or st.startswith(a) for a in allowed)):
                out.append(text[last:m.start()])
                out.append(fmt_like(m, nv))
                last = m.end()
        if args.mode == "apply":
            out.append(text[last:])
            new_text = "".join(out)
            if new_text != text:
                f.write_text(new_text, encoding="utf-8", errors="surrogateescape")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
