# Playing: Switch install, controls, PC saves and mods

This assumes you have built the NRO (and, for PC play, the `gl` executable)
following [BUILDING.md](BUILDING.md). Paths use the same variables:

```sh
R=$PWD                                   # this repository
U=$R/build/browsing-port/repo            # upstream checkout
GAME="$HOME/.local/share/Steam/steamapps/common/The Binding of Isaac Rebirth"
```

## Installing on the Switch

You need a Switch running custom firmware (tested with Atmosphère on a
Switch Lite) and a homebrew launcher that can start NROs in **Application
mode**: hbmenu opened by holding R while launching a game, or Sphaira (tested
with 1.0.8). Applet mode (hbmenu opened from the Album) does not have enough
memory, and the NRO refuses to start there.

The SD card layout:

```
sdmc:/switch/isaac-browsing.nro           the NRO you built
sdmc:/switch/isaac-recomp/
  guest/                                  game files from your PC install
    resources/packed/*.a
    resources/scripts/...
    savedatapath.txt
    mods/<mod folder>/                    optional, one folder per mod
  saves/                                  your saves (created at first start)
  settings.ini                            Switch options (created at first start)
  browsing-last-run.txt                   what the last start did, for troubleshooting
```

Copy the files with an SD card reader or over FTP (Sphaira has an FTP
server; ftpd and others work too):

1. `build/browsing-native/switch/isaac-browsing.nro` → `sdmc:/switch/isaac-browsing.nro`.
2. From your Steam folder (`$GAME`): `resources/` and `savedatapath.txt` →
   `sdmc:/switch/isaac-recomp/guest/`. The NRO only reads the 22 files listed
   in its `instance_index.json` (about 1.07 GB): the ten `resources/packed/*.a`
   archives, `resources/scripts/` and `savedatapath.txt`. Copying the whole
   `resources/` folder (about 1.5 GB, with the language packs) also works; the
   extra files are not used.
3. Optional: your PC saves into `saves/` and your mods into `guest/mods/`
   (sections below).

Then launch **Isaac (browsing lift)** from the homebrew launcher. Quit with
HOME → Close: the game shuts down normally and writes its saves.

The NRO contains the memory image of your executable. Do not share it.

## Controls

The pad goes through the game's own controller support, so the game's
controller layout applies. Two things differ from an Xbox pad:

| Button | In a run | In menus |
| --- | --- | --- |
| A, B, X, Y | shoot by position: right, down, left, up | A accepts, B goes back (by position if `a_accepts=0`) |
| + (tap) | pause, when released | START |
| + (hold about 2 s) | restart the run (the game's own restart) | START |
| − | BACK | BACK |
| L, R, ZL, ZR, sticks, D-pad | their Xbox equivalents | their Xbox equivalents |

The buttons drawn on screen are the Switch ones (`switch_glyphs=1`), using
the Switch art that ships inside the PC game. The controls drawing on the
first room's floor shows the in-run layout.

## settings.ini

`sdmc:/switch/isaac-recomp/settings.ini` is created with the defaults the
first time the game starts. Edit it with any text editor; `1` is on and `0`
is off. Changes apply at the next start.

```ini
# Draw Nintendo Switch buttons instead of the Xbox ones.
switch_glyphs=1

# In menus A accepts and B goes back. 0: by position like an Xbox pad
# (the bottom button, B, accepts). In play the buttons always shoot by
# position.
a_accepts=1
```

These are part of the host, not mods: they do not appear in the mods menu,
and with `switch_glyphs=0` the game's files are not touched at all.

## Moving your PC saves

On PC, Repentance+ keeps its three save slots in Steam Cloud:
`<Steam>/userdata/<your id>/250900/remote/rep+persistentgamedataN.dat` and
`rep+gamestateN.dat` (the run in progress). The native build has Steam
stubbed and reads the same files under their local names. The content is the
same, only the name changes. `import_pc_saves.py` builds a save folder from
your Steam profile:

```sh
python3 host/import_pc_saves.py --out build/browsing-native/pc-saves
```

It writes:

```
pc-saves/
  documents/my games/binding of isaac repentance+/
    persistentgamedata1-3.dat     your three slots (unlocks, progress)
    gamestate1-3.dat              runs in progress, if any
    options.ini                   native defaults plus your PC preferences
  data/<mod folder>/saveN.dat     each installed mod's own save data
```

- The defaults are for Steam on Linux with the game under Proton. On Windows
  or with another Steam folder, pass `--steam <folder with userdata/>`,
  `--game <game folder>` and `--pc-options <Documents/My Games/Binding of
  Isaac Repentance+/options.ini>`. If you have several Steam accounts, pick
  one with `--steam-user <id>`.
- `options.ini` is not your PC file copied as-is: it starts from the native
  defaults (`SteamCloud=0`, so the local slots are read; windowed 960x540)
  and takes your preferences (volume, language, HUD, `EnableMods`…) from the
  PC file. Do not replace it with the PC one.
- Steam may rewrite the cloud files when the game closes or syncs. Close the
  game, let Steam finish syncing, then import.

For the Switch, copy the **contents** of the output folder (`documents/` and
`data/`) into `sdmc:/switch/isaac-recomp/saves/`. If that folder already has
saves you care about, back it up first: the files replace the console's.
On PC, point `ISAAC_SAVE_DIR` at the output folder.

The game writes back into the same folder: the slots, `options.ini`,
`save_backups/`, the game's `log.txt`, mod data and `mod-state/` (see below).
Copying saves from the Switch back to a PC has not been tested. If you try
it, the names have to go back to `rep+…` in Steam's `remote/` folder, and you
should back up both sides first.

## Mods

Mods from the Steam Workshop work, including Lua mods. They were tested with
17 mods at once (EID, Mod Config Menu, Enhanced Boss Bars, Planetarium
Chance, Boss Rush Wave Counter and others) together with their saved data.

### Adding mods

1. On PC, subscribe in the Workshop and start the game once: it places each
   mod in `$GAME/mods/<name>_<workshop id>/`. A mod is a folder with a
   `metadata.xml`; folders without one are ignored by the game.
2. Copy each mod folder **as is** (keep the folder name) to:
   - Switch: `sdmc:/switch/isaac-recomp/guest/mods/<folder>/`
   - PC: `<instance>/mods/<folder>/` (see [Playing on PC](#playing-on-pc))
3. Mod save data (Mod Config Menu settings, EID options, …) is not in the mod
   folder. `import_pc_saves.py` copies it from `$GAME/data/` to
   `data/<mod folder>/` in the save folder, because without Steam the engine
   uses the folder name. Re-run the importer, or copy `$GAME/data/<metadata
   directory>/` to `saves/data/<mod folder>/` by hand.

On the Switch, play mode always mounts `guest/mods/`.

### Turning mods on and off

Use the game's own **Mods** menu. The choice applies at the next start, as
on PC, and the host remembers it as an empty file in the save folder:
`mod-state/<folder>.off` or `mod-state/<folder>.on`. That file wins over
anything else, so:

- to turn a mod off without the menu, create `saves/mod-state/<folder>.off`;
- a mod folder that contains `disable.it` starts off until you turn it on in
  the menu;
- to remove a mod, delete its folder from `mods/` (and, if you like, its
  `mod-state` file and `data/` folder).

A disabled mod is still listed in the menu, greyed out, so it can be turned
back on; none of its files are loaded.

### Limits

- Mods run on the game's Lua 5.3.3 API, as on PC. A mod that fails logs the
  error to the game's `log.txt` (in `saves/documents/my games/binding of
  isaac repentance+/`) and the game continues, as on PC.
- Mods that need REPENTOGON or anything else that patches the Windows
  executable are not supported: there is no Windows executable here.
- The game has 768 MiB of heap. Leaving the mods menu reloads every mod's
  resources while the old ones are still in memory: 17 mods peaked at
  421.5 MiB, so very large mod collections may not fit.
- Replacing game graphics by dropping loose files into `resources/` does not
  work: the engine reads its packed archives first. Graphics have to be
  replaced through a mod's own `resources/` folder, as on PC.

## Playing on PC

The `gl` profile opens a 960x540 window with keyboard (WASD moves, arrows
shoot, E bomb, space item) and the first gamepad through the game's XInput.
Closing the window quits normally.

Without mods, the upstream game bundle is the instance:

```sh
ISAAC_NATIVE_WINDOW=1 ISAAC_SAVE_DIR=build/browsing-native/pc-saves \
  build/browsing-native/gl/isaac_native \
  "$U/output/recomp/host/isaac.segs.bin" "$U/.scratch/game-bundle" main
```

With mods, make an instance folder that shares the bundle's resources and
has its own `mods/`:

```sh
I=build/browsing-native/pc-instance
mkdir -p "$I/mods"
ln -s "$U/.scratch/game-bundle/resources" "$I/resources"
cp "$U/.scratch/game-bundle/savedatapath.txt" "$I/"
cp -r "$GAME/mods/." "$I/mods/"
ISAAC_NATIVE_WINDOW=1 ISAAC_NATIVE_MODS=1 ISAAC_SAVE_DIR=build/browsing-native/pc-saves \
  build/browsing-native/gl/isaac_native "$U/output/recomp/host/isaac.segs.bin" "$I" main
```

Add `ISAAC_NATIVE_SWITCH_GLYPHS=1` to see the Switch prompts on PC. Other
variables are listed in [DESIGN.md](DESIGN.md#environment-variables).

## Troubleshooting

`sdmc:/switch/isaac-recomp/browsing-last-run.txt` records each stage of the
last start (settings read, heap state, game started, how it ended). Look
there first.

- **It returns to the launcher at once.** If the log's `end:` line says
  `WRONG_MODE`, the launcher is in applet mode: start it by holding R while
  launching a game, or use Sphaira as a title.
- **"The software was closed because an error occurred" right at launch,
  after another homebrew ran.** Older builds of this NRO left GPU memory
  behind in hbloader's shared heap; current builds release it and fence off
  what other NROs leave. If it still happens, close the launcher completely
  (HOME → Close) and start it again.
- **It stops while booting.** Check that the game files are under `guest/`
  exactly as in the layout above (`guest/resources/packed/…`, not
  `guest/The Binding of Isaac Rebirth/resources/…`).
- **A mod does not appear.** Its folder needs `metadata.xml` at the top
  level, directly under `mods/<folder>/`.
- **Mod settings are gone.** Copy the mod's data folder into
  `saves/data/<mod folder>/` (see [Adding mods](#adding-mods)).
