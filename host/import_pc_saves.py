#!/usr/bin/env python3
"""Turn a PC Repentance+ profile into a native save store (ISAAC_SAVE_DIR).

Steam Cloud keeps the slots as userdata/<id>/250900/remote/rep+persistentgamedataN.dat
and rep+gamestateN.dat; the native build has Steam stubbed and reads the local
names under Documents/My Games/Binding of Isaac Repentance+/. The bytes are the
same file (same exe), only the name changes -- the web port's EDIT FILE import
does the same rename.

options.ini starts from the native defaults (SteamCloud=0, so the local files
are read; windowed 960x540; notices accepted) and takes the player's own
preferences from the PC file.

Mod save data (Isaac.SaveModData) lives in <game>/data/<metadata directory>/ on
PC; with Steam stubbed the engine uses the mod's folder name instead, so each
installed mod's data is copied to data/<folder>/.

    python host/import_pc_saves.py --out build/browsing-native/pc-saves-user

The defaults are a Linux Steam install (~/.local/share/Steam, the game under
Proton). Elsewhere, name the three places: --steam (the folder that holds
userdata/), --game (the game folder, with mods/ and data/) and --pc-options
(the PC options.ini, in Documents/My Games/Binding of Isaac Repentance+/).
"""
import argparse
import os
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
STEAM = Path.home() / ".local/share/Steam"
GAME_REL = Path("steamapps/common/The Binding of Isaac Rebirth")
PROTON_DOCS_REL = Path("steamapps/compatdata/250900/pfx/drive_c/users/steamuser/Documents/My Games/Binding of Isaac Repentance+")
DOCS_REL = Path("documents/my games/binding of isaac repentance+")
KEEP = {"MusicVolume", "SFXVolume", "Language", "MapOpacity", "Filter", "Exposure", "Gamma",
        "HudOffset", "ChargeBars", "BulletVisibility", "AimLock", "ShowRecentItems", "CameraStyle",
        "PopUps", "RumbleEnabled", "BossHpOnBottom", "AnnouncerVoiceMode", "ItemInfoDisplayEnabled",
        "EnableMods", "FoundHUD", "MaxScale", "MaxRenderScale", "MusicEnabled", "JacobEsauControls",
        "AscentVoiceOver", "EnableDebugConsole", "ConsoleFont", "FadedConsoleDisplay", "SaveCommandHistory"}


def native_default_options() -> str:
    src = (HERE / "native" / "native_main.c").read_text()
    m = re.search(r"DEFAULT_OPTIONS\[\] =\n(.*?);\n", src, re.S)
    body = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
    return body.replace("\\r", "\r").replace("\\n", "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--steam-user", help="userdata/<id>; default: the only one with 250900")
    ap.add_argument("--steam", type=Path, default=STEAM, help="Steam folder (default %(default)s)")
    ap.add_argument("--game", type=Path, help="game folder (default <steam>/%s)" % GAME_REL)
    ap.add_argument("--pc-options", type=Path, help="PC options.ini (default: the Proton prefix's)")
    args = ap.parse_args()
    game = args.game or args.steam / GAME_REL
    pc_opts = args.pc_options or args.steam / PROTON_DOCS_REL / "options.ini"

    users = [p for p in (args.steam / "userdata").glob("*/250900/remote")]
    if args.steam_user:
        users = [args.steam / "userdata" / args.steam_user / "250900" / "remote"]
    if len(users) != 1:
        sys.exit("pick one --steam-user of: %s" % ", ".join(p.parts[-3] for p in users))
    remote = users[0]

    docs = args.out / DOCS_REL
    docs.mkdir(parents=True, exist_ok=True)
    copied = []
    for kind in ("persistentgamedata", "gamestate"):
        for n in (1, 2, 3):
            src = remote / ("rep+%s%d.dat" % (kind, n))
            if src.exists():
                shutil.copy2(src, docs / ("%s%d.dat" % (kind, n)))
                copied.append(src.name)

    options = native_default_options()
    if pc_opts.exists():
        pc = dict(line.split("=", 1) for line in pc_opts.read_text(encoding="latin-1").replace("\r", "").split("\n")
                  if "=" in line)
        lines = []
        for line in options.replace("\r", "").split("\n"):
            key = line.split("=", 1)[0]
            lines.append("%s=%s" % (key, pc[key]) if "=" in line and key in KEEP and key in pc else line)
        options = "\r\n".join(lines)
    (docs / "options.ini").write_bytes(options.encode("latin-1"))

    mods_with_data = 0
    for folder in sorted(os.listdir(game / "mods")) if (game / "mods").exists() else []:
        meta = game / "mods" / folder / "metadata.xml"
        if not meta.exists():
            continue
        m = re.search(r"<directory>(.*?)</directory>", meta.read_text(encoding="utf-8", errors="replace"))
        data = game / "data" / (m.group(1).strip() if m else folder)
        if data.is_dir():
            shutil.copytree(data, args.out / "data" / folder, dirs_exist_ok=True)
            mods_with_data += 1

    print("saves: %s" % ", ".join(copied) or "none")
    print("options.ini: native defaults + PC preferences (SteamCloud=0)")
    print("mod data: %d mod(s)" % mods_with_data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
