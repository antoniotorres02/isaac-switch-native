"""Finding the SD card and copying the build onto it."""
from __future__ import annotations

import os
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path

NRO = "switch/isaac-repentance-ns1-recomp.nro"


@dataclass
class Drive:
    path: Path
    label: str
    fs: str = ""

    def __str__(self) -> str:
        extra = " (%s)" % self.fs if self.fs else ""
        return "%s  %s%s" % (self.path, self.label, extra) if self.label else "%s%s" % (self.path, extra)


def _windows_drives() -> list[Drive]:
    import ctypes
    k32 = ctypes.windll.kernel32
    buf = ctypes.create_unicode_buffer(512)
    k32.GetLogicalDriveStringsW(len(buf), buf)
    drives = []
    for root in buf[:].split("\x00"):
        if not root or k32.GetDriveTypeW(root) != 2:      # DRIVE_REMOVABLE
            continue
        name = ctypes.create_unicode_buffer(261)
        fs = ctypes.create_unicode_buffer(261)
        if not k32.GetVolumeInformationW(root, name, 261, None, None, None, fs, 261):
            continue                                       # a reader with no card in it
        drives.append(Drive(Path(root), name.value, fs.value))
    return drives


def _linux_drives(mounts: str | None = None) -> list[Drive]:
    if mounts is None:
        try:
            mounts = Path("/proc/mounts").read_text()
        except OSError:
            return []
    drives = []
    for line in mounts.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        point = parts[1].replace("\\040", " ")
        if point.startswith(("/media/", "/run/media/")):
            drives.append(Drive(Path(point), Path(point).name, parts[2]))
    return drives


def removable_drives() -> list[Drive]:
    try:
        return _windows_drives() if sys.platform == "win32" else _linux_drives()
    except Exception:
        return []


def looks_like_switch_card(root: Path) -> bool:
    return (root / "atmosphere").is_dir() or (root / "switch").is_dir() or (root / "Nintendo").is_dir()


def plan_copy(src: Path, dest: Path) -> list[tuple[Path, Path, int]]:
    """(source, target, size) for what has to be copied. A game file already on
    the card with the same size is left alone; the NRO is always copied."""
    todo = []
    for path in sorted(src.rglob("*")):
        if not path.is_file():
            continue
        rel = path.relative_to(src)
        target = dest / rel
        size = path.stat().st_size
        if rel.as_posix() != NRO and target.is_file() and target.stat().st_size == size:
            continue
        todo.append((path, target, size))
    return todo


def copy_to_card(src: Path, dest: Path, progress=None) -> tuple[int, int]:
    """Copies the SD card folder onto the card. Saves and anything else on the card
    are left as they are. Returns (files copied, files checked)."""
    todo = plan_copy(src, dest)
    total = sum(size for _, _, size in todo) or 1
    need = total - sum(t.stat().st_size for _, t, _ in todo if t.is_file())
    free = shutil.disk_usage(dest).free
    if need > free:
        raise OSError("not enough room on %s: %.2f GB free, %.2f GB needed" % (dest, free / 1e9, need / 1e9))
    done = 0
    for source, target, size in todo:
        target.parent.mkdir(parents=True, exist_ok=True)
        part = target.with_name(target.name + ".part")
        with open(source, "rb") as fi, open(part, "wb") as fo:
            while True:
                block = fi.read(4 << 20)
                if not block:
                    break
                fo.write(block)
                done += len(block)
                if progress:
                    progress(done / total, source.name)
            fo.flush()
            os.fsync(fo.fileno())
        if target.exists():
            target.unlink()
        part.rename(target)
    checked = 0
    for path in src.rglob("*"):
        if path.is_file():
            target = dest / path.relative_to(src)
            if not target.is_file() or target.stat().st_size != path.stat().st_size:
                raise OSError("%s did not arrive whole" % target)
            checked += 1
    return len(todo), checked
