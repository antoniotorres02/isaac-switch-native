# Building it with Docker

From your own copy of the game to a folder you copy to the SD card, with one
command. Docker brings every tool at the exact version this was measured with.
[BUILDING.md](BUILDING.md) has the same steps by hand.

**Verified on x86-64 Linux only.** It should also run on Windows with Docker
Desktop, but nobody has tried that yet.

**Prefer a window?** [host/builder/README.md](host/builder/README.md) describes Isaac Switch
Builder, a program for Windows and Linux that does all of this for you: it finds the
game through Steam, checks everything with an explanation for each problem, runs
this same build with a progress bar, and copies the result to the SD card. It needs
the same executable (below) and Docker.

## What you need

- **Your own purchased copy** of The Binding of Isaac: Rebirth with all its
  expansions through Repentance+, at the version Steam ships today (**v1.9.7.19**).
  The build names the version it supports if yours differs.
- **The executable described below.** This is the one step this guide does not
  cover.
- **[Docker](https://docs.docker.com/get-docker/).** Nothing else gets installed.
- **6 GiB of free disk and 4 GiB of memory** for the build (it ends up using about
  3 GiB of disk), plus about 1 GB for the Docker image (a 230 MB download).
- **A Switch that already runs homebrew** in Application mode, with enough room on
  the card for about 1.1 GB. Preparing the console is not covered here.

The build took 11 minutes on the 16-thread machine it was measured on. With fewer
cores or less memory it runs fewer jobs at once and takes longer. You can leave it
running.

## The executable

The build needs the game's executable with Steam's DRM removed. That step is
required, and this guide does not cover it: this project does not include, link to
or describe any tool or method for it. Whether and how you do it is your decision
and your responsibility under the law that applies to you. Please check that first.

The file has to be exactly the one for your version, because every address in the
build belongs to it. For v1.9.7.19:

```sh
sha256sum isaac-ng.unpacked.exe
# 8c9526c1bf0910a930cba4037d120b357779940bbae34fabc8afaf56150e0b6a
```

Name it however you like; the command below mounts it under the name the build
expects. The build checks the hash first and refuses any other file within seconds.

## Build

From the root of this repository, once:

```sh
docker build -t isaac-switch-builder -f docker/Dockerfile .
```

This takes a few minutes, downloads a few hundred MB, and the image
contains nothing from the game. Then, with the paths changed to yours:

```sh
GAME="$HOME/.local/share/Steam/steamapps/common/The Binding of Isaac Rebirth"
EXE="$HOME/isaac-ng.unpacked.exe"

mkdir -p isaac-build
docker run --rm --network none -u "$(id -u):$(id -g)" \
  -v "$EXE":/input/isaac-ng.unpacked.exe:ro \
  -v "$GAME":/game:ro \
  -v "$PWD/isaac-build":/work \
  isaac-switch-builder --format both
```

The executable and the game are mounted read-only, and the build has no network:
nothing leaves your machine. You will see nine steps:

```
Checking your files (this reads the executable once)...
Executable, game folder, disk and memory are fine.
[1/9] Preparing the sources...
[2/9] Listing the game's functions...
...
Done in ...
NRO sha256: ...
Copy the contents of .../out/sd-card to the root of your SD card.
```

**If it stops, or you stop it**, run the same command again: it carries on from the
step it was on. Each step's output is in `isaac-build/logs/`.

Add `--check-only` at the end of the command to run only the checks.

### Steam Cloud is left out by default

The console can sign in to Steam, download your Cloud saves and upload them again
(from the in-game panel, hold − and press +, and when the game closes), but it does
so as an unofficial Steam client. To include it, read
[STEAM-CLOUD.md](STEAM-CLOUD.md), then add `--steam` at the end of the command and
run it again; only the last two steps repeat.

## Install

Copy the **contents** of `isaac-build/out/sd-card/` to the root of your SD card, with
a card reader or over FTP. It holds:

```
switch/isaac-repentance-ns1-recomp.nro  direct-launch game
switch/isaac-recomp/guest/...          the game files the NRO reads
switch/isaac-recomp-installer.nro      HOME Menu installer
switch/isaac-recomp/install/...        installer payload
```

Open **ISAAC REPENTANCE+ NS1 RECOMP** from the Homebrew Menu in Application mode for direct
launch. For a HOME Menu icon, start `isaac-recomp-installer.nro` and press
**A**. Use `--format nro` or `--format nsp` to prepare a single launch method.

[PLAYING.md](PLAYING.md) covers the controls, `settings.ini`, moving your PC saves
and adding mods.

**Do not share the NRO or the `isaac-build` folder.** The NRO contains the memory
image of the executable, and the folder holds the executable and the game's files.
Everyone has to build their own.

## When something goes wrong

| What you see | What it means |
| --- | --- |
| `not a game version this builder knows` | The file's hash is not one in `versions/`. Another game version needs a newer builder; a file that differs in any byte cannot work. |
| `no longer supports` | The executable is from an older version. Update the game in Steam and use the new executable. |
| `still with its DRM` | That is Steam's own `isaac-ng.exe`. The build needs the executable without the DRM (above). |
| `... is a folder, not a file` | The path in `EXE` does not exist, so Docker made an empty folder there. Fix the path and delete that folder. |
| `does not look like the game's folder` | `GAME` has to be the folder containing `resources/` and `savedatapath.txt`. |
| `The game folder is incomplete` | Let Steam verify the game's files, or check that all the expansions are installed. |
| `Not enough disk` / `Not enough memory` | As it says. On Docker Desktop the memory is a setting: Resources > Memory. |
| `cannot be written` | Create `isaac-build` before the command, as shown, so that it belongs to you. |
| The console says **NO GAME FILES** | The copy to the card is incomplete. The screen names what is missing. |
| It returns to the launcher at once | Start it in Application mode. See [PLAYING.md](PLAYING.md#troubleshooting). |

**If Steam has updated the game**, the build says so in its first seconds and names
the version it supports. Each version needs its addresses moved over once
([versions/README.md](versions/README.md)); after that, a new builder supports it.
