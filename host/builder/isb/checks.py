"""The checks a person sees as a list of ✔ / ⚠ / ✘, each with what to do.

They run on the user's machine in seconds. build_all.py repeats the important
ones inside the container before anything slow starts."""
from __future__ import annotations

import hashlib
import os
import shutil
from dataclasses import dataclass, field
from pathlib import Path

import build_all
import versions

from . import steam

# The newest supported version: what the texts name and what the search looks for.
VERSION = versions.latest()
PE_SHA256 = VERSION.pe_sha256
PE_SIZE = VERSION.pe_size
PE_SIZES = versions.pe_sizes()
STEAM_EXE_SHA256 = VERSION.steam_exe_sha256
STEAM_BUILDID = VERSION.buildid
GIB = build_all.GIB
NEED_DISK = build_all.MIN_FREE_DISK
NEED_MEMORY = build_all.MIN_MEMORY

OK, WARN, FAIL, INFO = "ok", "warn", "fail", "info"


@dataclass
class Item:
    """One line of the list: a state, a text key and its values, and maybe a fix
    the window can offer as a button (see gui.FIXES)."""
    state: str
    key: str
    args: dict = field(default_factory=dict)
    fix: str | tuple | None = None

    @property
    def fixes(self) -> tuple:
        if not self.fix:
            return ()
        return self.fix if isinstance(self.fix, tuple) else (self.fix,)


def sha256_file(path: Path, progress=None) -> str:
    h = hashlib.sha256()
    total = max(1, path.stat().st_size)
    done = 0
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
            done += len(block)
            if progress:
                progress(done / total)
    return h.hexdigest()


def blocking(items: list[Item]) -> bool:
    return any(i.state == FAIL for i in items)


# ---- the game folder ---------------------------------------------------------------

def check_game(game: steam.GameInstall | None) -> list[Item]:
    if game is None:
        return [Item(FAIL, "game.not_found", fix="choose_game")]
    path = game.path
    if not path.is_dir():
        return [Item(FAIL, "game.no_folder", {"path": path}, fix="choose_game")]
    missing = [n for n in build_all.GAME_FILES if not (path / n).is_file()]
    if len(missing) == len(build_all.GAME_FILES):
        return [Item(FAIL, "game.not_game", {"path": path}, fix="choose_game")]
    items = [Item(OK, "game.found", {"path": path})]
    if missing:
        items.append(Item(FAIL, "game.incomplete", {"files": ", ".join(missing)}, fix="verify_steam"))
    if game.manifest is not None:
        if game.missing_dlcs:
            items.append(Item(FAIL, "game.dlc_missing", {"dlcs": ", ".join(game.missing_dlcs)}))
        else:
            items.append(Item(OK, "game.dlc_ok"))
        known = versions.by_buildid(game.buildid) if game.buildid else None
        if game.buildid and not (known and known.supported):
            items.append(Item(WARN, "game.build_differs", {"got": game.buildid, "want": STEAM_BUILDID,
                                                           "version": VERSION.name}))
    original = path / "isaac-ng.exe"
    if original.is_file():
        found = versions.by_steam_exe_sha256(sha256_file(original))
        if found and found.supported:
            items.append(Item(OK, "game.version_ok", {"version": found.name}))
        else:
            items.append(Item(WARN, "game.version_differs", {"version": VERSION.name}))
    return items


# ---- the executable ----------------------------------------------------------------

def classify_exe(path: Path | None, progress=None) -> tuple[str, str | None]:
    """'ok' | 'original' | 'retired' | 'other' | 'missing' | 'folder', and its sha256."""
    if path is None or not path.exists():
        return "missing", None
    if path.is_dir():
        return "folder", None
    digest = sha256_file(path, progress)
    v = versions.by_pe_sha256(digest)
    if v:
        return ("ok" if v.supported else "retired"), digest
    if versions.by_steam_exe_sha256(digest):
        return "original", digest
    return "other", digest


def check_exe(path: Path | None, progress=None) -> list[Item]:
    kind, digest = classify_exe(path, progress)
    if kind == "ok":
        return [Item(OK, "exe.ok", {"path": path})]
    if kind == "original":
        return [Item(FAIL, "exe.original", {"path": path}, fix="choose_exe")]
    if kind == "retired":
        old = versions.by_pe_sha256(digest)
        return [Item(FAIL, "exe.retired", {"path": path, "got": old.name, "version": VERSION.name},
                     fix="choose_exe")]
    if kind == "other":
        return [Item(FAIL, "exe.other", {"path": path, "got": digest, "want": PE_SHA256,
                                         "version": VERSION.name}, fix="choose_exe")]
    if kind == "folder":
        return [Item(FAIL, "exe.folder", {"path": path}, fix="choose_exe")]
    return [Item(FAIL, "exe.missing", fix="choose_exe")]


def exe_search_dirs(game: Path | None = None) -> list[Path]:
    home = Path.home()
    dirs = [home / "Downloads", home / "Descargas", home / "Desktop", home / "Escritorio", home / "Documents",
            home / "Documentos", home]
    if game:
        dirs.insert(0, game)
    return [d for d in dirs if d.is_dir()]


def _walk(d: Path, depth: int, level: int = 0):
    try:
        entries = list(os.scandir(d))
    except OSError:
        return
    for e in entries:
        try:
            if e.is_file(follow_symlinks=False):
                yield e
            elif level < depth and e.is_dir(follow_symlinks=False) and not e.name.startswith("."):
                yield from _walk(Path(e.path), depth, level + 1)
        except OSError:
            continue


def exe_candidates(dirs: list[Path], depth: int = 2, game: Path | None = None, limit: int = 20) -> list[Path]:
    """Files worth suggesting: the exact size, or an .exe named like the game's. The
    game's own isaac-ng.exe is left out: it still has the DRM."""
    own = (game / "isaac-ng.exe").resolve() if game else None
    found: list[Path] = []
    for d in dirs:
        for e in _walk(d, depth):
            name = e.name.lower()
            try:
                right_size = e.stat().st_size in PE_SIZES
            except OSError:
                continue
            named = name.endswith(".exe") and any(k in name for k in ("isaac-ng", "isaac_ng", "isaacng", "unpacked"))
            if right_size or named:
                p = Path(e.path)
                if (own and p.resolve() == own) or p in found:
                    continue
                found.append(p)
                if len(found) >= limit:
                    return found
    # The exact size first: those are the ones that can be right.
    return sorted(found, key=lambda p: p.stat().st_size not in PE_SIZES)


def find_exe(dirs: list[Path], depth: int = 2) -> Path | None:
    """A file of the exact size whose hash matches, in a few likely folders."""
    for d in dirs:
        for e in _walk(d, depth):
            try:
                if e.stat().st_size not in PE_SIZES:
                    continue
                v = versions.by_pe_sha256(sha256_file(Path(e.path)))
                if v and v.supported:
                    return Path(e.path)
            except OSError:
                continue
    return None


# ---- this machine ------------------------------------------------------------------

def free_disk(path: Path) -> int | None:
    p = path
    while not p.exists() and p != p.parent:
        p = p.parent
    try:
        return shutil.disk_usage(p).free
    except OSError:
        return None


def check_disk(path: Path, need: int = NEED_DISK, key: str = "disk") -> list[Item]:
    free = free_disk(path)
    if free is None:
        return []
    args = {"path": path, "free": "%.1f" % (free / GIB), "need": "%.0f" % (need / GIB)}
    return [Item(OK if free >= need else FAIL, key + (".ok" if free >= need else ".low"), args)]
