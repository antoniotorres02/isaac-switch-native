# Isaac Switch Native

A native Nintendo Switch (libnx homebrew) host for **The Binding of Isaac:
Repentance+ PC v1.9.7.17**, built on the static recompilation of
[the-browsing-of-isaac](https://github.com/xreamm1/the-browsing-of-isaac).
The same host also runs on x86-64 Linux.

The game is the real PC code, recompiled ahead of time from the Windows
executable into C and then compiled for the Switch's ARM CPU. It is not an
emulator and not a remake. On a Switch running homebrew:

- the full game plays, with graphics (EGL/GLES3 on Mesa) and sound (OpenAL);
- your PC saves carry over, including the run in progress;
- Steam Workshop mods work, Lua ones included, and the game's Mods menu turns
  them on and off;
- the buttons drawn on screen are the Switch ones, A accepts and B goes back
  in menus (both can be turned off in `settings.ini`), and holding +
  restarts a run.

**This repository contains no game code, data or assets, and no tool or
method for removing the game's copy protection.** You need your own
purchased copy of the game, and you build everything yourself from it.

## Documentation

| Document | What it covers |
| --- | --- |
| [BUILDING.md](BUILDING.md) | Requirements, the upstream pipeline, the PC build and the Switch NRO |
| [PLAYING.md](PLAYING.md) | Installing on the Switch, controls, `settings.ini`, PC saves, adding and toggling mods, playing on PC, troubleshooting |
| [DESIGN.md](DESIGN.md) | How the host works, what was changed and why, and what was measured on hardware |

## Inspiration and credits

This project exists because of
**[the-browsing-of-isaac](https://github.com/xreamm1/the-browsing-of-isaac)
by xreamm**, which made Repentance+ playable in a browser tab by recompiling
its Windows executable to WebAssembly. Its pipeline (Ghidra export, the
P-code → C lifter, the memory image, the Win32/GL/audio host and the mod
support) does the hard part. This repository adds a native host for that
lifted C: PC and Switch platform layers, the fixes the native build needed
(listed in [DESIGN.md](DESIGN.md#corrections-to-the-upstream-pipeline)) and
the Switch-specific features. It works on a checkout of upstream at a pinned
commit, with a one-line local change to its PE hash check and one patch to
its host sources (`host/patches/`).

Also used or built on:

- [REPENTOGON](https://github.com/TeamREPENTOGON/REPENTOGON): function
  signatures, through upstream;
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra) and
  [pypcode](https://github.com/angr/pypcode): analysis and lifting;
- [devkitPro](https://devkitpro.org/) (devkitA64, libnx, switch-tools) and
  its portlibs, including [Mesa](https://www.mesa3d.org/) and
  [OpenAL Soft](https://github.com/kcat/openal-soft);
- [Lua 5.3.3](https://www.lua.org/), [GLFW](https://www.glfw.org/) for the
  PC window, and [Emscripten](https://emscripten.org/) for upstream's
  pipeline;
- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere),
  [Sphaira](https://github.com/ITotalJustice/sphaira) and
  [sys-botbase](https://github.com/olliz0r/sys-botbase), the environment the
  hardware tests ran in.

*The Binding of Isaac: Repentance+* is by Edmund McMillen and Nicalis. This
project is not affiliated with or endorsed by them, by Nintendo, by Valve or
by xreamm.

## License

The code in this repository is free software under the
[GNU General Public License v3.0 or later](LICENSE), the same license as
the-browsing-of-isaac, whose code it is built and linked with.

The license covers this project's own work only. It gives no rights to *The
Binding of Isaac: Repentance+*: the game's code, assets and name belong to
their owners. Anything you build with this repository from your copy of the
game (the lifted C, the memory image, the PC executable, the NRO) contains
material derived from the game. Keep it for your own use and do not
distribute it.
