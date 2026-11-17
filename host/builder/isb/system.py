"""Small things that differ between Windows and Linux."""
from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import webbrowser
from contextlib import contextmanager
from pathlib import Path

DOCKER_URL = {"win32": "https://docs.docker.com/desktop/setup/install/windows-install/",
              "linux": "https://docs.docker.com/engine/install/"}


def docker_url() -> str:
    return DOCKER_URL["win32" if sys.platform == "win32" else "linux"]


# What a person on Linux pastes into a terminal; the window copies it for them.
COMMANDS = {"copy_group": "sudo usermod -aG docker $USER",
            "copy_start": "sudo systemctl start docker"}


def docker_desktop() -> Path | None:
    """Docker Desktop's program on Windows, when it is installed."""
    if sys.platform != "win32":
        return None
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramW6432"), r"C:\Program Files"):
        if base:
            exe = Path(base) / "Docker" / "Docker" / "Docker Desktop.exe"
            if exe.is_file():
                return exe
    return None


def start_docker_desktop() -> bool:
    exe = docker_desktop()
    if not exe:
        return False
    try:
        subprocess.Popen([str(exe)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        return True
    except OSError:
        return False


def winget() -> str | None:
    return shutil.which("winget") if sys.platform == "win32" else None


def can_install_docker() -> bool:
    """On Windows, winget installs Docker Desktop with one click (and an administrator prompt)."""
    return winget() is not None


def install_docker(on_line) -> int:
    """Installs Docker Desktop with winget. Windows asks for administrator permission."""
    exe = winget()
    if not exe:
        return 1
    cmd = [exe, "install", "--exact", "--id", "Docker.DockerDesktop", "--source", "winget",
           "--accept-package-agreements", "--accept-source-agreements", "--disable-interactivity"]
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                text=True, encoding="utf-8", errors="replace", creationflags=0x08000000)
    except OSError as e:
        on_line(str(e))
        return 1
    for line in proc.stdout:
        # winget draws progress bars with carriage returns and block characters.
        line = line.replace("\r", "\n").split("\n")[-1].strip() or line.strip()
        if line and any(c.isalnum() for c in line):
            on_line(line)
    return proc.wait()


def uses_volume() -> bool:
    """On Windows the work folder lives in a Docker volume: a Windows folder
    mounted into Linux is slow for Ghidra and ignores letter case."""
    return sys.platform == "win32"


def default_folder() -> Path:
    """Where the result goes (and, on Linux, the work as well)."""
    home = Path.home()
    if sys.platform == "win32":
        docs = home / "Documents"
        return (docs if docs.is_dir() else home) / "IsaacSwitchBuild"
    return home / "IsaacSwitchBuild"


def work_folder(base: Path) -> Path | None:
    return None if uses_volume() else base / "work"


def result_folder(base: Path) -> Path:
    """The SD card folder build_all.py leaves: exported on Windows, in place on Linux."""
    return base / "sd-card" if uses_volume() else base / "work" / "out" / "sd-card"


def export_folder(base: Path) -> Path | None:
    return base if uses_volume() else None


def suggested_start_dir() -> Path:
    """Where a file dialog opens when nothing else is known: Downloads, usually."""
    home = Path.home()
    for d in (home / "Downloads", home / "Descargas"):
        if d.is_dir():
            return d
    return home


def open_path(path: Path) -> None:
    try:
        if sys.platform == "win32":
            os.startfile(str(path))       # noqa: the Windows way to open a folder
        else:
            subprocess.Popen(["xdg-open", str(path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        pass


def open_url(url: str) -> None:
    webbrowser.open(url)


def open_uri(uri: str) -> None:
    """A link for another program, such as steam://validate/250900."""
    try:
        if sys.platform == "win32":
            os.startfile(uri)             # noqa
        else:
            subprocess.Popen(["xdg-open", uri], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        pass


def wslconfig() -> Path:
    return Path.home() / ".wslconfig"


WSLCONFIG_TEMPLATE = "[wsl2]\nmemory=10GB\n"


def edit_wslconfig() -> Path:
    """Opens %USERPROFILE%\\.wslconfig in Notepad, writing a template if it is absent."""
    path = wslconfig()
    if not path.exists():
        path.write_text(WSLCONFIG_TEMPLATE)
    subprocess.Popen(["notepad.exe", str(path)])
    return path


@contextmanager
def keep_awake(reason: str):
    """Keeps the computer from sleeping while the build runs (about 45 minutes)."""
    inhibitor = None
    if sys.platform == "win32":
        import ctypes
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        try:
            ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
        except Exception:
            pass
    elif shutil.which("systemd-inhibit"):
        try:
            inhibitor = subprocess.Popen(["systemd-inhibit", "--what=sleep:idle", "--who=Isaac Switch Builder",
                                          "--why=" + reason, "--mode=block", "sleep", "infinity"],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                         stdin=subprocess.DEVNULL)
        except OSError:
            inhibitor = None
    try:
        yield
    finally:
        if sys.platform == "win32":
            try:
                import ctypes
                ctypes.windll.kernel32.SetThreadExecutionState(0x80000000)
            except Exception:
                pass
        if inhibitor:
            inhibitor.terminate()


def redact(text: str) -> str:
    """The user's home becomes ~ in anything they may paste somewhere."""
    home = str(Path.home())
    for form in {home, home.replace("\\", "/"), home.replace("/", "\\")}:
        if len(form) > 3:
            text = text.replace(form, "~")
    return text


def describe_system() -> str:
    return "%s %s, %s, Python %s" % (platform.system(), platform.release(), platform.machine(),
                                     platform.python_version())


def default_threads() -> tuple[int, int]:
    """Ghidra threads and compile jobs, as build_all.py chooses them."""
    cpus = os.cpu_count() or 4
    return min(cpus, 8), max(1, cpus - 1)
