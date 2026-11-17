#!/usr/bin/env python3
"""The builder program (host/builder) without Docker, Steam or the game."""
import hashlib
import io
import os
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "host/builder"))

from isb import checks, dockerctl, progress, sdcard, steam, system, texts  # noqa: E402
from isb.checks import FAIL, OK, WARN  # noqa: E402

MANIFEST = '''"AppState"
{
	"appid"		"250900"
	"installdir"		"The Binding of Isaac Rebirth"
	"buildid"		"%s"
	"InstalledDepots"
	{
		"250902" { "manifest" "1" "size" "2" }
%s	}
}
'''


def dlc_lines(appids):
    return "".join('\t\t"%s" { "manifest" "1" "dlcappid" "%s" }\n' % (int(a) + 7, a) for a in appids)


def fake_library(lib: Path, buildid="22878971", dlcs=tuple(steam.DLC)) -> Path:
    apps = lib / "steamapps"
    game = apps / "common" / steam.INSTALL_DIR
    for name in checks.build_all.GAME_FILES:
        (game / name).parent.mkdir(parents=True, exist_ok=True)
        (game / name).write_bytes(b"x")
    (apps / "appmanifest_250900.acf").write_text(MANIFEST % (buildid, dlc_lines(dlcs)))
    return game


def test_vdf(tmp: Path) -> None:
    data = steam.parse_vdf('''// a comment
"libraryfolders"
{
	"0" { "path" "C:\\\\Program Files (x86)\\\\Steam" "apps" { "250900" "1" } }
	"1"
	{
		"path"		"/mnt/games/Steam Library"
	}
	"quote" "say \\"hi\\""
}''')
    lf = data["libraryfolders"]
    assert lf["0"]["path"] == "C:\\Program Files (x86)\\Steam", lf
    assert lf["0"]["apps"] == {"250900": "1"}
    assert lf["1"]["path"] == "/mnt/games/Steam Library"
    assert lf["quote"] == 'say "hi"'
    assert steam.parse_vdf("") == {}
    assert steam.parse_vdf('"a" { "b" ') == {"a": {}}, "a cut file does not raise"


def vdf_str(path: Path) -> str:
    return str(path).replace("\\", "\\\\")


def test_find_game(tmp: Path) -> None:
    home = tmp / "home"
    assert steam.find_game(home, "linux") is None
    root = home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"   # Flatpak
    (root / "steamapps").mkdir(parents=True)
    other = tmp / "disk2" / "Steam Library"
    game = fake_library(other)
    (root / "steamapps/libraryfolders.vdf").write_text(
        # Steam escapes backslashes in its .vdf files, as Windows paths have them.
        '"libraryfolders" { "0" { "path" "%s" } "1" { "path" "%s" } }' % (vdf_str(root), vdf_str(other)))
    found = steam.find_game(home, "linux")
    assert found and found.path == game, found
    assert found.buildid == "22878971" and found.missing_dlcs == []
    # the native install wins over the Flatpak one
    native = home / ".local/share/Steam"
    native_game = fake_library(native, dlcs=("401920",))
    found = steam.find_game(home, "linux")
    assert found.path == native_game and found.missing_dlcs == ["Afterbirth+", "Repentance", "Repentance+"]
    assert steam.describe_folder(native_game).manifest is not None
    assert [g.path for g in steam.find_games(home, "linux")] == [native_game, game], "every install is offered"
    loose = tmp / "loose"
    loose.mkdir()
    assert steam.describe_folder(loose).manifest is None and steam.describe_folder(loose).missing_dlcs == []


def test_check_game(tmp: Path) -> None:
    game = steam.describe_folder(fake_library(tmp / "lib"))
    states = {i.key: i.state for i in checks.check_game(game)}
    assert states["game.found"] == OK and states["game.dlc_ok"] == OK, states
    (game.path / "isaac-ng.exe").write_bytes(b"another build")
    assert {i.key: i.state for i in checks.check_game(game)}["game.version_differs"] == WARN
    old = steam.describe_folder(fake_library(tmp / "old", buildid="1", dlcs=("401920", "570660", "1426300")))
    items = {i.key: i for i in checks.check_game(old)}
    assert items["game.build_differs"].state == WARN and items["game.dlc_missing"].state == FAIL
    assert "Repentance+" in items["game.dlc_missing"].args["dlcs"]
    (old.path / "savedatapath.txt").unlink()
    items = {i.key: i for i in checks.check_game(old)}
    assert items["game.incomplete"].state == FAIL and items["game.incomplete"].fix == "verify_steam"
    empty = tmp / "empty"
    empty.mkdir()
    assert checks.check_game(steam.GameInstall(empty))[0].key == "game.not_game"
    assert checks.check_game(None)[0].fix == "choose_game"


def test_exe(tmp: Path) -> None:
    right, original = tmp / "right.exe", tmp / "isaac-ng.exe"
    right.write_bytes(b"R" * 64)
    original.write_bytes(b"O" * 70)
    saved = checks.PE_SHA256, checks.STEAM_EXE_SHA256, checks.PE_SIZE
    checks.PE_SHA256 = hashlib.sha256(b"R" * 64).hexdigest()
    checks.STEAM_EXE_SHA256 = hashlib.sha256(b"O" * 70).hexdigest()
    checks.PE_SIZE = 64
    try:
        seen = []
        assert checks.classify_exe(right, seen.append)[0] == "ok" and seen[-1] == 1.0
        assert checks.check_exe(original)[0].key == "exe.original"
        assert checks.check_exe(tmp / "x.py")[0].key == "exe.missing"
        assert checks.check_exe(tmp)[0].key == "exe.folder"
        (tmp / "x.bin").write_bytes(b"Z" * 64)
        other = checks.check_exe(tmp / "x.bin")[0]
        assert other.key == "exe.other" and other.args["want"] == checks.PE_SHA256
        deep = tmp / "a" / "b"
        deep.mkdir(parents=True)
        (deep / "same-size-wrong.exe").write_bytes(b"Y" * 64)
        (deep / "renamed.exe").write_bytes(b"R" * 64)
        assert checks.find_exe([tmp / "a"]) == deep / "renamed.exe", "found by content, not name"
        assert checks.find_exe([tmp / "a"], depth=0) is None
        (deep / "Isaac-unpacked-old.EXE").write_bytes(b"short")
        (deep / "notes.txt").write_bytes(b"nothing")
        game = tmp / "game"
        game.mkdir()
        (game / "isaac-ng.exe").write_bytes(b"O" * 70)
        offered = checks.exe_candidates([game, tmp / "a"], game=game)
        assert offered[-1] == deep / "Isaac-unpacked-old.EXE", "named like Isaac's, but the wrong size: last"
        assert set(offered) == {deep / "same-size-wrong.exe", deep / "renamed.exe", deep / "Isaac-unpacked-old.EXE"}, \
            "the game's own isaac-ng.exe still has the DRM and is never suggested"
    finally:
        checks.PE_SHA256, checks.STEAM_EXE_SHA256, checks.PE_SIZE = saved


def plan(**kw) -> dockerctl.Plan:
    base = dict(engine="docker", exe_path="docker", image="img@sha256:abc",
                game_exe=Path("/srv/u/My Files/isaac.exe"),
                game=Path("/srv/u/.steam/steamapps/common/The Binding of Isaac Rebirth"),
                work=Path("/srv/u/IsaacSwitchBuild/work"), export=None)
    base.update(kw)
    return dockerctl.Plan(**base)


def mounts(cmd):
    return [cmd[i + 1] for i, c in enumerate(cmd) if c == "--mount"]


def test_command_linux(tmp: Path) -> None:
    cmd = dockerctl.build_command(plan(), "linux", (1000, 1000))
    assert cmd[:3] == ["docker", "run", "--rm"] and "--network" in cmd and cmd[cmd.index("--network") + 1] == "none"
    assert cmd[cmd.index("-u") + 1] == "1000:1000"
    m = mounts(cmd)
    # Paths in this system's own form (backslashes on Windows, which Docker Desktop accepts).
    assert m[0] == "type=bind,source=%s,target=/input/isaac-ng.unpacked.exe,readonly" % Path("/srv/u/My Files/isaac.exe"), m
    assert m[1].endswith("target=/game,readonly") and "The Binding of Isaac Rebirth" in m[1]
    assert m[2] == "type=bind,source=%s,target=/work" % Path("/srv/u/IsaacSwitchBuild/work")
    tail = cmd[cmd.index("img@sha256:abc"):]
    assert tail == ["img@sha256:abc", "--progress", "json"], tail
    cmd = dockerctl.build_command(plan(steam=True, threads=4, jobs=3), "linux", (5, 6))
    assert cmd[-5:] == ["--steam", "--threads", "4", "--jobs", "3"]


def test_command_windows(tmp: Path) -> None:
    p = plan(game_exe=Path("C:/Users/u/Downloads/isaac.exe"), game=Path("D:/SteamLibrary,2/Isaac"),
             work=None, export=Path("C:/Users/u/Documents/IsaacSwitchBuild"))
    cmd = dockerctl.build_command(p, "win32")
    assert "-u" not in cmd, "Docker Desktop maps ownership itself"
    m = mounts(cmd)
    assert m[1].startswith('type=bind,"source=%s",target=/game' % Path("D:/SteamLibrary,2/Isaac")), m[1]
    assert m[2] == "type=volume,source=%s,target=/work" % dockerctl.VOLUME
    assert m[3].endswith("target=/export")
    assert cmd[-4:] == ["--progress", "json", "--export", "/export"]
    pod = dockerctl.build_command(plan(engine="podman", exe_path="podman"), "linux", (1, 1))
    assert "--userns=keep-id" in pod and "-u" not in pod


def test_events(tmp: Path) -> None:
    assert dockerctl.parse_event('{"event": "log", "line": "x"}') == {"event": "log", "line": "x"}
    for junk in ("", "plain text", "{not json", '{"no": "event"}', "[1, 2]"):
        assert dockerctl.parse_event(junk) is None, junk


def test_image_ref(tmp: Path) -> None:
    saved = dict(dockerctl.IMAGE_INFO)
    env = os.environ.pop("ISAAC_BUILDER_IMAGE", None)
    try:
        dockerctl.IMAGE_INFO.update(digest="")
        assert dockerctl.image_ref() == dockerctl.IMAGE_INFO["local"]
        assert not dockerctl.image_is_published(dockerctl.image_ref())
        dockerctl.IMAGE_INFO.update(digest="sha256:" + "a" * 64)
        ref = dockerctl.image_ref()
        assert ref.startswith("ghcr.io/") and ref.endswith("@sha256:" + "a" * 64) and dockerctl.image_is_published(ref)
        assert dockerctl.image_ref("mine:1") == "mine:1"
        os.environ["ISAAC_BUILDER_IMAGE"] = "env:2"
        assert dockerctl.image_ref() == "env:2"
    finally:
        dockerctl.IMAGE_INFO.clear()
        dockerctl.IMAGE_INFO.update(saved)
        os.environ.pop("ISAAC_BUILDER_IMAGE", None)
        if env:
            os.environ["ISAAC_BUILDER_IMAGE"] = env
    assert dockerctl.source_tree() == ROOT, "the repository can build the image"


def test_remove_work(tmp: Path) -> None:
    mine = tmp / "documents"
    (mine / "notes").mkdir(parents=True)
    assert not dockerctl.remove_work("docker", mine) and (mine / "notes").is_dir(), "never someone else's folder"
    work = tmp / "work"
    (work / "stamps").mkdir(parents=True)
    (work / "upstream" / "x").mkdir(parents=True)
    assert dockerctl.remove_work("docker", work) and not work.exists()
    assert dockerctl.remove_work("docker", tmp / "absent")


def test_progress(tmp: Path) -> None:
    now = [0.0]
    p = progress.Progress(clock=lambda: now[0])
    assert p.fraction() == 0.0 and p.eta() == sum(progress.EXPECTED.values())

    def stage(name, state, seconds=0):
        p.feed({"event": "stage", "name": name, "state": state, "seconds": seconds,
                "number": progress.ORDER.index(name) + 1, "total": 9})

    stage("prepare", "skipped")
    stage("ghidra", "running")
    now[0] = 750.0
    half = p.fraction()
    assert abs(half - 750 / (sum(progress.EXPECTED.values()) - progress.EXPECTED["prepare"])) < 1e-9, half
    stage("ghidra", "done", 1400)
    assert p.took["ghidra"] == 1400 and p.current == "ghidra"
    left = p.eta()
    assert left == sum(v for k, v in progress.EXPECTED.items() if k not in ("prepare", "ghidra"))
    stage("tables", "running")
    now[0] = 10_000.0               # a stage longer than expected never goes negative
    assert p.eta() >= progress.EXPECTED["tables"] * 0.05 and p.fraction() < 1.0
    p.feed({"event": "log", "line": "analysing"})
    assert p.last_log == "analysing"
    p.feed({"event": "failed", "message": "boom", "tail": []})
    assert p.state["tables"] == "failed" and p.failure["message"] == "boom"
    q = progress.Progress(export=True)
    q.feed({"event": "exporting"})
    assert q.state["export"] == "running"
    q.feed({"event": "exported"})
    q.feed({"event": "done", "nro_sha256": "ab"})
    assert q.state["export"] == "done" and q.eta() == 0.0 and q.result["nro_sha256"] == "ab"


def test_sdcard(tmp: Path) -> None:
    mounts_text = ("/dev/sda1 / ext4 rw 0 0\n/dev/sdb1 /media/u/SWITCH\\040SD vfat rw 0 0\n"
                   "/dev/sdc1 /run/media/u/CARD exfat rw 0 0\n")
    drives = sdcard._linux_drives(mounts_text)
    assert [d.path for d in drives] == [Path("/media/u/SWITCH SD"), Path("/run/media/u/CARD")]
    assert drives[0].fs == "vfat"

    src = tmp / "sd-card"
    guest = src / "switch/isaac-recomp/guest/resources"
    guest.mkdir(parents=True)
    (src / sdcard.NRO).write_bytes(b"new nro")
    (guest / "big.a").write_bytes(b"G" * 1000)
    card = tmp / "card"
    (card / "switch/isaac-recomp/saves").mkdir(parents=True)
    (card / "switch/isaac-recomp/saves/save1.dat").write_text("mine")
    assert not sdcard.looks_like_switch_card(tmp / "sd-card" / "switch" / "isaac-recomp")
    assert sdcard.looks_like_switch_card(card)
    seen = []
    copied, checked = sdcard.copy_to_card(src, card, lambda f, n: seen.append(f))
    assert (copied, checked) == (2, 2) and seen[-1] == 1.0
    assert (card / "switch/isaac-recomp/saves/save1.dat").read_text() == "mine", "saves are left alone"
    assert not list(card.rglob("*.part"))
    (src / sdcard.NRO).write_bytes(b"newer nro")
    copied, _ = sdcard.copy_to_card(src, card)
    assert copied == 1, "only the NRO: the game files are already there"
    assert (card / sdcard.NRO).read_bytes() == b"newer nro"


def test_redact(tmp: Path) -> None:
    home = str(Path.home())
    assert system.redact("log in %s/IsaacSwitchBuild" % home) == "log in ~/IsaacSwitchBuild"
    assert system.redact("nothing here") == "nothing here"


def test_docker_fixes(tmp: Path) -> None:
    """Each Docker problem comes with what fixes it, and every fix has a button in the window."""
    S = dockerctl.Status
    linux = {i.key: i.fixes for i in
             dockerctl.status_items(S("missing"), "linux") + dockerctl.status_items(S("no_permission"), "linux")
             + dockerctl.status_items(S("stopped", detail="x"), "linux")}
    assert linux == {"docker.missing": ("docker_page",), "docker.no_permission": ("copy_group",),
                     "docker.stopped": ("copy_start",)}, linux
    win = dockerctl.status_items(S("missing"), "win32")[0]
    assert win.key == "docker.missing_win" and "docker_page" in win.fixes
    assert dockerctl.status_items(S("stopped"), "win32")[0].key == "docker.stopped_win"
    assert set(system.COMMANDS) <= {"copy_group", "copy_start"}
    used = set()
    for path in (ROOT / "host/builder/isb").glob("*.py"):
        text = path.read_text()
        for m in re.finditer(r"fix=(\([^)]*\)|\"\w+\")", text):
            used |= set(re.findall(r'"(\w+)"', m.group(1)))
    gui = (ROOT / "host/builder/isb/gui.py").read_text()
    offered = set(re.findall(r'^\s+"(\w+)": \(t\("btn\.', gui, re.M))
    assert used and not used - offered, "fixes with no button: %s" % sorted(used - offered)


def test_texts(tmp: Path) -> None:
    fields = re.compile(r"\{(\w+)\}")
    for key, (es, en) in texts.T.items():
        assert es and en, key
        assert sorted(fields.findall(es)) == sorted(fields.findall(en)), "placeholders differ in %s" % key
    used = set()
    for path in (ROOT / "host/builder/isb").glob("*.py"):
        used |= set(re.findall(r'''\bt\(\s*"([a-z_]+\.[a-z_.]+|yes_no)"''', path.read_text()))
        used |= set(re.findall(r'''Item\(\w+, "([a-z_]+\.[a-z_]+)"''', path.read_text()))
    missing = sorted(k for k in used if k not in texts.T)
    assert not missing, missing
    for stage in progress.ORDER + ["export"]:
        assert "stage." + stage in texts.T, stage
    texts.set_lang("es")
    assert texts.t("btn.start") == "Empezar el build" and "8 min" == texts.duration(470)
    texts.set_lang("en")
    assert texts.t("btn.start") == "Start the build" and texts.duration(12) == "12 s"
    assert checks.PE_SHA256 in texts.t("drm.body", sha=checks.PE_SHA256)


def test_cli_check(tmp: Path) -> None:
    """The command line's check against a fake game, without a real executable."""
    from isb import cli
    game = fake_library(tmp / "lib")
    out = io.StringIO()
    stdout, sys.stdout = sys.stdout, out
    try:
        rc = cli.main(["--lang", "en", "check", "--game", str(game), "--exe", str(tmp / "absent.exe"),
                       "--folder", str(tmp / "out")])
    finally:
        sys.stdout = stdout
    said = out.getvalue()
    assert rc == 2 and "DRM" in said and checks.PE_SHA256 in said, said
    assert "✘ Choose the executable without DRM." in said, said


def main() -> int:
    tests = [test_vdf, test_find_game, test_check_game, test_exe, test_command_linux, test_command_windows,
             test_events, test_image_ref, test_remove_work, test_progress, test_sdcard, test_redact, test_docker_fixes,
             test_texts,
             test_cli_check]
    for test in tests:
        with tempfile.TemporaryDirectory() as d:
            test(Path(d).resolve())     # Windows hands out 8.3 names such as RUNNER~1
        print("%-24s PASS" % test.__name__)
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
