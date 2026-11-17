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
  expansions through Repentance+, version **v1.9.7.17**.
- **The executable described below.** This is the one step this guide does not
  cover.
- **[Docker](https://docs.docker.com/get-docker/).** Nothing else gets installed.
- **8 GiB of free disk and 8 GiB of memory** for the build (it ends up using 3.5 GiB
  of disk), plus 7.4 GB for the Docker image.
- **A Switch that already runs homebrew** in Application mode, with enough room on
  the card for about 1.1 GB. Preparing the console is not covered here.

The build took 45 minutes on the 16-thread machine it was measured on, 25 of them in
Ghidra, which uses up to 8 threads. You can leave it running.

## The executable

The build needs the game's executable with Steam's DRM removed. That step is
required, and this guide does not cover it: this project does not include, link to
or describe any tool or method for it. Whether and how you do it is your decision
and your responsibility under the law that applies to you. Please check that first.

The file has to be exactly this one, because every address in the build belongs to
it:

```sh
sha256sum isaac-ng.unpacked.exe
# b4743967f3d31050f7b4222e42ac6beae6a0b7850b1f1adaac07ef7b94d01ec8
```

Name it however you like; the command below mounts it under the name the build
expects. The build checks the hash first and refuses any other file within seconds.

## Build

From the root of this repository, once:

```sh
docker build -t isaac-switch-builder -f docker/Dockerfile .
```

This took about 3 minutes on a fast connection, downloads several GB, and the image
contains nothing from the game. Then, with the paths changed to yours:

```sh
GAME="$HOME/.local/share/Steam/steamapps/common/The Binding of Isaac Rebirth"
EXE="$HOME/isaac-ng.unpacked.exe"

mkdir -p isaac-build
docker run --rm --network none -u "$(id -u):$(id -g)" \
  -v "$EXE":/input/isaac-ng.unpacked.exe:ro \
  -v "$GAME":/game:ro \
  -v "$PWD/isaac-build":/work \
  isaac-switch-builder
```

The executable and the game are mounted read-only, and the build has no network:
nothing leaves your machine. You will see nine steps:

```
Checking your files (this reads the executable once)...
Executable, game folder, disk and memory are fine.
[1/9] Preparing the sources...
[2/9] Analysing the executable with Ghidra (the long one)...
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
switch/isaac-repentance-ns1-recomp.nro
switch/isaac-recomp/guest/...      the 22 game files the NRO reads
```

Your saves are not in there, so copying it does not touch them. Then start
**ISAAC REPENTANCE+ NS1 RECOMP** from the homebrew launcher in Application mode.

[PLAYING.md](PLAYING.md) covers the controls, `settings.ini`, moving your PC saves
and adding mods.

**Do not share the NRO or the `isaac-build` folder.** The NRO contains the memory
image of the executable, and the folder holds the executable and the game's files.
Everyone has to build their own.

## When something goes wrong

| What you see | What it means |
| --- | --- |
| `The executable is not the one this build needs` | The file's hash is different. A file from another game version cannot work, nor can one that differs in any other byte. |
| `... is a folder, not a file` | The path in `EXE` does not exist, so Docker made an empty folder there. Fix the path and delete that folder. |
| `does not look like the game's folder` | `GAME` has to be the folder containing `resources/` and `savedatapath.txt`. |
| `The game folder is incomplete` | Let Steam verify the game's files, or check that all the expansions are installed. |
| `Not enough disk` / `Not enough memory` | As it says. On Docker Desktop the memory is a setting: Resources > Memory. |
| `cannot be written` | Create `isaac-build` before the command, as shown, so that it belongs to you. |
| The console says **NO GAME FILES** | The copy to the card is incomplete. The screen names what is missing. |
| It returns to the launcher at once | Start it in Application mode. See [PLAYING.md](PLAYING.md#troubleshooting). |

**If Steam has updated the game**, it may no longer match this build. The build is
tied to v1.9.7.17 and has no way around that: it would have to be redone for the new
version.
