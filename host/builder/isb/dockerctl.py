"""Docker: is it there and running, the image, and the build container.

Everything goes through the docker (or podman) command line, with arguments as
a list and never through a shell: the game's folder has spaces in its name."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import threading
from dataclasses import dataclass
from pathlib import Path

from . import system
from .checks import NEED_MEMORY, OK, FAIL, WARN, Item

CONTAINER = "isaac-switch-build"
VOLUME = "isaac-switch-work"
IMAGE_INFO = json.loads((Path(__file__).with_name("image.json")).read_text(encoding="utf-8"))

# No console window flashing for every docker call on Windows.
_NO_WINDOW = {"creationflags": 0x08000000} if sys.platform == "win32" else {}


def find_engine() -> tuple[str, str] | None:
    """('docker' | 'podman', executable)."""
    exe = shutil.which("docker")
    if not exe and sys.platform == "win32":
        default = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Docker/Docker/resources/bin/docker.exe"
        if default.is_file():
            exe = str(default)
    if exe:
        return "docker", exe
    exe = shutil.which("podman")
    if exe:
        return "podman", exe
    return None


def _run(cmd: list, timeout: float = 30) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout,
                          stdin=subprocess.DEVNULL, **_NO_WINDOW)


@dataclass
class Status:
    state: str              # missing | no_permission | stopped | windows_containers | ok | error
    engine: str | None = None
    exe: str | None = None
    memory: int | None = None
    cpus: int | None = None
    detail: str = ""


def status() -> Status:
    found = find_engine()
    if not found:
        return Status("missing")
    engine, exe = found
    try:
        r = _run([exe, "info", "--format", "{{json .}}"], timeout=40)
    except (OSError, subprocess.TimeoutExpired) as e:
        return Status("stopped", engine, exe, detail=str(e))
    text = (r.stdout or "").strip()
    err = (r.stderr or "").strip()
    info = None
    for line in reversed(text.splitlines()):
        try:
            info = json.loads(line)
            break
        except ValueError:
            continue
    low = err.lower()
    if info is None or r.returncode != 0:
        if "permission denied" in low:
            return Status("no_permission", engine, exe, detail=err)
        return Status("stopped", engine, exe, detail=err or text)
    if info.get("ServerErrors"):
        return Status("stopped", engine, exe, detail="; ".join(info["ServerErrors"]))
    if engine == "podman":
        host = info.get("host", {})
        return Status("ok", engine, exe, host.get("memTotal"), host.get("cpus"))
    if str(info.get("OSType", "linux")).lower() == "windows":
        return Status("windows_containers", engine, exe)
    return Status("ok", engine, exe, info.get("MemTotal"), info.get("NCPU"))


def status_items(st: Status, platform: str | None = None) -> list[Item]:
    win = (platform or sys.platform) == "win32"
    if st.state == "missing":
        if win:
            fix = ("install_docker", "docker_page") if system.can_install_docker() else ("docker_page",)
            return [Item(FAIL, "docker.missing_win", fix=fix)]
        return [Item(FAIL, "docker.missing", fix="docker_page")]
    if st.state == "no_permission":
        return [Item(FAIL, "docker.no_permission", fix="copy_group")]
    if st.state == "stopped":
        if win:
            # The page has its own "Check again", and polls by itself.
            return [Item(FAIL, "docker.stopped_win", fix="start_docker" if system.docker_desktop() else None)]
        return [Item(FAIL, "docker.stopped", {"detail": st.detail[:200]}, fix="copy_start")]
    if st.state == "windows_containers":
        return [Item(FAIL, "docker.windows_containers")]
    items = [Item(OK, "docker.ok", {"engine": st.engine})]
    if st.memory:
        gib = st.memory / 1024 ** 3
        if st.memory >= NEED_MEMORY:
            items.append(Item(OK, "docker.memory_ok", {"gib": "%.1f" % gib}))
        else:
            fix = "wslconfig" if sys.platform == "win32" else None
            items.append(Item(FAIL, "docker.memory_low", {"gib": "%.1f" % gib, "need": NEED_MEMORY // 1024 ** 3}, fix=fix))
    if st.engine == "podman":
        items.append(Item(WARN, "docker.podman"))
    return items


# ---- the image ---------------------------------------------------------------------------

def image_ref(override: str | None = None) -> str:
    """ISAAC_BUILDER_IMAGE or --image, else the published digest, else the local tag."""
    if override:
        return override
    env = os.environ.get("ISAAC_BUILDER_IMAGE")
    if env:
        return env
    if IMAGE_INFO.get("digest"):
        return "%s@%s" % (IMAGE_INFO["repository"], IMAGE_INFO["digest"])
    return IMAGE_INFO["local"]


def image_is_published(ref: str) -> bool:
    return "@sha256:" in ref or "/" in ref.split(":")[0]


def image_present(exe: str, ref: str) -> bool:
    try:
        return _run([exe, "image", "inspect", ref], timeout=30).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def source_tree() -> Path | None:
    """The repository root, when running from a checkout that can build the image."""
    for parent in Path(__file__).resolve().parents:
        if (parent / "docker" / "Dockerfile").is_file() and (parent / ".dockerignore").is_file():
            return parent
    return None


def stream(cmd: list, on_line, cwd: Path | None = None) -> int:
    """Runs a command, handing each output line (stdout and stderr) to on_line."""
    proc = subprocess.Popen([str(c) for c in cmd], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace", bufsize=1, **_NO_WINDOW)
    assert proc.stdout
    for line in proc.stdout:
        line = line.rstrip()
        if line:
            on_line(line)
    return proc.wait()


def pull(exe: str, ref: str, on_line) -> int:
    return stream([exe, "pull", ref], on_line)


def build_image(exe: str, tree: Path, tag: str, on_line) -> int:
    return stream([exe, "build", "-t", tag, "-f", tree / "docker" / "Dockerfile", tree], on_line, cwd=tree)


# ---- the build container --------------------------------------------------------------------

@dataclass
class Plan:
    engine: str
    exe_path: str            # docker or podman
    image: str
    game_exe: Path
    game: Path
    work: Path | None        # a folder (Linux); None means the VOLUME (Windows)
    export: Path | None      # where the SD card folder is copied, when work is the volume
    steam: bool = False
    fmt: str = "nsp"         # build_all.py --format: nsp (installer + payload) or nro
    threads: int | None = None
    jobs: int | None = None
    check_only: bool = False
    source: Path | None = None  # a checkout using the image's installed tools


def _mount_field(value: str) -> str:
    """--mount is a CSV list: a value with a comma or a quote is quoted."""
    if "," in value or '"' in value:
        return '"%s"' % value.replace('"', '""')
    return value


def mount(kind: str, source, target: str, readonly: bool = False) -> list[str]:
    spec = ",".join([_mount_field("type=" + kind), _mount_field("source=%s" % source),
                     _mount_field("target=" + target)] + (["readonly"] if readonly else []))
    return ["--mount", spec]


def build_command(plan: Plan, platform: str | None = None, uid: tuple[int, int] | None = None) -> list[str]:
    platform = platform or sys.platform
    cmd = [plan.exe_path, "run", "--rm", "--name", CONTAINER, "--network", "none"]
    if plan.engine == "podman":
        cmd += ["--userns=keep-id"]
    elif platform != "win32":
        if uid is None:
            uid = (os.getuid(), os.getgid())
        cmd += ["-u", "%d:%d" % uid]
    cmd += mount("bind", plan.game_exe, "/input/isaac-ng.unpacked.exe", readonly=True)
    cmd += mount("bind", plan.game, "/game", readonly=True)
    if plan.work is None:
        cmd += mount("volume", VOLUME, "/work")
    else:
        cmd += mount("bind", plan.work, "/work")
    if plan.export is not None:
        cmd += mount("bind", plan.export, "/export")
    if plan.source is not None:
        cmd += mount("bind", plan.source / "host", "/opt/isaac/host", readonly=True)
        host = plan.source / "host"
        if (host / "launcher_art.py").is_file():
            from importlib.util import module_from_spec, spec_from_file_location
            spec = spec_from_file_location("launcher_art", host / "launcher_art.py")
            art_module = module_from_spec(spec)
            spec.loader.exec_module(art_module)
            for name, art in art_module.find_art(plan.source).items():
                cmd += mount("bind", art, "/" + name, readonly=True)
    cmd += [plan.image, "--progress", "json"]
    if plan.export is not None:
        cmd += ["--export", "/export"]
    if plan.steam:
        cmd += ["--steam"]
    if plan.fmt != "nsp":
        cmd += ["--format", plan.fmt]
    if plan.threads:
        cmd += ["--threads", str(plan.threads)]
    if plan.jobs:
        cmd += ["--jobs", str(plan.jobs)]
    if plan.check_only:
        cmd += ["--check-only"]
    return cmd


def parse_event(line: str) -> dict | None:
    line = line.strip()
    if not line.startswith("{"):
        return None
    try:
        event = json.loads(line)
    except ValueError:
        return None
    return event if isinstance(event, dict) and "event" in event else None


def run_build(plan: Plan, on_event, on_text) -> int:
    """Runs the container. JSON lines go to on_event, anything else to on_text."""
    try:
        _run([plan.exe_path, "rm", "-f", CONTAINER], timeout=60)   # left over from a crash
    except (OSError, subprocess.TimeoutExpired):
        pass
    if plan.work is not None:
        plan.work.mkdir(parents=True, exist_ok=True)
    if plan.export is not None:
        plan.export.mkdir(parents=True, exist_ok=True)
    proc = subprocess.Popen(build_command(plan), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            stdin=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace", bufsize=1, **_NO_WINDOW)
    assert proc.stdout and proc.stderr

    def errors():
        for line in proc.stderr:
            if line.strip():
                on_text(line.rstrip())

    t = threading.Thread(target=errors, daemon=True)
    t.start()
    for line in proc.stdout:
        event = parse_event(line)
        if event:
            on_event(event)
        elif line.strip():
            on_text(line.rstrip())
    rc = proc.wait()
    t.join(timeout=5)
    return rc


def stop(exe: str) -> None:
    try:
        _run([exe, "stop", "-t", "10", CONTAINER], timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        pass


def remove_work(exe: str, work: Path | None) -> bool:
    """Deletes the work folder or volume. A folder is only deleted when it holds
    what build_all.py makes (stamps/ or upstream/), or nothing at all."""
    if work is None:
        try:
            return _run([exe, "volume", "rm", "-f", VOLUME], timeout=120).returncode == 0
        except (OSError, subprocess.TimeoutExpired):
            return False
    if not work.exists():
        return True
    # Existing command-line work can be inside the source checkout.
    if (work / ".git").exists() or (work / "host" / "build_all.py").is_file():
        return False
    names = {p.name for p in work.iterdir()}
    if names and not ({"stamps", "upstream"} & names):
        return False
    shutil.rmtree(work, ignore_errors=True)
    return not work.exists()
