"""The function list Ghidra finds in a game executable, stored as a small text file.

The build used to run Ghidra for 25 minutes (and needed 1.2 GB of Ghidra and
Java in the image) only to learn where each function starts and ends. Of its
whole export, the pipeline reads just this:

  output/recomp/export/functions.jsonl
      va, minVa, endVa, inText, external      lift/prepare_starts.py, lift/emit.py
      recovered                               host/gen_shims.py (skips recovered rows)
  output/recomp/export/recovered-functions.tsv
      va, looksLikePrologue                   lift/emit.py (load_fragments)

Ghidra's output is deterministic for these fields (the same exe gave the same
bytes on two machines), so a maintainer runs it once per game version and
stores the result in versions/<version>/functions.txt:

    # comment lines
    <va - previous va, hex> <endVa - va, hex> [flags]

flags: r = recovered by Ghidra's gap-recovery pass, p = that row looked like a
prologue, m<hex> = minVa is va - <hex>. Every function is in .text and none is
external (`make` refuses an export where that does not hold).

    python functions_data.py make <ghidra export dir> <versions/<version>>
    python functions_data.py write <versions/<version>> <output/recomp/export>
    python functions_data.py check <versions/<version>> <ghidra export dir>
"""
import argparse
import csv
import hashlib
import json
import sys
from pathlib import Path

FILE = "functions.txt"


class Row:
    __slots__ = ("va", "min_va", "end_va", "recovered", "prologue")

    def __init__(self, va, min_va, end_va, recovered=False, prologue=False):
        self.va, self.min_va, self.end_va = va, min_va, end_va
        self.recovered, self.prologue = recovered, prologue

    def key(self):
        return (self.va, self.min_va, self.end_va, self.recovered, self.prologue)


def read_export(export: Path) -> list:
    """The rows of a Ghidra export (functions.raw.jsonl or functions.jsonl, plus the TSV)."""
    raw = export / "functions.raw.jsonl"
    if not raw.is_file():
        raw = export / "functions.jsonl"
    frags = {}
    with open(export / "recovered-functions.tsv", encoding="utf-8") as fh:
        for rec in csv.DictReader(fh, delimiter="\t"):
            frags[int(rec["va"], 16)] = rec["looksLikePrologue"].strip().lower() == "true"
    rows = {}
    with open(raw, encoding="utf-8") as fh:
        for line in fh:
            if not line.strip():
                continue
            d = json.loads(line)
            va = int(d["va"], 16)
            if not d.get("inText") or d.get("external"):
                raise SystemExit("%s: 0x%08x is outside .text or external; the format has no room for it" % (raw, va))
            if va in rows:
                raise SystemExit("%s: 0x%08x appears twice" % (raw, va))
            min_va, end_va = int(d["minVa"], 16), int(d["endVa"], 16)
            if min_va > va or end_va <= va:
                raise SystemExit("%s: 0x%08x has minVa/endVa the format cannot hold" % (raw, va))
            rows[va] = Row(va, min_va, end_va, va in frags, frags.get(va, False))
    missing = set(frags) - set(rows)
    if missing:
        raise SystemExit("recovered-functions.tsv lists %d functions the inventory lacks (0x%08x...)"
                         % (len(missing), min(missing)))
    return [rows[va] for va in sorted(rows)]


def encode(rows: list, header: str) -> str:
    out = ["# " + line for line in header.splitlines()]
    prev = 0
    for r in rows:
        flags = ("r" if r.recovered else "") + ("p" if r.prologue else "")
        if r.min_va != r.va:
            flags += "m%x" % (r.va - r.min_va)
        out.append("%x %x%s" % (r.va - prev, r.end_va - r.va, " " + flags if flags else ""))
        prev = r.va
    return "\n".join(out) + "\n"


def decode(text: str) -> list:
    rows, va = [], 0
    for n, line in enumerate(text.splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        va += int(parts[0], 16)
        end = va + int(parts[1], 16)
        flags = parts[2] if len(parts) > 2 else ""
        min_va = va
        if "m" in flags:
            min_va = va - int(flags[flags.index("m") + 1:], 16)
            flags = flags[:flags.index("m")]
        if set(flags) - {"r", "p"}:
            raise ValueError("%s line %d: unknown flags %r" % (FILE, n, flags))
        rows.append(Row(va, min_va, end, "r" in flags, "p" in flags))
    return rows


def write_export(rows: list, export: Path) -> None:
    """functions.jsonl and recovered-functions.tsv with the fields the pipeline reads."""
    export.mkdir(parents=True, exist_ok=True)
    with open(export / "functions.jsonl", "w", encoding="utf-8", newline="\n") as fh:
        for r in rows:
            fh.write(json.dumps({"va": "0x%08x" % r.va, "minVa": "0x%08x" % r.min_va,
                                 "endVa": "0x%08x" % r.end_va, "inText": True, "external": False,
                                 "recovered": r.recovered}, separators=(",", ":")) + "\n")
    with open(export / "recovered-functions.tsv", "w", encoding="utf-8", newline="\n") as fh:
        fh.write("va\tlooksLikePrologue\n")
        for r in rows:
            if r.recovered:
                fh.write("0x%08x\t%s\n" % (r.va, "true" if r.prologue else "false"))


def digest(rows: list) -> str:
    h = hashlib.sha256()
    for r in rows:
        h.update(("%08x %08x %08x %d %d\n" % (r.va, r.min_va, r.end_va, r.recovered, r.prologue)).encode())
    return h.hexdigest()


def load(version_dir: Path) -> list:
    """The rows of a version, checked against the count and digest in its version.json."""
    meta = json.loads((version_dir / "version.json").read_text(encoding="utf-8"))["functions"]
    rows = decode((version_dir / meta["file"]).read_text(encoding="utf-8"))
    if len(rows) != meta["count"] or digest(rows) != meta["sha256"]:
        raise SystemExit("%s does not match version.json (count or digest)" % (version_dir / meta["file"]))
    return rows


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("make", help="maintainer: store a Ghidra export in a version folder")
    m.add_argument("export", type=Path)
    m.add_argument("version_dir", type=Path)
    w = sub.add_parser("write", help="build: write the export files the pipeline reads")
    w.add_argument("version_dir", type=Path)
    w.add_argument("export", type=Path)
    c = sub.add_parser("check", help="compare a version folder with a Ghidra export")
    c.add_argument("version_dir", type=Path)
    c.add_argument("export", type=Path)
    args = ap.parse_args(argv)

    if args.cmd == "make":
        rows = read_export(args.export)
        stats = json.loads((args.export / "ghidra-stats.json").read_text()) if (args.export / "ghidra-stats.json").is_file() else {}
        header = ("Function starts and ends Ghidra found in this version's executable (functions_data.py).\n"
                  "Addresses only: no code or data from the game.")
        if stats.get("ghidraVersion"):
            header += "\nGhidra %s." % stats["ghidraVersion"]
        text = encode(rows, header)
        assert [r.key() for r in decode(text)] == [r.key() for r in rows]
        (args.version_dir / FILE).write_text(text, encoding="utf-8", newline="\n")
        vj = args.version_dir / "version.json"
        data = json.loads(vj.read_text(encoding="utf-8")) if vj.is_file() else {}
        data["functions"] = {"file": FILE, "count": len(rows), "recovered": sum(r.recovered for r in rows),
                             "sha256": digest(rows)}
        vj.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8", newline="\n")
        print("%d functions (%d recovered) -> %s, %d bytes" % (len(rows), data["functions"]["recovered"],
                                                               args.version_dir / FILE, len(text)))
        return 0
    if args.cmd == "write":
        rows = load(args.version_dir)
        write_export(rows, args.export)
        print("%d functions -> %s" % (len(rows), args.export))
        return 0
    rows = load(args.version_dir)
    ghidra = read_export(args.export)
    if [r.key() for r in rows] != [r.key() for r in ghidra]:
        print("DIFFERENT: %s does not match %s" % (args.version_dir, args.export))
        return 1
    print("same: %d functions" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
