# Isaac Switch Builder

A program for Windows and Linux that turns **your own copy of the game** into the
folder you copy to the Switch's SD card, with a window that walks you through it.
It is a friendly front for [GUIDE.md](../../GUIDE.md): the same Docker build, with every
check and every step explained.

> **You need the game's executable WITHOUT Steam's DRM.** This program does not
> remove it, nor explain how to do it. Whether and how you do it is your decision
> and your responsibility under the law that applies to you: check it first.
> The program recognises Steam's original `isaac-ng.exe` and tells you that it is
> the right version but still has DRM; for v1.9.7.19 the file it needs has SHA-256
> `8c9526c1bf0910a930cba4037d120b357779940bbae34fabc8afaf56150e0b6a`
> (each version's is in `../versions/<version>/version.json`).

**Linux is tested end to end. Windows is not tested yet**: the Windows paths (Docker
Desktop, a Docker volume for the work, the copy to your folder) were exercised on
Linux, never on a Windows machine.

## Download

From the [latest release](https://github.com/antoniotorres02/isaac-switch-native/releases/latest):

- **Windows**: `isaac-switch-builder.exe`. Double-click it. (Windows SmartScreen may
  warn that it does not know the program: *More info → Run anyway*.)
  `isaac-switch-builder-cli.exe` is the same for a terminal.
- **Linux** (x86-64): `isaac-switch-builder-linux`, then `chmod +x` it and run it. It
  needs Tk, which desktop distributions already have.

## What you need

- The Binding of Isaac: Rebirth with every expansion up to Repentance+, at the
  version Steam ships today (v1.9.7.19; the program names it if yours differs),
  installed with Steam.
- The executable without DRM (see above).
- **Docker**: Docker Desktop on Windows, Docker Engine on Linux. You do not need
  to set it up beforehand: the program sees if it is missing, not running, or short
  of memory, and offers the fix (below).
- 4 GiB of memory for Docker, about 6 GiB of disk, and about 10–20 minutes.

## Using it

Run `isaac-switch-builder` (on Windows, `isaac-switch-builder.exe`). Six pages:

1. **The game**: found through Steam (Windows registry, `~/.local/share/Steam`,
   Flatpak, Snap, every library folder); every install it finds is in the list, and
   **Choose…** opens in Steam's library. It checks the files, the expansions and the
   version.
2. **The executable**: the program looks in Downloads, Desktop, Documents and the
   game's folder and picks the right file by its content, whatever its name. Files
   that might be it are in the list; **Choose…** opens in Downloads.
3. **Docker**, which this page waits for by itself, checking every 5 seconds:
   - not installed, on Windows: **Install Docker Desktop** (with `winget`, after
     Windows' administrator prompt), or **Download page**; on Linux, **How to install
     Docker**;
   - installed but closed, on Windows: **Open Docker Desktop**;
   - on Linux, not running or not allowed: **Copy the command**
     (`sudo systemctl start docker`, or `sudo usermod -aG docker $USER`);
   - too little memory, on Windows: **Open .wslconfig**, explained step by step;
   - then the tool image (downloaded once, ~230 MB; ~1 GB on disk).
4. **Ready**: where the result goes, **Check everything**, and **Start the build**.
   Steam Cloud and the number of jobs are under *Advanced options*.
5. **Building**: the nine stages, how long is left, and **Stop**. Stopping is safe:
   **Carry on from where it was** resumes at the stage it was on. The computer does
   not go to sleep while it builds.
6. **Done**: the NRO's hash, the folder, and **Copy to the SD card** (it finds
   removable cards and never touches your saves).

**Copy report** on every page puts what the program saw on the clipboard, with your
home folder shown as `~`, to paste where you ask for help.

The same from a terminal:

```sh
isaac-switch-builder check --exe ~/isaac-ng.unpacked.exe
isaac-switch-builder build --exe ~/isaac-ng.unpacked.exe     # again to resume
isaac-switch-builder copy-to-sd /media/me/SWITCH
isaac-switch-builder clean                                   # delete the work files
```

`--lang es|en` chooses the language (by default, the system's).

**Do not share the NRO or the result folder**: they hold the executable's image and
the game's files. Everyone has to build their own.

## How it works

The program never builds anything itself. It runs
[build_all.py](../build_all.py) inside the pinned image of
[docker/Dockerfile](../../docker/Dockerfile), offline (`--network none`), with the
executable and the game mounted read-only, and reads its progress as JSON lines
(`--progress json`).

| | Linux | Windows |
| --- | --- | --- |
| Work files (~3 GB) | `<folder>/work`, a normal folder | the Docker volume `isaac-switch-work`: a Windows folder mounted into Linux is slow and ignores letter case |
| Result | `<folder>/work/out/sd-card` | copied to `<folder>/sd-card` at the end (`--export`) |
| Default folder | `~/IsaacSwitchBuild` | `Documents\IsaacSwitchBuild` |

| File | What it does |
| --- | --- |
| `isb/steam.py` | Steam libraries, `appmanifest_250900.acf`, build and expansions |
| `isb/checks.py` | the checks, as ✔ / ⚠ / ✘ with what to do |
| `isb/dockerctl.py` | Docker status, the image, the container command, stop |
| `isb/progress.py` | stages, progress and time left from the measured durations |
| `isb/sdcard.py` | removable cards, the copy and its verification |
| `isb/system.py` | sleep inhibition, opening folders, `.wslconfig`, redaction |
| `isb/texts.py` | every text, in Spanish and English |
| `isb/cli.py`, `isb/gui.py` | the terminal and the window (Tkinter) |
| `isb/image.json` | the image: the published digest, or the local tag until there is one |

Tests: `python3 tools/test_builder.py` (no Docker or game needed) and
`python3 tools/test_build_all.py`.

## Building the program

```sh
pip install pyinstaller==6.16.0
python package.py            # dist/isaac-switch-builder-linux (~14 MB)
```

On Windows it makes `isaac-switch-builder.exe` (window, no console) and
`isaac-switch-builder-cli.exe` (terminal).

## Publishing (maintainers)

The image has to be public before the program is useful to others:

1. Push a tag `image-vN` to the public repository: `.github/workflows/image.yml`
   builds `docker/Dockerfile` and pushes it to
   `ghcr.io/<owner>/isaac-switch-builder`. The first time, make the package public.
2. Write the digest it prints into `isb/image.json` (`"digest": "sha256:…"`).
   Until then the program uses a local `isaac-switch-builder:latest`, and offers to
   build it when it runs from a checkout.
3. Push a tag `vX.Y.Z`: `.github/workflows/release.yml` builds the program on
   Windows and Linux and attaches it to the release.

The image contains the tools only (devkitA64 and the Switch libraries the NRO links,
Python, upstream's pipeline at a pinned commit), nothing from the game. The game
versions it can build come with the program's source, in `../versions/`.
