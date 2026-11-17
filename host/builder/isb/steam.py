"""Finds the game's Steam install: library folders, app manifest, build and DLC."""
from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field
from pathlib import Path

APPID = "250900"
INSTALL_DIR = "The Binding of Isaac Rebirth"
# The expansions the build needs, as appmanifest_250900.acf names them ("dlcappid").
DLC = {"401920": "Afterbirth", "570660": "Afterbirth+", "1426300": "Repentance", "3353470": "Repentance+"}


def parse_vdf(text: str) -> dict:
    """Valve's KeyValues text format: quoted keys and values, nested braces."""
    tokens: list[str | None] = []   # None for "{", "" sentinel handled below
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == "/" and text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "{" or c == "}":
            tokens.append(c)
            i += 1
        elif c == '"':
            out, i = [], i + 1
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    out.append({"n": "\n", "t": "\t"}.get(text[i + 1], text[i + 1]))
                    i += 2
                else:
                    out.append(text[i])
                    i += 1
            tokens.append("\x00" + "".join(out))   # marks a string token
            i += 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in '{}"':
                j += 1
            tokens.append("\x00" + text[i:j])
            i = j

    pos = 0

    def block() -> dict:
        nonlocal pos
        result: dict = {}
        while pos < len(tokens):
            tok = tokens[pos]
            if tok == "}":
                pos += 1
                return result
            if tok == "{":          # stray brace: skip
                pos += 1
                continue
            key = tok[1:]
            pos += 1
            if pos >= len(tokens):
                break
            nxt = tokens[pos]
            if nxt == "{":
                pos += 1
                result[key] = block()
            elif nxt == "}":
                continue
            else:
                result[key] = nxt[1:]
                pos += 1
        return result

    return block()


def _registry_steam() -> list[Path]:
    if sys.platform != "win32":
        return []
    import winreg
    found = []
    for hive, key, name in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                            (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath")):
        try:
            with winreg.OpenKey(hive, key) as k:
                found.append(Path(winreg.QueryValueEx(k, name)[0]))
        except OSError:
            pass
    return found


def steam_roots(home: Path | None = None, platform: str | None = None) -> list[Path]:
    """Steam installs that exist on this machine, most likely first."""
    platform = platform or sys.platform
    home = home or Path.home()
    if platform == "win32":
        candidates = _registry_steam() + [Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Steam",
                                          Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Steam"]
    else:
        candidates = [home / ".local/share/Steam", home / ".steam/steam", home / ".steam/root",
                      home / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
                      home / "snap/steam/common/.local/share/Steam"]
    roots, seen = [], set()
    for c in candidates:
        try:
            real = c.resolve()
        except OSError:
            continue
        if (real / "steamapps").is_dir() and real not in seen:
            seen.add(real)
            roots.append(real)
    return roots


def libraries(root: Path) -> list[Path]:
    """The library folders a Steam install knows, itself included."""
    libs = [root]
    vdf = root / "steamapps" / "libraryfolders.vdf"
    try:
        data = parse_vdf(vdf.read_text(encoding="utf-8", errors="replace"))
    except OSError:
        return libs
    for entry in data.get("libraryfolders", {}).values():
        path = entry.get("path") if isinstance(entry, dict) else entry
        if isinstance(path, str) and path:
            p = Path(path)
            if p not in libs:
                libs.append(p)
    return libs


@dataclass
class GameInstall:
    path: Path
    manifest: Path | None = None
    buildid: str | None = None
    dlcs: set[str] = field(default_factory=set)

    @property
    def missing_dlcs(self) -> list[str]:
        if self.manifest is None:
            return []           # unknown, not missing
        return [name for appid, name in DLC.items() if appid not in self.dlcs]


def read_manifest(manifest: Path, library: Path) -> GameInstall | None:
    try:
        state = parse_vdf(manifest.read_text(encoding="utf-8", errors="replace")).get("AppState", {})
    except OSError:
        return None
    installdir = state.get("installdir") or INSTALL_DIR
    dlcs = {d.get("dlcappid") for d in state.get("InstalledDepots", {}).values()
            if isinstance(d, dict) and d.get("dlcappid")}
    return GameInstall(library / "steamapps" / "common" / installdir, manifest, state.get("buildid"), dlcs)


def find_games(home: Path | None = None, platform: str | None = None) -> list[GameInstall]:
    """Every install of the game in every Steam library, most likely first."""
    games, seen = [], set()
    for root in steam_roots(home, platform):
        for lib in libraries(root):
            manifest = lib / "steamapps" / ("appmanifest_%s.acf" % APPID)
            if manifest.is_file():
                game = read_manifest(manifest, lib)
                if game and game.path.is_dir():
                    key = game.path.resolve()
                    if key not in seen:
                        seen.add(key)
                        games.append(game)
    return games


def find_game(home: Path | None = None, platform: str | None = None) -> GameInstall | None:
    games = find_games(home, platform)
    return games[0] if games else None


def describe_folder(path: Path) -> GameInstall:
    """A folder the user chose: its manifest, when it sits in a Steam library."""
    lib = path.parent.parent.parent       # <lib>/steamapps/common/<game>
    manifest = lib / "steamapps" / ("appmanifest_%s.acf" % APPID)
    if path.parent.name == "common" and manifest.is_file():
        game = read_manifest(manifest, lib)
        if game and game.path.resolve() == path.resolve():
            return game
    return GameInstall(path)
