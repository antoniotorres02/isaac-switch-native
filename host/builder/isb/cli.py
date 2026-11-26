"""The command line: the same steps as the window, for a terminal.

    isaac-switch-builder                      opens the window
    isaac-switch-builder check  --exe FILE    runs every check
    isaac-switch-builder build  --exe FILE    checks, then builds (resumes on its own)
    isaac-switch-builder copy-to-sd DEST      copies the result to the SD card
    isaac-switch-builder clean                deletes the work files
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import VERSION, checks, dockerctl, sdcard, steam, system, texts
from .checks import FAIL
from .progress import Progress
from .report import item_line
from .texts import t


def say(text: str = "") -> None:
    try:
        print(text, flush=True)
    except UnicodeEncodeError:       # an old Windows console
        print(text.encode("ascii", "replace").decode(), flush=True)


def drm_notice() -> None:
    say("=" * 72)
    say(t("drm.title"))
    say(t("drm.body", sha=checks.PE_SHA256))
    say("=" * 72)


def gather(args, with_exe: bool = True) -> tuple[list, steam.GameInstall | None, dockerctl.Status]:
    game = steam.describe_folder(args.game) if args.game else steam.find_game()
    items = checks.check_game(game)
    if with_exe:
        exe = args.exe
        if exe is None:
            say(t("exe.searching"))
            exe = checks.find_exe(checks.exe_search_dirs(game.path if game else None))
            args.exe = exe
        items += checks.check_exe(exe)
    st = dockerctl.status()
    items += dockerctl.status_items(st)
    if st.state == "ok":
        ref = dockerctl.image_ref(args.image)
        if dockerctl.image_present(st.exe, ref):
            items.append(checks.Item(checks.OK, "image.present", {"ref": ref}))
        elif dockerctl.image_is_published(ref):
            items.append(checks.Item(checks.WARN, "image.absent"))
        else:
            items.append(checks.Item(FAIL, "image.unpublished" if dockerctl.source_tree() else "image.unavailable"))
    items += checks.check_disk(args.folder, checks.NEED_DISK)
    return items, game, st


def show(items) -> None:
    for item in items:
        say("  " + item_line(item).replace("\n", "\n      "))
        if item.key == "docker.memory_low" and sys.platform == "win32":
            say("      " + t("docker.memory_low_win"))
        if "docker_page" in item.fixes:
            say("      " + system.docker_url())
        if "install_docker" in item.fixes:
            say("      winget install --exact --id Docker.DockerDesktop")
        for fix in item.fixes:
            if fix in system.COMMANDS:
                say("      " + system.COMMANDS[fix])


def cmd_check(args) -> int:
    drm_notice()
    say(t("cli.checking"))
    items, _, _ = gather(args)
    show(items)
    say()
    say(t("check.some_fail") if checks.blocking(items) else t("check.all_ok"))
    return 2 if checks.blocking(items) else 0


def ensure_image(st: dockerctl.Status, ref: str) -> bool:
    if dockerctl.image_present(st.exe, ref):
        return True
    if dockerctl.image_is_published(ref):
        say(t("image.pulling"))
        return dockerctl.pull(st.exe, ref, lambda line: say("  " + line)) == 0
    tree = dockerctl.source_tree()
    if tree is None:
        say(t("image.unavailable"))
        return False
    say(t("image.building"))
    return dockerctl.build_image(st.exe, tree, ref, lambda line: say("  " + line)) == 0


def cmd_build(args) -> int:
    drm_notice()
    say(t("cli.checking"))
    items, game, st = gather(args)
    # The image is fetched below; only its absence is not a reason to stop here.
    hard = [i for i in items if i.state == FAIL and not i.key.startswith("image.unpublished")]
    show(items)
    if hard:
        say()
        say(t("cli.cannot_start"))
        return 2
    ref = dockerctl.image_ref(args.image)
    if not ensure_image(st, ref):
        say(t("image.failed"))
        return 2
    plan = make_plan(args, st, ref, game)
    progress = Progress(export=plan.export is not None, nsp=plan.fmt in ("nsp", "both"))
    last = {"line": ""}

    def on_event(event):
        progress.feed(event)
        kind = event.get("event")
        if kind == "stage" and event.get("state") in ("running", "done", "skipped"):
            mark = {"running": "▶", "done": "✔", "skipped": "✔"}[event["state"]]
            extra = ""
            if event["state"] == "done":
                extra = " (%s)" % texts.duration(event.get("seconds", 0))
            elif event["state"] == "skipped":
                extra = " (ya hecho)" if texts.lang() == "es" else " (already done)"
            left = progress.eta()
            eta = "  ~%s" % texts.duration(left) if event["state"] == "running" and left else ""
            say("[%d/%d] %s %s%s%s" % (event["number"], event["total"], mark, t("stage." + event["name"]),
                                       extra, eta))
        elif kind == "log" and args.verbose:
            say("      " + event.get("line", ""))
        elif kind == "problems":
            say(t("page.build.problems"))
            for p in event.get("problems", []):
                say("  - " + p)
        elif kind == "exporting":
            say("▶ " + t("stage.export"))
        elif kind == "failed":
            say()
            say(t("cli.failed", message=event.get("message", "")))
            for line in event.get("tail", [])[-15:]:
                say("    " + line)

    def on_text(line):
        last["line"] = line
        if args.verbose:
            say("    " + line)

    with system.keep_awake("Building Isaac for the Switch"):
        rc = dockerctl.run_build(plan, on_event, on_text)
    if progress.result:
        say()
        say(t("page.result.hash", sha=progress.result.get("nro_sha256", "")))
        say(t("cli.done", time=texts.duration(progress.elapsed()), path=system.result_folder(args.folder)))
        say(texts.launch_steps(plan.fmt))
        say(t("page.result.share"))
        return 0
    if progress.stopped or rc in (130, 137, 143):
        say()
        say(t("cli.stopped"))
    elif not progress.failure and not progress.problems:
        say(t("cli.failed", message="exit %s" % rc))
        if last["line"]:
            say("    " + last["line"])
    return rc or 1


def make_plan(args, st: dockerctl.Status, ref: str, game) -> dockerctl.Plan:
    return dockerctl.Plan(engine=st.engine, exe_path=st.exe, image=ref, game_exe=Path(args.exe).resolve(),
                          game=game.path.resolve(), work=system.work_folder(args.folder),
                          export=system.export_folder(args.folder), steam=args.steam,
                          fmt=args.format, threads=args.threads, jobs=args.jobs, source=dockerctl.source_tree())


def cmd_copy(args) -> int:
    src = system.result_folder(args.folder)
    if not sdcard.has_result(src):
        say(t("cli.no_sd_source", path=src))
        return 2
    dest = args.dest
    if dest is None:
        drives = sdcard.removable_drives()
        if len(drives) != 1:
            say(t("page.result.no_sd"))
            for d in drives:
                say("  " + str(d))
            return 2
        dest = drives[0].path
    if not sdcard.looks_like_switch_card(dest) and not args.yes:
        if input(t("page.result.not_switch", path=dest) + t("yes_no")).strip().lower() not in ("y", "s", "yes", "si", "sí"):
            return 1

    def progress(frac, name):
        sys.stdout.write("\r%3d%%  %-40s" % (frac * 100, name[:40]))
        sys.stdout.flush()

    try:
        copied, checked = sdcard.copy_to_card(src, dest, progress)
    except OSError as e:
        say()
        say(t("page.result.copy_failed", error=e))
        return 1
    say()
    say(t("page.result.copied", copied=copied, checked=checked))
    return 0


def cmd_clean(args) -> int:
    found = dockerctl.find_engine()
    work = system.work_folder(args.folder)
    what = dockerctl.VOLUME if work is None else str(work)
    if not args.yes and input(t("clean.confirm", what=what) + t("yes_no")).strip().lower() not in ("y", "s", "yes", "si", "sí"):
        return 1
    if work is None and not found:
        say(t("docker.missing"))
        return 2
    if found:
        dockerctl.stop(found[1])
    if dockerctl.remove_work(found[1] if found else "", work):
        say(t("cli.cleaned"))
        return 0
    say(t("cli.clean_refused", path=what))
    return 1


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="isaac-switch-builder", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=VERSION)
    ap.add_argument("--lang", choices=("es", "en"))
    sub = ap.add_subparsers(dest="command")

    def common(p, exe=True):
        p.add_argument("--folder", type=Path, default=system.default_folder(),
                       help="where the result goes (default: %(default)s)")
        p.add_argument("--image", help="the Docker image to use instead of the pinned one")
        if exe:
            p.add_argument("--exe", type=Path, help="the executable without DRM (searched for when absent)")
            p.add_argument("--game", type=Path, help="the game's folder (found through Steam when absent)")

    gui = sub.add_parser("gui", help="open the window (the default)")
    common(gui)
    common(sub.add_parser("check", help="run every check and stop"))
    b = sub.add_parser("build", help="check, then build; run it again to resume")
    common(b)
    b.add_argument("--steam", action="store_true", help="include Steam Cloud sync (read STEAM-CLOUD.md first)")
    for p in (gui, b):
        p.add_argument("--format", choices=("nsp", "nro", "both"), default="nsp",
                       help="nsp: HOME Menu installer (default); nro: direct launch; both: both launch methods")
    b.add_argument("--threads", type=int)
    b.add_argument("--jobs", type=int)
    b.add_argument("-v", "--verbose", action="store_true", help="show the build's log lines")
    c = sub.add_parser("copy-to-sd", help="copy the result to the SD card")
    common(c, exe=False)
    c.add_argument("dest", type=Path, nargs="?", help="the card's root (found when only one is inserted)")
    c.add_argument("-y", "--yes", action="store_true")
    k = sub.add_parser("clean", help="delete the work files")
    common(k, exe=False)
    k.add_argument("-y", "--yes", action="store_true")
    return ap


def main(argv=None) -> int:
    args = parser().parse_args(argv)
    texts.set_lang(args.lang)
    if args.command in (None, "gui"):
        from . import gui
        return gui.main(args)
    return {"check": cmd_check, "build": cmd_build, "copy-to-sd": cmd_copy, "clean": cmd_clean}[args.command](args)
