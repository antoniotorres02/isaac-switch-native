#!/usr/bin/env python3
"""The install payload: the game as two NCAs that only lack header encryption.

The Builder cannot make an NSP: an installable NCA has its 0xC00-byte header
encrypted with the console's header key, and that key is not ours to ship.
Everything else in an NCA needs no key at all when its sections are stored
in plaintext (encryption type 1, which FS reads like any other), so this
script writes both NCAs complete -- hash trees, superblocks, the header with
every field set -- and the installer NRO (installer/source) finishes them on
the console: it derives the header key through spl, the way FS does,
encrypts the headers, hashes the results, builds the meta NCA and installs
the three through ncm.

    payload.py --elf game.elf --segs isaac.segs.bin --index instance_index.json \\
               --bundle game-bundle/ --bench bench/isaac-bench --out install/

writes <out>/program.nca, <out>/control.nca and <out>/install.txt.

Layout inside the program NCA's RomFS (native_switch.c reads it):
    /isaac.segs.bin  /instance_index.json  /isaac-bench/  /guest/<the game's files>

Every hash level and PFS0 here is padded to a whole number of hash blocks,
and its declared size is the padded one, so no hash covers a partial block:
how the last block is hashed is the one point readers have disagreed on.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent

# 0x05... is the range homebrew titles use; ...000 leaves the low bits for the
# patch (+0x800) and add-on (+0x1000) ids that the metadata derives from it.
TITLE_ID = 0x0500B15AAC000000
TITLE_NAME = "ISAAC REPENTANCE+ NS1 RECOMP"
AUTHOR = "isaacport"
SDK_VERSION = 0x000C1100        # FS refuses NCAs below 0x000B0000
MEDIA = 0x200
EXEFS_BLOCK = 0x10000
IVFC_BLOCK_LOG2 = 14            # 0x4000
CHUNK = 8 << 20

CONTENT_PROGRAM, CONTENT_META, CONTENT_CONTROL = 0, 1, 2
FS_ROMFS, FS_PFS0 = 0, 1
HASH_SHA256, HASH_IVFC = 2, 3
CRYPT_NONE = 1

LANGUAGES = ("AmericanEnglish BritishEnglish Japanese French German LatinAmericanSpanish Spanish Italian "
             "Dutch CanadianFrench Portuguese Russian Korean TraditionalChinese SimplifiedChinese "
             "BrazilianPortuguese").split()


def align(x: int, a: int) -> int:
    return (x + a - 1) // a * a


# ---- sources: bytes in memory or a file on disk -------------------------------------

@dataclass
class Blob:
    """A run of bytes that is either held in memory or read from a file."""
    data: bytes | None = None
    path: Path | None = None
    size: int = 0

    @staticmethod
    def of(data: bytes) -> "Blob":
        return Blob(data=data, size=len(data))

    @staticmethod
    def file(path: Path) -> "Blob":
        return Blob(path=path, size=path.stat().st_size)

    def chunks(self):
        if self.data is not None:
            for i in range(0, len(self.data), CHUNK):
                yield self.data[i:i + CHUNK]
            return
        with open(self.path, "rb") as f:
            left = self.size
            while left:
                b = f.read(min(CHUNK, left))
                if not b:
                    raise IOError("%s shrank while it was being read" % self.path)
                left -= len(b)
                yield b


class Stream:
    """A sequence of Blobs and zero runs, read in order as one byte stream."""

    def __init__(self):
        self.parts: list[Blob | int] = []
        self.size = 0

    def add(self, part: Blob | bytes) -> None:
        if isinstance(part, bytes):
            part = Blob.of(part)
        self.parts.append(part)
        self.size += part.size

    def pad_to(self, a: int) -> None:
        n = align(self.size, a) - self.size
        if n:
            self.parts.append(n)
            self.size += n

    def chunks(self):
        for p in self.parts:
            if isinstance(p, int):
                left = p
                while left:
                    n = min(left, CHUNK)
                    yield bytes(n)
                    left -= n
            else:
                yield from p.chunks()

    def blocks(self, block: int):
        """The stream cut into exact `block`-sized pieces (the last may be short)."""
        buf = bytearray()
        for c in self.chunks():
            buf += c
            while len(buf) >= block:
                yield bytes(buf[:block])
                del buf[:block]
        if buf:
            yield bytes(buf)


# ---- RomFS --------------------------------------------------------------------------

def romfs_hash(parent: int, name: bytes) -> int:
    h = parent ^ 123456789
    for c in name:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= c
    return h


def romfs_bucket_count(n: int) -> int:
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    c = n
    while any(c % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        c += 1
    return c


@dataclass
class RDir:
    name: bytes
    parent: "RDir | None" = None
    dirs: dict = field(default_factory=dict)
    files: dict = field(default_factory=dict)
    off: int = 0


def build_romfs(entries: dict[str, Blob]) -> Stream:
    """A RomFS image of `entries` ({"a/b.bin": Blob}) as a Stream."""
    root = RDir(b"")
    for path, blob in entries.items():
        parts = path.split("/")
        d = root
        for p in parts[:-1]:
            d = d.dirs.setdefault(p, RDir(p.encode(), d))
        d.files[parts[-1]] = blob

    dirs: list[RDir] = []          # breadth first, children by name
    queue = [root]
    while queue:
        d = queue.pop(0)
        dirs.append(d)
        queue += [d.dirs[k] for k in sorted(d.dirs)]
    files: list[tuple[RDir, bytes, Blob]] = [(d, k.encode(), d.files[k]) for d in dirs for k in sorted(d.files)]

    def entry_len(name: bytes, base: int) -> int:
        return base + align(len(name), 4)

    off = 0
    for d in dirs:
        d.off = off
        off += entry_len(d.name, 0x18)
    dir_meta_size = off
    file_off, off = {}, 0
    for i, (d, name, _) in enumerate(files):
        file_off[i] = off
        off += entry_len(name, 0x20)
    file_meta_size = off

    data_off_of, data = {}, 0
    for i, (_, _, blob) in enumerate(files):
        data = align(data, 0x10)
        data_off_of[i] = data
        data += blob.size
    data_size = data

    EMPTY = 0xFFFFFFFF
    dir_buckets = [EMPTY] * romfs_bucket_count(len(dirs))
    file_buckets = [EMPTY] * romfs_bucket_count(len(files))

    dir_meta = bytearray(dir_meta_size)
    for d in dirs:
        parent = d.parent.off if d.parent else 0
        siblings = sorted(d.parent.dirs) if d.parent else []
        sib = EMPTY
        if d.parent:
            i = siblings.index(d.name.decode())
            if i + 1 < len(siblings):
                sib = d.parent.dirs[siblings[i + 1]].off
        child = d.dirs[sorted(d.dirs)[0]].off if d.dirs else EMPTY
        first_file = EMPTY
        if d.files:
            first = sorted(d.files)[0]
            first_file = next(file_off[i] for i, (fd, n, _) in enumerate(files) if fd is d and n == first.encode())
        b = romfs_hash(parent, d.name) % len(dir_buckets)
        nxt, dir_buckets[b] = dir_buckets[b], d.off
        struct.pack_into("<6I", dir_meta, d.off, parent, sib, child, first_file, nxt, len(d.name))
        dir_meta[d.off + 0x18:d.off + 0x18 + len(d.name)] = d.name

    file_meta = bytearray(file_meta_size)
    for i, (d, name, blob) in enumerate(files):
        o = file_off[i]
        sib = file_off[i + 1] if i + 1 < len(files) and files[i + 1][0] is d else EMPTY
        b = romfs_hash(d.off, name) % len(file_buckets)
        nxt, file_buckets[b] = file_buckets[b], o
        struct.pack_into("<IIQQII", file_meta, o, d.off, sib, data_off_of[i], blob.size, nxt, len(name))
        file_meta[o + 0x20:o + 0x20 + len(name)] = name

    data_start = 0x200
    meta_start = align(data_start + data_size, 4)
    dir_hash_off = meta_start
    dir_meta_off = dir_hash_off + 4 * len(dir_buckets)
    file_hash_off = dir_meta_off + dir_meta_size
    file_meta_off = file_hash_off + 4 * len(file_buckets)
    header = struct.pack("<10Q", 0x50, dir_hash_off, 4 * len(dir_buckets), dir_meta_off, dir_meta_size,
                         file_hash_off, 4 * len(file_buckets), file_meta_off, file_meta_size, data_start)

    s = Stream()
    s.add(header)
    s.pad_to(data_start)
    for i, (_, _, blob) in enumerate(files):
        s.pad_to(0x10)
        assert s.size - data_start == data_off_of[i]
        s.add(blob)
    s.pad_to(4)
    s.add(struct.pack("<%dI" % len(dir_buckets), *dir_buckets))
    s.add(bytes(dir_meta))
    s.add(struct.pack("<%dI" % len(file_buckets), *file_buckets))
    s.add(bytes(file_meta))
    return s


# ---- PFS0 ---------------------------------------------------------------------------

def build_pfs0(files: list[tuple[str, bytes]]) -> bytes:
    names = b""
    offs = []
    for name, _ in files:
        offs.append(len(names))
        names += name.encode() + b"\0"
    head_len = 0x10 + 0x18 * len(files) + len(names)
    names += bytes(align(head_len, 0x20) - head_len)
    out = bytearray(struct.pack("<4sIII", b"PFS0", len(files), len(names), 0))
    data_off = 0
    for (name, data), so in zip(files, offs):
        out += struct.pack("<QQII", data_off, len(data), so, 0)
        data_off += len(data)
    out += names
    for _, data in files:
        out += data
    return bytes(out)


# ---- NCA sections -------------------------------------------------------------------

@dataclass
class Section:
    fs_header: bytes      # 0x200
    body: Stream          # the section's bytes, a multiple of MEDIA


def fs_header(fs_type: int, hash_type: int, hash_data: bytes) -> bytes:
    h = bytearray(0x200)
    struct.pack_into("<HBBBB", h, 0, 2, fs_type, hash_type, CRYPT_NONE, 0)
    h[8:8 + len(hash_data)] = hash_data
    return bytes(h)


def pfs0_section(pfs0: bytes, block: int) -> Section:
    pfs0 += bytes(align(len(pfs0), block) - len(pfs0))
    table = b"".join(hashlib.sha256(pfs0[i:i + block]).digest() for i in range(0, len(pfs0), block))
    pfs0_off = align(len(table), MEDIA)
    hd = hashlib.sha256(table).digest() + struct.pack("<II4Q", block, 2, 0, len(table), pfs0_off, len(pfs0))
    body = Stream()
    body.add(table)
    body.pad_to(MEDIA)
    body.add(pfs0)
    body.pad_to(MEDIA)
    return Section(fs_header(FS_PFS0, HASH_SHA256, hd), body)


def romfs_section(romfs: Stream, progress=None) -> Section:
    """Six IVFC levels: five of hashes over the RomFS, each padded to whole blocks."""
    block = 1 << IVFC_BLOCK_LOG2
    romfs.pad_to(block)
    levels: list[bytes] = []
    hashes = bytearray()
    done = 0
    for b in romfs.blocks(block):
        hashes += hashlib.sha256(b).digest()
        done += len(b)
        if progress:
            progress(done, romfs.size)
    level = bytes(hashes) + bytes(align(len(hashes), block) - len(hashes))
    levels.insert(0, level)
    while len(levels) < 5:
        lv = levels[0]
        h = b"".join(hashlib.sha256(lv[i:i + block]).digest() for i in range(0, len(lv), block))
        levels.insert(0, h + bytes(align(len(h), block) - len(h)))
    sizes = [len(l) for l in levels] + [romfs.size]
    offs, o = [], 0
    for s in sizes:
        offs.append(o)
        o += s
    hd = struct.pack("<4sIII", b"IVFC", 0x20000, 0x20, 7)
    for off, size in zip(offs, sizes):
        hd += struct.pack("<QQII", off, size, IVFC_BLOCK_LOG2, 0)
    hd += bytes(0x20) + hashlib.sha256(levels[0]).digest()
    body = Stream()
    for l in levels:
        body.add(l)
    for p in romfs.parts:
        body.parts.append(p)
    body.size += romfs.size
    body.pad_to(MEDIA)
    return Section(fs_header(FS_ROMFS, HASH_IVFC, hd), body)


def nca_header(content_type: int, title_id: int, sections: list[Section]) -> tuple[bytes, int]:
    """The plaintext 0xC00-byte header and the NCA's total size."""
    h = bytearray(0xC00)
    off = 0xC00
    for i, s in enumerate(sections):
        start = off
        off += s.body.size
        struct.pack_into("<IIB", h, 0x240 + 0x10 * i, start // MEDIA, off // MEDIA, 1)
        h[0x280 + 0x20 * i:0x2A0 + 0x20 * i] = hashlib.sha256(s.fs_header).digest()
        h[0x400 + 0x200 * i:0x600 + 0x200 * i] = s.fs_header
    struct.pack_into("<4sBBBBQQII", h, 0x200, b"NCA3", 0, content_type, 0, 0, off, title_id, 0, SDK_VERSION)
    return bytes(h), off


def write_nca(path: Path, content_type: int, title_id: int, sections: list[Section]) -> tuple[int, str]:
    header, size = nca_header(content_type, title_id, sections)
    sha = hashlib.sha256()
    with open(path, "wb") as f:
        f.write(header)
        sha.update(header)
        for s in sections:
            for c in s.body.chunks():
                f.write(c)
                sha.update(c)
    assert path.stat().st_size == size, (path, size)
    return size, sha.hexdigest()


# ---- control: NACP and icon ---------------------------------------------------------

def make_nacp(base: bytes, title_id: int, version: str) -> bytes:
    """nacptool's NACP for an installed title: no user picker, no console save data."""
    n = bytearray(base)
    assert len(n) == 0x4000
    for i in range(16):
        e = 0x300 * i
        n[e:e + 0x300] = bytes(0x300)
        n[e:e + len(TITLE_NAME)] = TITLE_NAME.encode()
        n[e + 0x200:e + 0x200 + len(AUTHOR)] = AUTHOR.encode()
    n[0x3025] = 0                                   # StartupUserAccount: none
    struct.pack_into("<Q", n, 0x3038, title_id)     # PresenceGroupId
    n[0x3060:0x3070] = bytes(0x10)
    n[0x3060:0x3060 + len(version)] = version.encode()
    struct.pack_into("<Q", n, 0x3070, title_id + 0x1000)   # AddOnContentBaseId
    struct.pack_into("<Q", n, 0x3078, title_id)     # SaveDataOwnerId
    n[0x3080:0x30A8] = bytes(0x28)                  # no user/device save data sizes
    return bytes(n)


def make_icon() -> bytes:
    """A 256x256 baseline JPEG: the title in type, no art from the game."""
    from PIL import Image, ImageDraw, ImageFont
    img = Image.new("RGB", (256, 256), (28, 24, 30))
    d = ImageDraw.Draw(img)
    d.rectangle((10, 10, 245, 245), outline=(150, 40, 40), width=4)
    try:
        big, small = ImageFont.load_default(size=44), ImageFont.load_default(size=22)
    except TypeError:
        big = small = ImageFont.load_default()
    for text, font, y, colour in (("ISAAC", big, 62, (236, 228, 214)), ("REPENTANCE+", small, 122, (220, 90, 80)),
                                  ("NS1 RECOMP", small, 160, (170, 164, 156))):
        w = d.textlength(text, font=font)
        d.text(((256 - w) / 2, y), text, font=font, fill=colour)
    out = io.BytesIO()
    img.save(out, "JPEG", quality=90, progressive=False, subsampling=0)
    data = out.getvalue()
    assert len(data) <= 0x20000
    return data


# ---- driver -------------------------------------------------------------------------

def tool(name: str) -> str:
    dkp = os.environ.get("DEVKITPRO", "/opt/devkitpro")
    path = Path(dkp) / "tools" / "bin" / name
    return str(path) if path.exists() else name


def run(cmd: list) -> None:
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode:
        sys.exit("%s failed:\n%s%s" % (cmd[0], r.stdout, r.stderr))


def make_npdm(tmp: Path, title_id: int) -> bytes:
    spec = json.loads((HERE / "npdm.json").read_text())
    tid = "0x%016X" % title_id
    spec.update(title_id=tid, title_id_range_min=tid, title_id_range_max=tid)
    (tmp / "main.json").write_text(json.dumps(spec, indent=1))
    run([tool("npdmtool"), tmp / "main.json", tmp / "main.npdm"])
    return (tmp / "main.npdm").read_bytes()


def game_entries(segs: Path, index: Path, bundle: Path | None, bench: Path | None,
                 launcher: Path | None = None) -> dict[str, Blob]:
    entries = {"isaac.segs.bin": Blob.file(segs), "instance_index.json": Blob.file(index)}
    if bench:
        for p in sorted(bench.rglob("*")):
            if p.is_file():
                entries["isaac-bench/" + p.relative_to(bench).as_posix()] = Blob.file(p)
    if launcher:
        for path in sorted(launcher.iterdir()):
            if path.is_file():
                entries["launcher/" + path.name] = Blob.file(path)
    if bundle:
        for row in json.loads(index.read_text()):
            src = bundle / row["p"]
            if not src.is_file():
                sys.exit("the build lists %s, but it is not in the game's files" % row["p"])
            entries["guest/" + row["p"]] = Blob.file(src)
    return entries


def build(args) -> int:
    out: Path = args.out
    out.mkdir(parents=True, exist_ok=True)
    title_id = args.title_id
    with tempfile.TemporaryDirectory(prefix="isaac-payload-") as t:
        tmp = Path(t)
        if args.nso:
            nso = args.nso.read_bytes()
        else:
            run([tool("elf2nso"), args.elf, tmp / "main"])
            nso = (tmp / "main").read_bytes()
        npdm = make_npdm(tmp, title_id)
        run([tool("nacptool"), "--create", TITLE_NAME, AUTHOR, args.version, tmp / "control.nacp"])
        nacp = make_nacp((tmp / "control.nacp").read_bytes(), title_id, args.version)

    exefs = pfs0_section(build_pfs0([("main", nso), ("main.npdm", npdm)]), EXEFS_BLOCK)
    entries = game_entries(args.segs, args.index, args.bundle, args.bench, args.launcher)
    total = sum(b.size for b in entries.values())
    print("program romfs: %d files, %.1f MB" % (len(entries), total / 1048576), flush=True)
    last = [0]

    def progress(done, size):
        pct = done * 100 // size
        if pct >= last[0] + 10:
            last[0] = pct
            print("  hashing %d%%" % pct, flush=True)

    romfs = romfs_section(build_romfs(entries), progress)
    program_size, program_sha = write_nca(out / "program.nca", CONTENT_PROGRAM, title_id, [exefs, romfs])

    icon = args.icon.read_bytes() if args.icon else make_icon()
    control_files = {"control.nacp": Blob.of(nacp)}
    for lang in LANGUAGES:
        control_files["icon_%s.dat" % lang] = Blob.of(icon)
    control_size, control_sha = write_nca(out / "control.nca", CONTENT_CONTROL, title_id,
                                          [romfs_section(build_romfs(control_files))])

    (out / "install.txt").write_text(
        "# Read by the installer NRO (installer/source/main.c). content: type, file, size, content id\n"
        "# (the first half of the plaintext SHA-256: unique per build, known before the console hashes it).\n"
        "title_id %016x\n"
        "version 0\n"
        "display_version %s\n"
        "name %s\n"
        "content program program.nca %d %s\n"
        "content control control.nca %d %s\n" % (title_id, args.version, TITLE_NAME, program_size, program_sha[:32],
                                                 control_size, control_sha[:32]))
    print("program.nca %.1f MB (plaintext sha256 %s)" % (program_size / 1048576, program_sha))
    print("control.nca %.1f KB (plaintext sha256 %s)" % (control_size / 1024, control_sha))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--elf", type=Path, help="the game's linked ELF (elf2nso makes the NSO)")
    src.add_argument("--nso", type=Path, help="an NSO already made (tests)")
    ap.add_argument("--segs", type=Path, required=True, help="isaac.segs.bin")
    ap.add_argument("--index", type=Path, required=True, help="instance_index.json")
    ap.add_argument("--bundle", type=Path, help="the game's files the index lists (left out in tests)")
    ap.add_argument("--bench", type=Path, help="bench/isaac-bench")
    ap.add_argument("--launcher", type=Path, help="local launcher artwork")
    ap.add_argument("--icon", type=Path, help="a 256x256 baseline JPEG; one is drawn when absent")
    ap.add_argument("--title-id", type=lambda s: int(s, 16), default=TITLE_ID)
    ap.add_argument("--version", default="0.1.0", help="the display version")
    ap.add_argument("--out", type=Path, required=True)
    return build(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
