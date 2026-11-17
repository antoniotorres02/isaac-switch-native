#!/usr/bin/env python3
"""From your own copy of the game to a folder ready for the SD card.

The whole of BUILDING.md in one command, meant to run inside the image of
docker/Dockerfile (see GUIDE.md). Everything is checked before anything slow
starts, finished stages are remembered in <work>/stamps so a run that was cut
short carries on where it stopped, and each stage keeps its log in <work>/logs.

    build_all.py [--steam] [--check-only] [--progress json] [--export DIR]

With --progress json every line on stdout is one JSON object (see emit_json),
for the builder program in builder/; the text output is for people.

It reads the executable and the game folder and never writes to them. All it
produces stays under <work>, and the NRO it builds contains the memory image of
the executable: it is for you alone and must not be shared.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
UPSTREAM_INFO = json.loads((HERE / "upstream.json").read_text())

# Bump when a stage changes what it produces: older stamps stop counting.
PIPELINE_VERSION = 1

GIB = 1024 ** 3
MIN_FREE_DISK = 8 * GIB     # see "What you need" in GUIDE.md
MIN_MEMORY = 8 * GIB        # Ghidra alone asks for a 6 GiB heap
GHIDRA_HEAP = "6G"

# What the game has to contain before it is worth starting. The exact list comes
# later, from the game itself (upstream's ship.py).
GAME_FILES = ["resources/packed/%s.a" % n for n in (
    "afterbirth", "afterbirthp", "animations", "config", "fonts", "graphics", "music", "rooms", "sfx", "videos",
)] + ["resources/scripts/main.lua", "savedatapath.txt"]


class StageFailed(Exception):
    pass


class Stopped(Exception):
    """SIGTERM: `docker stop`, or the builder's Stop button."""


def on_sigterm(signum, frame):
    # As PID 1 in a container Python would ignore SIGTERM and be killed 10 s later.
    raise Stopped()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def memory_limit() -> int | None:
    """The memory this process can use: the machine's, or a container's limit."""
    limits = []
    try:
        limits.append(os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES"))
    except (ValueError, OSError):
        pass
    try:
        text = Path("/sys/fs/cgroup/memory.max").read_text().strip()
        if text != "max":
            limits.append(int(text))
    except (OSError, ValueError):
        pass
    return min(limits) if limits else None


# ---- checks: seconds, before anything slow -------------------------------------

def check_exe(exe: Path, expected: str) -> list[str]:
    if not exe.exists():
        return ["The executable was not found at %s." % exe]
    if not exe.is_file():
        # Docker makes an empty folder when the path it was told to mount does not exist.
        return ["%s is a folder, not a file. Check the executable's path in your docker run command." % exe]
    got = sha256_file(exe)
    if got.lower() != expected.lower():
        return ["The executable is not the one this build needs (see \"The executable\" in GUIDE.md).\n"
                "    expected sha256 %s\n    this file has   %s\n"
                "    Every address in the build belongs to one exact file, so another one cannot work."
                % (expected.lower(), got.lower())]
    return []


def check_game(game: Path) -> list[str]:
    if not game.is_dir():
        return ["The game folder was not found at %s." % game]
    missing = [name for name in GAME_FILES if not (game / name).is_file()]
    if len(missing) == len(GAME_FILES):
        return ["%s does not look like the game's folder: it has none of the files expected there. "
                "Mount the folder that contains resources/ and savedatapath.txt." % game]
    if missing:
        return ["The game folder is incomplete. Missing: %s." % ", ".join(missing)]
    return []


def check_work(work: Path, need_disk: int, memory: int | None) -> list[str]:
    problems = []
    try:
        work.mkdir(parents=True, exist_ok=True)
        probe = work / ".write-test"
        probe.write_text("")
        probe.unlink()
    except OSError as e:
        return ["%s cannot be written (%s). Check the -u option and the folder's owner." % (work, e.strerror)]
    free = shutil.disk_usage(work).free
    if free < need_disk:
        problems.append("Not enough disk in %s: %.1f GiB free, the build needs about %.0f GiB."
                        % (work, free / GIB, need_disk / GIB))
    if memory is not None and memory < MIN_MEMORY:
        problems.append("Not enough memory: %.1f GiB available, the build needs %.0f GiB. "
                        "On Docker Desktop this is a setting (Resources > Memory)."
                        % (memory / GIB, MIN_MEMORY / GIB))
    return problems


# ---- stages ----------------------------------------------------------------------

class Stage:
    """`produces`, when given, returns the file the stage exists to make: a stamp
    without it does not count (a work folder from before a rename, say)."""

    def __init__(self, name: str, title: str, run, depends_on_options: bool = False, produces=None):
        self.name, self.title, self.run = name, title, run
        self.depends_on_options = depends_on_options
        self.produces = produces


def emit_json(out, event: str, **fields) -> None:
    """One line of the machine-readable progress. The events: problems (list of
    texts, then exit 2), checked, stage (number, total, name, title, state
    "skipped" | "running" | "done", seconds), log (the stage log's last line, at
    most every LOG_EVERY seconds), failed (message, log, tail), exporting,
    exported (path, files), stopped, done (seconds, nro_sha256, sd_card)."""
    fields["event"] = event
    out.write(json.dumps(fields) + "\n")
    out.flush()


LOG_EVERY = 2.0


class Runner:
    """Runs stages in order. A stage whose stamp matches is skipped; once one has
    to run, the stages after it run too, since they consume its output."""

    def __init__(self, work: Path, exe_hash: str, options: str, out=sys.stdout, json_mode: bool = False):
        self.stamps = work / "stamps"
        self.logs = work / "logs"
        self.exe_hash = exe_hash
        self.options = options
        self.out = out
        self.json_mode = json_mode
        self.log_path: Path | None = None

    def key(self, stage: Stage) -> str:
        return "%d %s %s %s" % (PIPELINE_VERSION, stage.name, self.exe_hash,
                                self.options if stage.depends_on_options else "-")

    def say(self, text: str) -> None:
        if not self.json_mode:
            print(text, file=self.out, flush=True)

    def stage_event(self, number: int, total: int, stage: Stage, state: str, seconds: float = 0.0) -> None:
        if self.json_mode:
            emit_json(self.out, "stage", number=number, total=total, name=stage.name, title=stage.title,
                      state=state, seconds=round(seconds, 1))

    def done(self, stage: Stage) -> bool:
        stamp = self.stamps / stage.name
        if not (stamp.is_file() and stamp.read_text() == self.key(stage)):
            return False
        return stage.produces is None or Path(stage.produces()).is_file()

    def execute(self, stages: list[Stage]) -> None:
        self.stamps.mkdir(parents=True, exist_ok=True)
        self.logs.mkdir(parents=True, exist_ok=True)
        dirty = False
        for number, stage in enumerate(stages, 1):
            head = "[%d/%d] %s" % (number, len(stages), stage.title)
            if not dirty and self.done(stage):
                self.say("%s: already done" % head)
                self.stage_event(number, len(stages), stage, "skipped")
                continue
            dirty = True
            (self.stamps / stage.name).unlink(missing_ok=True)
            self.log_path = self.logs / (stage.name + ".log")
            self.log_path.write_text("")
            self.say("%s..." % head)
            self.stage_event(number, len(stages), stage, "running")
            started = time.monotonic()
            try:
                stage.run(self)
            except (StageFailed, Stopped):
                raise
            except Exception as e:      # a bug or a missing file: still a stage that stopped, with its log
                raise StageFailed("%s: %s" % (type(e).__name__, e)) from e
            (self.stamps / stage.name).write_text(self.key(stage))
            took = time.monotonic() - started
            self.say("%s: done in %s" % (head, duration(took)))
            self.stage_event(number, len(stages), stage, "done", took)

    def sh(self, cmd: list, cwd: Path | None = None, env: dict | None = None, stdout_to: Path | None = None) -> None:
        """A command, with its output in the stage's log."""
        cmd = [str(c) for c in cmd]
        full_env = dict(os.environ, **(env or {}))
        assert self.log_path
        with open(self.log_path, "a") as log:
            log.write("$ %s\n" % " ".join(cmd))
            log.flush()
            target = open(stdout_to, "w") if stdout_to else log
            try:
                proc = subprocess.Popen(cmd, cwd=cwd, env=full_env, stdout=target, stderr=log,
                                        stdin=subprocess.DEVNULL)
                returncode = self.wait(proc)
            except OSError as e:
                raise StageFailed("cannot run %s: %s" % (cmd[0], e))
            finally:
                if stdout_to:
                    target.close()
        if returncode != 0:
            raise StageFailed("%s exited with %d" % (" ".join(cmd[:4]), returncode))

    def wait(self, proc: subprocess.Popen) -> int:
        """The command's exit code. In JSON mode the log's last line goes out now
        and then, so that a long stage shows it is alive."""
        if not self.json_mode:
            return proc.wait()
        last = ""
        while True:
            try:
                return proc.wait(timeout=LOG_EVERY)
            except subprocess.TimeoutExpired:
                pass
            line = last_line(self.log_path) if self.log_path else ""
            if line and line != last:
                emit_json(self.out, "log", line=line[:300])
                last = line


def last_line(path: Path) -> str:
    """The last non-empty line of a growing log, reading only its end."""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - 4096))
            lines = [x for x in f.read().decode("utf-8", "replace").splitlines() if x.strip()]
    except OSError:
        return ""
    return lines[-1].strip() if lines else ""


def export_sd_card(sd: Path, export: Path) -> int:
    """A copy of the SD card folder at <export>/sd-card, for a work folder the
    user cannot open (a Docker volume, on Windows). Returns the file count."""
    target = export / "sd-card"
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(sd, target)
    return sum(1 for p in target.rglob("*") if p.is_file())


def duration(seconds: float) -> str:
    return "%d min %02d s" % (seconds // 60, seconds % 60) if seconds >= 60 else "%.0f s" % seconds


def build_stages(args, up: Path, out: Path) -> list[Stage]:
    lift = up / "output" / "recomp" / "lift"
    threads = args.threads or min(os.cpu_count() or 4, 8)
    jobs = args.jobs or max(1, (os.cpu_count() or 4) - 1)

    def prepare(r: Runner) -> None:
        if not (up / "scripts").is_dir():
            shutil.rmtree(up, ignore_errors=True)
            shutil.copytree(args.upstream_src, up, symlinks=True)
        shutil.copyfile(args.exe, up / "tools" / "isaac-ng.unpacked.exe")

    def ghidra(r: Runner) -> None:
        r.sh(["node", "scripts/recomp/export-all.mjs", "--stage", "all", "--threads", threads,
              "--timeout", 240, "--heap", GHIDRA_HEAP], cwd=up)

    def tables(r: Runner) -> None:
        for script in (["census/run_all.py"], ["host/boot_tables.py"], ["host/memimage.py"],
                       ["host/verify_memimage.py"], ["host/gen_shims.py"], ["host/lua_build.py", "--build"]):
            r.sh(["python", "scripts/recomp/" + script[0]] + script[1:], cwd=up)
        r.sh(["python", "scripts/decomp/tools/build-pe-index.py"], cwd=up)

    def starts(r: Runner) -> None:
        r.sh(["python", "scripts/recomp/lift/prepare_starts.py"], cwd=up)
        union = lift / "starts_union.txt"
        orig = lift / "starts_union.orig.txt"
        extra = lift / "extra_code_pointer_starts.txt"
        shutil.copyfile(union, orig)
        r.sh(["python", HERE / "extra_code_pointer_starts.py", "--exe", "tools/isaac-ng.unpacked.exe",
              "--starts", orig], cwd=up, stdout_to=extra)
        lines = orig.read_text().splitlines() + extra.read_text().splitlines()
        union.write_text("".join(line + "\n" for line in sorted(set(lines))))

    def do_lift(r: Runner) -> None:
        for d in (lift / "gu", lift / "gu.pristine"):
            shutil.rmtree(d, ignore_errors=True)
        r.sh(["python", "scripts/recomp/lift/lift_parallel.py", "--jobs", jobs,
              "--exe", "tools/isaac-ng.unpacked.exe",
              "--ghidra-functions", "output/recomp/export/functions.jsonl",
              "--fragments-tsv", "output/recomp/export/recovered-functions.tsv",
              "--imports", "output/recomp/census/imports.json",
              "--shim-table", "output/recomp/host/shim-table.json",
              "--va-file", "output/recomp/lift/starts_union.txt",
              "--hand-written", "scripts/recomp/host/src/missing_fns.c",
              "--dispatch", "auto", "--max-insns", 100000, "--split-va", "0x30000", "--trace-va",
              "--out", "output/recomp/lift/gu", "--module", "lifted",
              "--stats", "output/recomp/lift/gu/stats.json"], cwd=up)
        r.sh(["python", "scripts/recomp/lift/patch_reentry.py", "--dir", "output/recomp/lift/gu",
              "--exe", "tools/isaac-ng.unpacked.exe"], cwd=up)
        shutil.copytree(lift / "gu", lift / "gu.pristine")

    def boot(r: Runner) -> None:
        # build_boot.py rewrites gu/ in place, so a second run starts from the copy.
        shutil.rmtree(lift / "gu")
        shutil.copytree(lift / "gu.pristine", lift / "gu")
        r.sh(["python", "scripts/recomp/lift/build_boot.py", "--web", "--fast", "--jobs", min(jobs, 4)],
             cwd=up, env={"ISAAC_CALL_SLIM": "0"})

    def assets(r: Runner) -> None:
        r.sh(["python", "scripts/recomp/assets/bundle.py", "build", args.game, ".scratch/game-bundle"], cwd=up)
        r.sh(["python", "scripts/recomp/assets/ship.py", "build"], cwd=up)

    def nro(r: Runner) -> None:
        cmd = ["python", HERE / "build_native.py", "--profile", "switch", "--upstream", up,
               "--out", out / "browsing-native", "--jobs", jobs]
        r.sh(cmd + ([] if args.steam else ["--no-steam"]))

    def sd_card(r: Runner) -> None:
        make_sd_card(up / ".scratch" / "game-dist" / "instance_index.json", up / ".scratch" / "game-bundle",
                     nro_path(out, args.steam), out / "sd-card")

    return [
        Stage("prepare", "Preparing the sources", prepare),
        Stage("ghidra", "Analysing the executable with Ghidra (the long one)", ghidra),
        Stage("tables", "Building tables, memory image and Lua", tables),
        Stage("starts", "Finding function entries", starts),
        Stage("lift", "Translating the game to C", do_lift),
        Stage("boot", "Patching the translated code", boot),
        Stage("assets", "Indexing the game's files", assets),
        Stage("nro", "Compiling the NRO", nro, depends_on_options=True, produces=lambda: nro_path(out, args.steam)),
        Stage("sdcard", "Preparing the SD card folder", sd_card, depends_on_options=True),
    ]


def nro_path(out: Path, steam: bool) -> Path:
    return out / "browsing-native" / ("switch" if steam else "switch-nosteam") / "isaac-repentance-ns1-recomp.nro"


def place(src: Path, dst: Path) -> None:
    """A hard link where it can be one: the files are large, and already here."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def make_sd_card(index: Path, bundle: Path, nro: Path, sd: Path) -> int:
    """<sd>/switch/isaac-repentance-ns1-recomp.nro and the files the build lists, as PLAYING.md
    lays them out. Saves are not touched: they do not exist here."""
    shutil.rmtree(sd, ignore_errors=True)
    place(nro, sd / "switch" / "isaac-repentance-ns1-recomp.nro")
    rows = json.loads(index.read_text())
    guest = sd / "switch" / "isaac-recomp" / "guest"
    for row in rows:
        src = bundle / row["p"]
        if not src.is_file():
            raise StageFailed("the build lists %s, but it is not in the game's files" % row["p"])
        place(src, guest / row["p"])
    return len(rows)


# ---- main ------------------------------------------------------------------------

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", type=Path, default=Path("/input/isaac-ng.unpacked.exe"))
    ap.add_argument("--game", type=Path, default=Path("/game"))
    ap.add_argument("--work", type=Path, default=Path("/work"))
    ap.add_argument("--upstream-src", type=Path, default=Path("/opt/isaac/upstream"),
                    help="the prepared upstream checkout, copied into <work> at the first run")
    ap.add_argument("--steam", action="store_true",
                    help="include the Steam sign-in and Cloud sync (an unofficial client: read STEAM-CLOUD.md first)")
    ap.add_argument("--check-only", action="store_true", help="run the checks and stop")
    ap.add_argument("--threads", type=int, help="Ghidra threads (default: up to 8)")
    ap.add_argument("--jobs", type=int, help="compile jobs (default: all CPUs but one)")
    ap.add_argument("--progress", choices=("text", "json"), default="text",
                    help="json: one JSON object per line on stdout, for the builder program")
    ap.add_argument("--export", type=Path,
                    help="also copy the SD card folder to <EXPORT>/sd-card at the end")
    args = ap.parse_args(argv)
    json_mode = args.progress == "json"

    def tell(text: str) -> None:
        if not json_mode:
            print(text, flush=True)

    expected = UPSTREAM_INFO["game_pe_sha256"]
    tell("Checking your files (this reads the executable once)...")
    problems = check_exe(args.exe, expected) + check_game(args.game)
    started_before = (args.work / "upstream" / "output").is_dir()
    problems += check_work(args.work, MIN_FREE_DISK // 2 if started_before else MIN_FREE_DISK, memory_limit())
    if args.export and not problems:
        problems += check_work(args.export, 2 * GIB, None)
    if problems:
        if json_mode:
            emit_json(sys.stdout, "problems", problems=problems)
        print("\nCannot start:\n", file=sys.stderr)
        for p in problems:
            print("  - " + p, file=sys.stderr)
        return 2
    tell("Executable, game folder, disk and memory are fine.")
    if json_mode:
        emit_json(sys.stdout, "checked", resumed=started_before)
    if args.check_only:
        return 0

    # Tools that want a home or a cache of their own, whichever user this runs as.
    (args.work / ".home").mkdir(exist_ok=True)
    os.environ["HOME"] = str(args.work / ".home")
    os.environ["EM_CACHE"] = str(args.work / ".cache" / "emscripten")

    up, out = args.work / "upstream", args.work / "out"
    options = "steam" if args.steam else "no-steam"
    runner = Runner(args.work, expected.lower(), options, json_mode=json_mode)
    started = time.monotonic()
    signal.signal(signal.SIGTERM, on_sigterm)
    try:
        runner.execute(build_stages(args, up, out))
    except Stopped:
        if json_mode:
            emit_json(sys.stdout, "stopped")
        print("\nStopped. Run the same command again to carry on from this step.", file=sys.stderr)
        return 143
    except StageFailed as e:
        tail = []
        if runner.log_path and runner.log_path.exists():
            tail = [line[:200] for line in runner.log_path.read_text(errors="replace").splitlines()[-25:]]
        if json_mode:
            emit_json(sys.stdout, "failed", message=str(e), log=str(runner.log_path or ""), tail=tail)
        print("\nThe build stopped: %s" % e, file=sys.stderr)
        if tail:
            print("Last lines of %s:\n" % runner.log_path, file=sys.stderr)
            for line in tail:
                print("  " + line, file=sys.stderr)
        print("\nRun the same command again to carry on from this step.", file=sys.stderr)
        return 1

    nro = nro_path(out, args.steam)
    sd = out / "sd-card"
    if args.export:
        tell("Copying the SD card folder to %s..." % (args.export / "sd-card"))
        if json_mode:
            emit_json(sys.stdout, "exporting", path=str(args.export / "sd-card"))
        files = export_sd_card(sd, args.export)
        sd = args.export / "sd-card"
        if json_mode:
            emit_json(sys.stdout, "exported", path=str(sd), files=files)
    took = time.monotonic() - started
    digest = sha256_file(nro)
    if json_mode:
        emit_json(sys.stdout, "done", seconds=round(took, 1), nro_sha256=digest, sd_card=str(sd))
        return 0
    print("\nDone in %s." % duration(took))
    print("NRO sha256: %s" % digest)
    print("Copy the contents of %s to the root of your SD card." % sd)
    print("The NRO contains the game's executable image: it is for you alone, do not share it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
