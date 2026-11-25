"""The game versions this build knows, one folder each under versions/.

versions/<version>/version.json names the version's files: Steam's build id
and executable (with its DRM), the unpacked executable the build reads, and
for a supported version the upstream overlay (upstream.patch, the changes to
upstream's scripts that version needs) and the function list Ghidra found
(functions.txt, see functions_data.py). A version kept only to be recognised
has "supported": false.

Standard library only: the builder program imports it too.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "versions"


class Version:
    def __init__(self, folder: Path):
        self.folder = folder
        self.data = json.loads((folder / "version.json").read_text(encoding="utf-8"))
        self.name = self.data["version"]
        self.supported = bool(self.data.get("supported"))
        self.buildid = str(self.data["steam"]["buildid"])
        self.steam_exe_sha256 = self.data["steam"]["exe_sha256"].lower()
        self.pe_sha256 = self.data["pe"]["sha256"].lower()
        self.pe_size = int(self.data["pe"]["size"])

    @property
    def overlay(self) -> Path | None:
        name = self.data.get("upstream", {}).get("overlay")
        return self.folder / name if name else None

    def fingerprint(self) -> str:
        """Changes whenever anything in the folder does: the stages that use it run again."""
        h = hashlib.sha256()
        for p in sorted(self.folder.iterdir()):
            if p.is_file():
                h.update(p.name.encode() + b"\0" + p.read_bytes())
        return h.hexdigest()[:16]

    def __repr__(self) -> str:
        return "Version(%s%s)" % (self.name, "" if self.supported else ", unsupported")


def load(root: Path | None = None) -> list[Version]:
    root = root or ROOT     # read at call time, so tests can point it elsewhere
    out = [Version(p) for p in sorted(root.iterdir()) if (p / "version.json").is_file()]
    return sorted(out, key=lambda v: [int(x) if x.isdigit() else x for x in v.name.split(".")])


def supported(root: Path | None = None) -> list[Version]:
    return [v for v in load(root) if v.supported]


def latest(root: Path | None = None) -> Version:
    return supported(root)[-1]


def _find(attr: str, value: str, root: Path) -> Version | None:
    value = str(value).lower()
    for v in load(root):
        if str(getattr(v, attr)).lower() == value:
            return v
    return None


def by_pe_sha256(sha: str, root: Path | None = None) -> Version | None:
    return _find("pe_sha256", sha, root)


def by_steam_exe_sha256(sha: str, root: Path | None = None) -> Version | None:
    return _find("steam_exe_sha256", sha, root)


def by_buildid(buildid: str, root: Path | None = None) -> Version | None:
    return _find("buildid", buildid, root)


def pe_sizes(root: Path | None = None) -> set[int]:
    return {v.pe_size for v in supported(root)}
