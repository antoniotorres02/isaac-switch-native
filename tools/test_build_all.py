#!/usr/bin/env python3
"""build_all.py's checks and its resuming, without Docker or the game."""
import hashlib
import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "host/build_all.py"
spec = importlib.util.spec_from_file_location("build_all", SCRIPT)
build_all = importlib.util.module_from_spec(spec)
sys.modules["build_all"] = build_all
spec.loader.exec_module(build_all)

GIB = build_all.GIB


def fake_game(root: Path) -> Path:
    for name in build_all.GAME_FILES:
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_bytes(b"x")
    return root


def fake_versions(root: Path, entries) -> Path:
    """A versions/ folder: (name, supported, pe bytes, steam exe bytes, buildid) each."""
    for name, supported, pe, steam_exe, buildid in entries:
        (root / name).mkdir(parents=True)
        (root / name / "version.json").write_text(json.dumps({
            "version": name, "supported": supported,
            "steam": {"buildid": buildid, "exe_sha256": hashlib.sha256(steam_exe).hexdigest().upper()},
            "pe": {"sha256": hashlib.sha256(pe).hexdigest().upper(), "size": len(pe)}}))
    return root


def test_exe(tmp: Path) -> None:
    reg = fake_versions(tmp / "versions", [("1.0.0.1", False, b"old pe", b"old drm", "10"),
                                           ("1.0.0.2", True, b"the right bytes", b"new drm", "20")])
    def ident(name, data=None):
        path = tmp / name
        if data is not None:
            path.write_bytes(data)
        return build_all.identify_exe(path, reg)
    problems, v = ident("game.exe", b"the right bytes")
    assert problems == [] and v.name == "1.0.0.2", (problems, v)
    problems, v = ident("old.exe", b"old pe")
    assert v is None and "v1.0.0.1" in problems[0] and "no longer supports" in problems[0], problems
    problems, v = ident("drm.exe", b"new drm")
    assert v is None and "DRM" in problems[0] and "v1.0.0.2" in problems[0], problems
    problems, v = ident("other.exe", b"something else")
    got = hashlib.sha256(b"something else").hexdigest()
    assert v is None and got in problems[0] and "v1.0.0.2 (Steam build 20)" in problems[0], problems
    assert "not found" in ident("absent.exe")[0][0]
    # docker run makes an empty folder when the path it should mount is absent
    (tmp / "folder.exe").mkdir()
    assert "is a folder" in ident("folder.exe")[0][0]


def test_shipped_versions(tmp: Path) -> None:
    """The versions/ in the repository: one supported version, whose files are what version.json says."""
    sys.path.insert(0, str(SCRIPT.parent))
    import functions_data
    import versions
    supported = versions.supported()
    assert len(supported) == 1, supported
    v = supported[0]
    assert v.overlay and v.overlay.is_file() and v.overlay.stat().st_size < 1 << 20
    overlay = v.overlay.read_text(encoding="utf-8")
    targets = [line[6:] for line in overlay.splitlines() if line.startswith("+++ b/")]
    assert targets and all(t.startswith("scripts/") and "/generated/" not in t for t in targets), targets
    assert 'EXPECTED_SHA256 = "%s"' % v.pe_sha256.upper() in overlay, "the overlay must expect this exe"
    common = sorted((SCRIPT.parent / "upstream-patches").glob("*.patch"))
    touched = {line[6:] for f in common for line in f.read_text(encoding="utf-8").splitlines()
               if line.startswith("+++ b/")}
    assert touched and all(t.startswith("scripts/") for t in touched), touched
    assert not touched & set(targets), "a common patch and the overlay must not touch the same file"
    rows = functions_data.load(v.folder)          # checks count and digest
    assert rows == sorted(rows, key=lambda r: r.va) and len({r.va for r in rows}) == len(rows)
    for old in versions.load():
        if old.name != v.name:
            assert not old.supported and old.pe_sha256 != v.pe_sha256


def test_functions_data(tmp: Path) -> None:
    sys.path.insert(0, str(SCRIPT.parent))
    import functions_data as fd
    export = tmp / "ghidra"
    export.mkdir()
    rows = [dict(va=0x401000, minVa=0x401000, endVa=0x401020, inText=True, external=False, name="a"),
            dict(va=0x401030, minVa=0x401028, endVa=0x401100, inText=True, external=False, name="b"),
            dict(va=0x401200, minVa=0x401200, endVa=0x401201, inText=True, external=False, name="c")]
    with open(export / "functions.raw.jsonl", "w") as f:
        for r in rows:
            f.write(json.dumps({k: ("0x%08x" % v if isinstance(v, int) and not isinstance(v, bool) else v)
                                for k, v in r.items()}) + "\n")
    (export / "recovered-functions.tsv").write_text(
        "round\tva\trunBytes\tbodyBytes\tfirstInsn\tlooksLikePrologue\n"
        "1\t0x00401030\t9\t9\tPUSH EBP\ttrue\n1\t0x00401200\t1\t1\tRET\tfalse\n")
    vdir = tmp / "v"
    vdir.mkdir()
    assert fd.main(["make", str(export), str(vdir)]) == 0
    assert fd.main(["check", str(vdir), str(export)]) == 0
    out = tmp / "out"
    assert fd.main(["write", str(vdir), str(out)]) == 0
    written = [json.loads(line) for line in (out / "functions.jsonl").read_text().splitlines()]
    assert written[1] == {"va": "0x00401030", "minVa": "0x00401028", "endVa": "0x00401100", "inText": True,
                          "external": False, "recovered": True}, written[1]
    assert [w["recovered"] for w in written] == [False, True, True]
    assert (out / "recovered-functions.tsv").read_text() == \
        "va\tlooksLikePrologue\n0x00401030\ttrue\n0x00401200\tfalse\n"
    # a list that no longer matches its version.json is refused
    text = (vdir / "functions.txt").read_text()
    assert "\n401000 20\n" in text, text
    (vdir / "functions.txt").write_text(text.replace("\n401000 20\n", "\n401000 21\n"))
    try:
        fd.load(vdir)
    except SystemExit:
        pass
    else:
        raise AssertionError("a changed functions.txt must not load")


def test_game(tmp: Path) -> None:
    game = fake_game(tmp / "game")
    assert build_all.check_game(game) == []
    (game / "resources/packed/music.a").unlink()
    only = build_all.check_game(game)
    assert len(only) == 1 and "resources/packed/music.a" in only[0] and "incomplete" in only[0], only
    empty = tmp / "empty"
    empty.mkdir()
    assert "does not look like" in build_all.check_game(empty)[0]
    assert "not found" in build_all.check_game(tmp / "absent")[0]


def test_work(tmp: Path) -> None:
    work = tmp / "work"
    assert build_all.check_work(work, 1, 16 * GIB) == []
    assert work.is_dir() and not list(work.iterdir()), "the write test leaves nothing behind"
    assert "Not enough disk" in build_all.check_work(work, 1 << 60, 16 * GIB)[0]
    assert "Not enough memory" in build_all.check_work(work, 1, 3 * GIB)[0]
    assert build_all.check_work(work, 1, None) == [], "unknown memory is not a reason to refuse"
    if os.getuid() != 0:
        locked = tmp / "locked"
        locked.mkdir()
        locked.chmod(0o500)
        try:
            assert "cannot be written" in build_all.check_work(locked / "w", 1, 16 * GIB)[0]
        finally:
            locked.chmod(0o700)


def stages(ran: list, fail_at: str = "") -> list:
    def step(name):
        def run(r):
            ran.append(name)
            if name == fail_at:
                raise build_all.StageFailed("failed on purpose")
        return run
    return [build_all.Stage("a", "A", step("a")), build_all.Stage("b", "B", step("b")),
            build_all.Stage("c", "C", step("c"), depends_on_options=True),
            build_all.Stage("d", "D", step("d"), depends_on_options=True)]


def test_resume(tmp: Path) -> None:
    work = tmp / "work"

    def go(exe="h1", options="no-steam", fail_at=""):
        ran = []
        runner = build_all.Runner(work, exe, options, out=io.StringIO())
        try:
            runner.execute(stages(ran, fail_at))
        except build_all.StageFailed:
            ran.append("stopped")
        return ran

    assert go(fail_at="c") == ["a", "b", "c", "stopped"]
    assert go() == ["c", "d"], "a cut run carries on at the stage that stopped"
    assert go() == [], "nothing left to do"
    assert go(options="steam") == ["c", "d"], "another option redoes only what depends on it"
    assert go(options="steam") == []
    assert go(exe="h2", options="steam") == ["a", "b", "c", "d"], "another executable redoes everything"
    (work / "stamps" / "b").unlink()
    assert go(exe="h2", options="steam") == ["b", "c", "d"], "what follows a redone stage is redone"
    (work / "stamps" / "d").write_text("stale")
    assert go(exe="h2", options="steam") == ["d"]


def test_produces(tmp: Path) -> None:
    work = tmp / "work"
    made = tmp / "made.bin"
    ran = []

    def make(r):
        ran.append("make")
        made.write_bytes(b"x")

    stages_ = [build_all.Stage("m", "M", make, produces=lambda: made)]
    build_all.Runner(work, "h", "o", out=io.StringIO()).execute(stages_)
    build_all.Runner(work, "h", "o", out=io.StringIO()).execute(stages_)
    assert ran == ["make"]
    made.unlink()
    build_all.Runner(work, "h", "o", out=io.StringIO()).execute(stages_)
    assert ran == ["make", "make"], "a stamp whose output is gone does not count"


def test_unexpected_error(tmp: Path) -> None:
    def broken(r):
        open(tmp / "absent" / "file")

    try:
        build_all.Runner(tmp / "work", "h", "o", out=io.StringIO()).execute([build_all.Stage("x", "X", broken)])
    except build_all.StageFailed as e:
        assert "FileNotFoundError" in str(e), e
    else:
        raise AssertionError("any error in a stage becomes StageFailed")


def test_sigterm(tmp: Path) -> None:
    import signal

    def killed(r):
        os.kill(os.getpid(), signal.SIGTERM)
        time.sleep(5)

    previous = signal.signal(signal.SIGTERM, build_all.on_sigterm)
    started = time.monotonic()
    try:
        build_all.Runner(tmp / "work", "h", "o", out=io.StringIO()).execute([build_all.Stage("x", "X", killed)])
    except build_all.Stopped:
        pass
    else:
        raise AssertionError("SIGTERM stops the build")
    finally:
        signal.signal(signal.SIGTERM, previous)
    assert time.monotonic() - started < 2, "at once, not when docker gives up"
    assert not (tmp / "work" / "stamps" / "x").exists(), "a stopped stage is redone next time"


def test_sh(tmp: Path) -> None:
    work = tmp / "work"
    runner = build_all.Runner(work, "h", "o", out=io.StringIO())
    runner.logs.mkdir(parents=True)
    runner.log_path = runner.logs / "x.log"
    runner.log_path.write_text("")
    runner.sh([sys.executable, "-c", "import sys; print('out'); print('err', file=sys.stderr)"])
    log = runner.log_path.read_text()
    assert "out" in log and "err" in log
    captured = tmp / "captured.txt"
    runner.sh([sys.executable, "-c", "print('only here')"], stdout_to=captured)
    assert captured.read_text() == "only here\n"
    assert "\nonly here" not in runner.log_path.read_text(), "the output went to the file, not the log"
    for cmd in ([sys.executable, "-c", "raise SystemExit(3)"], ["/nonexistent/tool"]):
        try:
            runner.sh(cmd)
        except build_all.StageFailed:
            continue
        raise AssertionError("a failing command must stop the stage")


def test_sd_card(tmp: Path) -> None:
    bundle = fake_game(tmp / "bundle")
    (bundle / "resources/packed/not-listed.a").write_bytes(b"extra")
    rows = [{"p": name, "s": 1} for name in build_all.GAME_FILES]
    index = tmp / "index.json"
    index.write_text(json.dumps(rows))
    nro = tmp / "isaac-repentance-ns1-recomp.nro"
    nro.write_bytes(b"nro")
    sd = tmp / "out" / "sd-card"
    (sd / "stale").mkdir(parents=True)
    assert build_all.make_sd_card(index, bundle, nro, sd) == len(rows)
    assert (sd / "switch/isaac-repentance-ns1-recomp.nro").read_bytes() == b"nro"
    guest = sd / "switch/isaac-recomp/guest"
    assert (guest / "resources/packed/music.a").is_file() and (guest / "savedatapath.txt").is_file()
    assert not (guest / "resources/packed/not-listed.a").exists(), "only what the build lists"
    assert not (sd / "stale").exists(), "a second run starts clean"
    assert not (sd / "switch/isaac-recomp/saves").exists(), "saves are never part of it"
    (bundle / "savedatapath.txt").unlink()
    try:
        build_all.make_sd_card(index, bundle, nro, sd)
    except build_all.StageFailed as e:
        assert "savedatapath.txt" in str(e)
    else:
        raise AssertionError("a listed file that is absent must stop the stage")


def test_output_formats(tmp: Path) -> None:
    up, out = tmp / "upstream", tmp / "out"
    index = up / ".scratch" / "game-dist" / "instance_index.json"
    index.parent.mkdir(parents=True)
    index.write_text(json.dumps([{"p": "savedatapath.txt"}]))
    bundle = up / ".scratch" / "game-bundle"
    bundle.mkdir()
    (bundle / "savedatapath.txt").write_bytes(b"game files")
    installer = out / "installer" / build_all.INSTALLER_NRO
    installer.parent.mkdir(parents=True)
    installer.write_bytes(b"installer")
    payload = build_all.payload_dir(out)
    payload.mkdir(parents=True)
    (payload / "install.txt").write_bytes(b"manifest")
    (payload / "program.nca").write_bytes(b"payload")

    for steam in (True, False):
        nro = build_all.nro_path(out, steam)
        nro.parent.mkdir(parents=True)
        nro.write_bytes(b"steam game" if steam else b"offline game")
        for fmt in ("both", "nsp", "both", "nro"):
            args = SimpleNamespace(jobs=1, version=object(), format=fmt, steam=steam)
            stages = {stage.name: stage for stage in build_all.build_stages(args, up, out)}
            assert ("nsp" in stages) == (fmt != "nro")
            stages["sdcard"].run(None)
            switch = out / "sd-card" / "switch"
            direct = switch / nro.name
            assert direct.exists() == (fmt != "nsp")
            if direct.exists():
                assert direct.read_bytes() == nro.read_bytes()
                assert (switch / "isaac-recomp/guest/savedatapath.txt").is_file()
            assert (switch / installer.name).exists() == (fmt != "nro")
            assert (switch / "isaac-recomp/install/program.nca").exists() == (fmt != "nro")


def test_main_refuses_quickly(tmp: Path) -> None:
    work = tmp / "work"
    game = fake_game(tmp / "game")
    exe = tmp / "wrong.exe"
    exe.write_bytes(b"not the game")
    stderr, sys.stderr = sys.stderr, io.StringIO()
    stdout, sys.stdout = sys.stdout, io.StringIO()
    try:
        rc = build_all.main(["--exe", str(exe), "--game", str(game), "--work", str(work)])
        said = sys.stderr.getvalue()
    finally:
        sys.stderr, sys.stdout = stderr, stdout
    assert rc == 2 and "Cannot start" in said and "sha256" in said, said
    assert not (work / "stamps").exists(), "nothing started"


def test_json_events(tmp: Path) -> None:
    out = io.StringIO()
    runner = build_all.Runner(tmp / "work", "h", "o", out=out, json_mode=True)
    ran = []
    runner.execute(stages(ran))
    first = [json.loads(line) for line in out.getvalue().splitlines()]
    assert [(e["name"], e["state"]) for e in first] == [
        ("a", "running"), ("a", "done"), ("b", "running"), ("b", "done"),
        ("c", "running"), ("c", "done"), ("d", "running"), ("d", "done")], first
    assert all(e["event"] == "stage" and e["total"] == 4 for e in first)
    assert [e["number"] for e in first[::2]] == [1, 2, 3, 4]
    out.truncate(0)
    out.seek(0)
    build_all.Runner(tmp / "work", "h", "o", out=out, json_mode=True).execute(stages([]))
    again = [json.loads(line) for line in out.getvalue().splitlines()]
    assert [e["state"] for e in again] == ["skipped"] * 4, again


def test_json_log_lines(tmp: Path) -> None:
    out = io.StringIO()
    runner = build_all.Runner(tmp / "work", "h", "o", out=out, json_mode=True)
    runner.logs.mkdir(parents=True)
    runner.log_path = runner.logs / "x.log"
    runner.log_path.write_text("")
    every, build_all.LOG_EVERY = build_all.LOG_EVERY, 0.05
    try:
        runner.sh([sys.executable, "-c",
                   "import sys, time\nfor i in range(4):\n    print('step %d' % i, file=sys.stderr, flush=True)\n"
                   "    time.sleep(0.2)"])
    finally:
        build_all.LOG_EVERY = every
    events = [json.loads(line) for line in out.getvalue().splitlines()]
    lines = [e["line"] for e in events if e["event"] == "log"]
    assert lines and all(x.startswith("step ") for x in lines), events
    assert len(lines) == len(set(lines)), "a line is sent once, not at every tick"


def test_export(tmp: Path) -> None:
    sd = tmp / "sd-card"
    (sd / "switch/isaac-recomp/guest").mkdir(parents=True)
    (sd / "switch/isaac-repentance-ns1-recomp.nro").write_bytes(b"nro")
    (sd / "switch/isaac-recomp/guest/savedatapath.txt").write_bytes(b"x")
    export = tmp / "export"
    (export / "sd-card" / "old").mkdir(parents=True)
    (export / "keep.txt").write_text("the user's")
    assert build_all.export_sd_card(sd, export) == 2
    assert (export / "sd-card/switch/isaac-repentance-ns1-recomp.nro").read_bytes() == b"nro"
    assert not (export / "sd-card/old").exists(), "the copy replaces an older one"
    assert (export / "keep.txt").read_text() == "the user's", "nothing outside sd-card is touched"


def test_main_json_problems(tmp: Path) -> None:
    game = fake_game(tmp / "game")
    exe = tmp / "wrong.exe"
    exe.write_bytes(b"not the game")
    stderr, sys.stderr = sys.stderr, io.StringIO()
    stdout, sys.stdout = sys.stdout, io.StringIO()
    try:
        rc = build_all.main(["--exe", str(exe), "--game", str(game), "--work", str(tmp / "work"),
                             "--progress", "json"])
        said = sys.stdout.getvalue()
    finally:
        sys.stderr, sys.stdout = stderr, stdout
    events = [json.loads(line) for line in said.splitlines()]
    assert rc == 2 and len(events) == 1 and events[0]["event"] == "problems", said
    assert "sha256" in events[0]["problems"][0]


def main() -> int:
    for test in (test_exe, test_shipped_versions, test_functions_data, test_game, test_work, test_resume,
                 test_produces, test_unexpected_error, test_sigterm, test_sh,
                 test_sd_card, test_output_formats, test_main_refuses_quickly,
                 test_json_events, test_json_log_lines, test_export, test_main_json_problems):
        with tempfile.TemporaryDirectory() as d:
            test(Path(d))
        print("%-32s PASS" % test.__name__)
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
