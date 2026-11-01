# Native host for the the-browsing-of-isaac lift (PC and Nintendo Switch)

This directory runs **The Binding of Isaac: Repentance+ PC v1.9.7.17**
natively on x86-64 Linux and on the Nintendo Switch (libnx NRO). It reuses the
C code that [the-browsing-of-isaac](https://github.com/xreamm1/the-browsing-of-isaac)
lifts from the Windows executable (Ghidra P-code → C) and replaces that
project's WebAssembly/browser host with a native one: EGL + GLES3 for
graphics, OpenAL Soft for audio, a real filesystem for saves and mods, and the
Switch pad mapped onto the game's own XInput controller path.

The game is fully playable on the Switch: the PC saves and the Steam Workshop
mods carry over, mods can be turned on and off from the in-game menu, and the
buttons drawn on screen are the Switch ones.

| Document | What it covers |
| --- | --- |
| [BUILDING.md](BUILDING.md) | Building everything from your own copy of the game: the PE, the upstream lift, the PC executable and the Switch NRO |
| [PLAYING.md](PLAYING.md) | Installing on the Switch, controls, `settings.ini`, moving your PC saves, adding and toggling mods, playing on PC, troubleshooting |
| [STEAM-CLOUD.md](STEAM-CLOUD.md) | Syncing the saves with the account's Steam Cloud from the console itself: provisioning, conflict rules, measured protocol, risks |
| This file | How the host works, what was changed and why, and what was measured |

> **Nothing from the game is in this repository, and nothing built from it may
> be shared.** You need your own Steam copy of Repentance+. The lifted C, the
> memory image (`isaac.segs.bin`) and therefore the NRO are derived from the
> game's executable. Everyone has to build their own. The upstream code is
> GPL-3.0-or-later, so a binary that combines it is under the GPL-3 as well.

## Layout

```
host/
  build_native.py               build driver (profiles debug, asan, release, gl, switch)
  upstream.json                 pinned upstream commit, PE hashes and toolchain versions
  extra_code_pointer_starts.py  the 519 function entries the upstream lift misses
  import_pc_saves.py            Steam profile -> native save store
  steam_cloud_cm.py             Steam CM protocol: token provisioning, cloud read/write
  patches/                      changes to the upstream host sources (0001-0007)
  bench/isaac-bench/            the benchmark mod (tools/bench_native.py)
  native/
    native_main.c               entry, memory image, instance/RAM-FS seeding, saves, mods, clock
    native_mem.c                guest address window
    native_gl.c                 EGL context (pbuffer, GLFW window or Switch nwindow)
    native_glq.c, native_glq.h  the render thread: GL calls through a command queue
    native_audio_al.c           OpenAL Soft backend for the host's audio hooks
    native_input.c              scripted input, game tick probe, injected keys
    native_window.c             PC window and keyboard/gamepad (GLFW)
    native_switch.c             Switch entry: pad, settings.ini, test protocol, heap hygiene
    native_gamefiles.c          are the game's files on the card? (every build)
    native_steamcm.c            Steam CM session: TLS, websocket, Cloud.* calls (Switch only)
    native_steamsync.c          pull/push rules, state file, backups, conflict handling
    native_steamproto.c         the protobuf fields and CM framing the client needs
    native_zip.c                the ZIP wrapper Steam stores a cloud file in
    native_sha1.c               file_sha
    native_glyphs.c             Switch button prompts (in-memory .anm2 rewrite)
    native_lua_ramfs.c          Lua loads files through the RAM-FS
    native_lua_state.c          the game's Lua state: is Mod Config Menu open, is Lua running?
    native_stream_path.c        a doubled '/' in a path the engine opens (mod metadata)
    native_bench.c              frame timing for benchmarks (BENCH line)
    native_prof_switch.c        Switch: A57 sampler, clock readout, hang watchdog
    compat/                     small newlib/glibc gaps
```

The upstream checkout is kept outside Git at `build/browsing-port/repo`
(commit `22be498c60aad2f8abcf010cd408422385fb58a3`). `build_native.py` copies
its host sources into `build/browsing-native/src`, applies our patches and
compiles them together with the lifted translation units, Lua 5.3.3 and
`native/`. Nothing is written back into the upstream checkout.

## How it works

### Memory model

The lift is built with `RECOMP_MEM_IDENTITY=0`, so every guest access is
`recomp_mem_base + va`. `native_mem.c` reserves a 4 GiB window and only opens
the guest map (`0 .. ISAAC_HOST_BASE_VA`) for reading and writing, so any
32-bit address lands inside the reservation and anything above the map
faults.

The native guest heap is not the web one (384 MiB at `0x00d00000`): the patch
to `isaac_host.h` moves it to `0x1c100000` with 768 MiB, and the map becomes
`0 .. 0x4c200000`. The reason is mods: leaving the mods menu reloads every
mod's resources while the old ones are still alive. With 17 mods the measured
peak was 421.5 MiB, and with 384 MiB a 32 MiB font reservation failed and the
game closed. The fixed regions below the heap (TEB, stack, the shim table at
`0x1b000000`) did not move.

The host converts with the helpers in `isaac_host.h`:

- `isaac_g(va)` keeps the wasm meaning: guest address 0 is readable memory.
- `isaac_gp(va)` returns `NULL` for 0. It is the one external APIs (Lua)
  receive, where `NULL` means "absent".
- `isaac_va(p)` aborts with a backtrace (`isaac_va_escape`) if a host pointer
  outside the window would reach the guest, instead of truncating it.

### Changes to the upstream host (`patches/`)

`0001-native-guest-window.patch`, all of it under `ISAAC_NATIVE`:

- base-relative addressing in `isaac_host.h`, `boot_integration.c` (image,
  guard, layout) and the `cpuid` scratch of `recomp_rt.c`, plus the native
  heap layout above;
- `GlobalAlloc` allocates in the guest arena, not with the host `malloc`;
- `vsscanf` converts the pointers in its varargs with `isaac_g`;
- `lua_getinfo`/`lua_getstack` translate `lua_Debug` to the 32-bit layout of
  the game's Lua DLL (100 bytes), and Lua's static strings (`lua_typename`,
  `what`) are copied into the guest arena;
- scripted input enters through a C hook instead of `EM_JS`;
- `ISAAC_REAL_GL`: the web GLES3 backend (`host_gl_webgl.c`) also compiles
  with `ISAAC_NATIVE_GL` on top of an EGL context (`native_gl.c`);
  `glGetBufferSubData` becomes `glMapBufferRange`;
- audio: `native_audio_al.c` implements the weak `isaac_audio_backend_*`
  hooks of `host_audio.c` with OpenAL Soft (the PCM is copied on the host,
  one AL buffer per stream entry), and the driver gives
  `isaac_audio_clock_ms` the monotonic clock (outside Emscripten the default
  is 0, and the music stream then never sees processed buffers);
- the `lua_pcallk` shim restores the guest `jmp_buf`, `g_guest_call_esp` and
  `fs:[0]` when a Lua error jumps over a guest binding (on Windows the Lua
  DLL's `longjmp` unwinds SEH). Without it the engine could not catch the
  `LuaException` of a failing mod callback;
- `GetModuleFileName`/`GetCurrentDirectory` answer the virtual root `/`, as
  in the browser (the engine looks for `mods/` and `data/` next to the exe);
- newlib gaps for the Switch: `timegm` (checked against glibc on 200,000
  dates), `pread`, `sysconf`;
- an equivalent boot-layout check for the native window.

The other patches are about speed (see [Performance](#performance)):

- `0002-lift-const-base.patch`: the lifted units read the guest base through
  a `const` alias and see the shared globals with hidden visibility;
- `0003-native-render-gate.patch`: the upstream render gate on the native
  clock (`ISAAC_RENDER_GATE`). On the wall clock a drawn in-between frame
  also waits for the next tick's start, as on the web: vsync paces the
  presents, not the ticks, and without the wait the game ran 5 % fast
  (31.8 ms ticks in the starting room, 33.2 ms with it);
- `0004-snd-free-regions.patch`: a guest `free` skips the search over the
  lazy sound records when its 64 KiB region never held one;
- `0005-gl-location-one-pass.patch`: uniform and attribute locations are
  looked up by name pointer first, then by a hash computed in one pass;
- `0006-native-gl-render-thread.patch`: the GL backend includes
  `native_glq.h`, so its GLES calls go through the render thread's queue;
  the footstep pixel readback and the periodic `glGetError` drain run on that
  thread instead of waiting for the queue;
- `0007-readpixels-sync-for-mods.patch`: a 1x1 readback made while Lua runs
  (a mod's `Sprite:GetTexel`) is exact and synchronous; only the engine's own
  footstep read keeps the one-read-late answer (see [Mods](#mods)).

`build_native.py` also edits generated files on their way to the compiler:
`lifted_000.c` gets the `.anm2` hook, `lifted_032.c` the stream-open path
hook, and the cache hit of
`dispatch_tbl.c` no longer updates the per-entry census (a random write into
a 400 KB array on every indirect call; only the exit report read it).

Lua 5.3.3 is built with `LUA_EXTRASPACE 4`, like the game's 32-bit DLL (the
game uses `lua_getextraspace(L) == L - 4`). `luaL_loadfilex` reads from the
RAM-FS first (`native_lua_ramfs.c`; the original stays as
`luaL_loadfilex_disk`), so `dofile`/`loadfile` from mods find their files.

### Corrections to the upstream pipeline

These are needed with our executable and are part of [BUILDING.md](BUILDING.md):

1. The executable is the image with SHA256 `B4743967…` and the standard
   section names; variants with renamed sections break upstream's census and
   Ghidra scripts. Upstream recorded two other hashes (`5129DF72…`,
   `1FBA9A37…`); the structure matches their documentation, so
   `scripts/decomp/tools/pe.py` is changed locally to expect ours.
2. pypcode **4.0.1**, not the 3.3.3 upstream documents: their
   `lift_patches.py` matches 4.x temporary names, and with 3.3.3 the block
   patch at `0x00aa27f6` fails.
3. `export-all.mjs --stage all` without `--max-functions` (it also caps the
   inventory).
4. `scripts/decomp/tools/build-pe-index.py` before the lift.
5. The Lua tarball is extracted by hand into `tools/lua-5.3.3` (their
   `lua_build.py` fails to extract it).
6. 519 extra function entries. The game's Lua bindings hold small getters as
   plain addresses (`push imm32`, `mov r/m32, imm32`) and call them
   indirectly, for example `0x0086fc60` (`mov eax, [0x00c71678]; ret`, the
   Game getter). Ghidra never creates those functions, so the first mod that
   calls such an API traps with "indirect call … resolves to neither a host
   shim nor a lifted function". `extra_code_pointer_starts.py` finds them and
   they are appended to `starts_union.txt` before the lift.
7. `ISAAC_CALL_SLIM=0` for `build_boot.py`, run on an untouched copy of the
   lift. Upstream's `apply_call_slim` removes the `s->EIP = EIP` of every
   `ret`, and resuming after a C++ `catch` or a game `longjmp`
   (`isaac_guest_call_inner`) needs it: without it the host repeats the same
   continuation until EBP reaches 0.

### Saves

`ISAAC_SAVE_DIR=<dir>` keeps what the game writes (save slots, `options.ini`,
`log.txt`, `save_backups/`, mod data) with the Node driver's convention: the
key without the `c:/isaac/` root, atomic writes. Everything is restored into
the RAM-FS at the next start. The PC game keeps its slots in Steam Cloud as
`rep+persistentgamedataN.dat`/`rep+gamestateN.dat`; with Steam stubbed, the
native build reads the local names. The bytes are the same file (same exe),
only the name changes, which is what `import_pc_saves.py` does (see
[PLAYING.md](PLAYING.md#moving-your-pc-saves)).

### Mods

`ISAAC_NATIVE_MODS=1` mounts the instance's `mods/` the way upstream's
`mods.mjs` does: each enabled mod under `c:/isaac/mods/` and under the
Documents `mods/`. A disabled mod is mounted with only `metadata.xml`,
`thumbnail.png` and an empty `disable.it`, so the menu lists it as off and it
can be turned back on; none of its content is loaded.

The mods menu toggle writes or removes `c:/isaac/mods/<folder>/disable.it`.
The host records the choice as `<ISAAC_SAVE_DIR>/mod-state/<folder>.off` or
`.on` and applies it at the next start, overriding the `disable.it` of the
instance. Three engine behaviours had to be filtered for the choice to stick:

- the engine deletes `disable.it` under the Documents root by itself at every
  start, so that deletion is ignored;
- `disable.it` files are never mounted from the mod folder or restored from
  the save store, so the engine only sees the one the host decides on;
- `mods/*/metadata.xml` is never restored from the save store (the engine
  rewrote it while it could not read it, see below).

The engine reads a mod's metadata from `<mod folder>/` + `/metadata.xml`.
Windows reads the doubled `/` as one, and on PC the mod folder is an absolute
path; here it is the engine root `//mods/<mod>/`, which the engine's stream
open (`0x00a178d0`) looks up among the files it listed at start, and
`…/<mod>//metadata.xml` was not among them. Every mod then had no metadata:
its `<name>` became the folder name, so mods ran in folder order instead of
name order (PC runs ` Mod Config Menu - Impure` first and `!!~External item
descriptions` second; here EID ran before MCM and found no menu to add its
settings to), mod data went to `data/<folder>/` instead of
`data/<directory>/`, and the engine rewrote each `metadata.xml` with
defaults. A hook at the stream open (`native_stream_path.c`, `LIFT_HOOKS` in
`build_native.py`) hands it a copy of the path with each run of `/` after the
leading `//` collapsed; the load order, `<name>`, `<version>` and data folder
now match a Steam PC log.

Mod save data (`Isaac.SaveModData`) lives in `data/<metadata directory>/`, as
on PC, and the importer copies it there. Saves made by earlier builds keep it
in `data/<folder>/`: when `data/<directory>/` does not exist, the host
restores the old folder under the new name (`[native] mod data: …` at boot),
and the game's next save goes to the new one. The old folder is left alone.

Mods that read pixels (`Sprite:GetTexel`) get the exact pixel: patch 0007
keeps upstream's one-read-late answer to 1x1 `glReadPixels` (the footprint
colour) only for reads made outside Lua. EID compares a treasure-room
pedestal with the `?` sprite this way; served the previous read, every point
matched and every treasure-room item counted as a hidden alt-path item.

The engine ignores mod folders without `metadata.xml`, and it also ignores
loose files under the instance's `resources/` (it reads the packed archives
first), so graphics cannot be replaced by dropping files there. The Switch
button prompts below are therefore part of the host, not a mod.

### Switch button prompts (`native_glyphs.c`)

The PC exe only chooses between the `XboxOne`, `PS4` and `PS5` glyph sets by
controller type (`0x0095afb8`), but its `gfx/ui/buttons.anm2` and
`gfx/backdrop/controls_buttons.anm2` also contain Nicalis's Switch frames
(`Switch`, `Switch_Pro`) on the same sprite sheets.

The `.anm2` loader (`sub_0040ed50`) hands each file's XML text to rapidxml.
`build_native.py` (`LIFT_HOOKS`) writes a copy of `lifted_000.c` to
`build/browsing-native/lift-hooked/` with a call to
`isaac_hook_anm2_text(ESP)` right after `RECOMP_VA(0x40ee80u);`, where `[ESP]`
is that text pointer (the anchor must appear exactly once, or the build
stops). `native_glyphs.c` recognises the two sheets by their
`Path="…png"` attribute and gives the parser a copy, allocated in the guest
heap, whose `XboxOne` frames crop the Switch glyphs instead. Every other
animation and every other file is left alone, and nothing from the game is
stored in the source: the copy is made from the text the game has just loaded.

Nicalis's `Switch` frames go by position (Xbox Y → Switch X, A → B, B → A,
X → Y). With A accepting in menus, the four face frames of `buttons.anm2` are
reversed so the accept prompt shows A. The first room's floor drawing
(`controls_buttons.anm2`) is about shooting, which stays by position. In both
sheets View/Back and Menu/Start swap, because here − is BACK and + is START.

`ISAAC_NATIVE_SWITCH_GLYPHS`: `0` or unset, off; `1`, prompts by label (A
accepts); `2`, prompts by position. On the Switch, `settings.ini` sets it.

### Switch pad (`native_switch.c`)

The pad is reported through the game's own XInput path. The host tells play
from menus with the game's tick counter (`[0x00c71678] + 0x264f8`): it only
advances in an unpaused run (measured: it holds still while paused and reads
0 in the menus).

- **Face buttons** go by position in play, because they shoot in four
  directions (bottom shoots down, like Xbox A), and by label in menus (A
  accepts, B goes back). With `a_accepts=0` the menus also go by position. A
  held button keeps the meaning it had when it was pressed, so pausing or
  resuming in the middle of a press does not turn it into another action.
- **Mod Config Menu** draws over a running game, so the tick alone would read
  it as play. The host also reads `ModConfigMenu.IsVisible` from the game's
  Lua state (`native_lua_state.c`, raw reads, no mod code runs; the state
  comes from Lua's `luai_userstateopen/close` hooks); while it is true the
  pad is in menu mode for +, and the face buttons follow MCM's own
  numbering. MCM selects with the game's button 4 and goes back with 5,
  which it calls A and B, but the game numbers an Xbox pad X 4, B 5, Y 6,
  A 7 (a PC's `inputconfigs.dat` for an Xbox 360 pad; on the console, Switch
  A sent as Xbox A reached MCM's rebind screen as its "Y"), so with an Xbox
  pad MCM selects with X, on PC too. Here A goes as Xbox X (select) and B as
  Xbox B (back); with `a_accepts=0` the bottom button selects and the right
  one goes back.
- **+** outside a run is START. In a run, a tap (shorter than 400 ms) pauses
  when it is released, and holding it restarts the run: the host presses the
  game's own restart key (R) through `isaac_native_input_key` and releases it
  as soon as the tick starts over (measured on PC: the game restarts after
  about 70 frames of R, and again if R stays down).
- − is BACK; L/R, ZL/ZR, the sticks and their clicks, and the D-pad map to
  their Xbox counterparts.
- HOME → Close leaves through the same `WM_QUIT` as the frame budget, so the
  game shuts down normally and writes its saves.

### Clock

The host's `QueryPerformanceCounter` is deterministic (100 ns per query,
`Sleep` adds its argument), which is what the tests use. Anything that times
itself, such as the intro animation against its audio, then ran at the speed
of the queries (367 anm2 timing corrections during the intro).
`ISAAC_NATIVE_WALL_CLOCK=1` uses the real clock, as the web build does when
playing: 0 corrections. The PC window and the Switch play mode turn it on.

### Switch specifics

- RomFS holds `isaac.segs.bin` (the memory image) and `instance_index.json`
  (the 22 game files the web build mounts; the language packs are left out).
- The game files are read lazily from `sdmc:/switch/isaac-recomp/guest/`,
  saves go to `sdmc:/switch/isaac-recomp/saves/` (`saves-autotest/` for the
  test protocol), and play mode always mounts `guest/mods/`.
- The 1,218 MiB guest map comes from the libnx heap (checked with
  `svcQueryMemory` before use). Unlike on PC there is no guard page above it.
  This needs Application mode; in applet mode the NRO refuses to start.
- EGL on `nwindowGetDefault()` at 960x540 with vsync (the compositor scales
  it to the screen), Mesa nouveau (NV120, OpenGL ES 3.2), OpenAL from
  portlibs. The game runs on its own 8 MiB-stack thread.
- `--autotest --run-id <32 hex> [--return-to-sphaira] [--frames N]
  [--mods] [--no-script]` follows the `tools/run_switch_tests.py` protocol:
  `begin`/`end` receipts over nxlink with `synthetic`, `graphics` and `audio`
  results, `game_tick`, and a PPM capture in
  `sdmc:/switch/isaac-recomp/shots-<run>/`. `--no-script` leaves the game to
  the pad.
- Each start logs its stages to `sdmc:/switch/isaac-recomp/browsing-last-run.txt`.

#### hbloader's shared heap

hbloader runs every NRO in a single process with a single heap. The EGL
context stayed current on the game thread, which creates it and renders; the
shutdown ran on the main thread, so `eglTerminate` never returned and was
skipped. Mesa's GPU buffers then stayed in the heap, mapped to the GPU and
marked uncached (memory attribute 12). The next NRO in the same process got
that memory from `malloc`, and `memset` (`dc zva`) or the `malloc` headers
crashed: "The software was closed because an error occurred" when launching
again from Sphaira. Atmosphère's crash reports showed `_malloc_r` from
`seed_mods` and `memset` in `isaac_native_mem_init`; a second start in the
same process found more than 60 such regions above the previous map.

The fix:

- the game thread releases the context (`eglMakeCurrent` +
  `eglReleaseThread`) before it ends;
- `eglTerminate` is called again on the Switch;
- shutdown closes nvdrv (`nvExit` until it is really closed) and clears the
  uncached attribute;
- at start the heap is scanned, and anything another NRO left behind is
  fenced off under a block that is never freed.

Measured: after `eglTerminate` one 2 MiB region stays marked; the next start
in the same process finds 0.

The same rule applies to services: a session an NRO leaves open stays in the
process. The benchmark build read the clocks through `clkrst` and never closed
the service. Each run left one pcv session behind, and the fifth benchmark in
a row froze the whole console. Atmosphère's fatal report named pcv, with
`2011-0102` (out of session memory). `native_prof_switch.c` now opens and
closes the service around each read. After the fix, five runs in a row
passed.

## Environment variables

| Variable | Meaning |
| --- | --- |
| `ISAAC_SAVE_DIR=<dir>` | persistent save store (created if missing) |
| `ISAAC_NATIVE_MODS=1` | mount the instance's `mods/` |
| `ISAAC_NATIVE_WINDOW=1` | PC: GLFW window at 960x540, 60 fps, keyboard and first gamepad; implies the wall clock |
| `ISAAC_NATIVE_WALL_CLOCK=1` | real time instead of the deterministic clock |
| `ISAAC_NATIVE_SWITCH_GLYPHS=0/1/2` | Switch button prompts: off / by label / by position |
| `ISAAC_NATIVE_AUDIO=0` | no audio (`ALSOFT_CONF` with `drivers=wave` records the mix to a WAV) |
| `ISAAC_NATIVE_FPS=60` | headless: pace presents with the clock |
| `ISAAC_NATIVE_SHOTS=600,1200`, `ISAAC_NATIVE_SHOT_DIR=<dir>` | save those presented frames as PPM |
| `ISAAC_NATIVE_INSTANCE_LIST=<json>` | mount only the files of an `instance_index.json` (the Switch uses it) |
| `ISAAC_MAX_FRAMES=N` | stop after N presented frames |
| `ISAAC_INPUT=...` | scripted input, the upstream Node driver grammar: `frame:key[:frames]`, `frame:mouse:x:y`, `frame:click` |
| `ISAAC_NATIVE_TICK_EVERY=N` | log the game tick every N frames (default 300) |
| `ISAAC_RENDER_GATE=0/1/4` | the in-between frame: always drawn / skipped when it would make the next tick late / always skipped (Switch play mode: `frame_skip` in `settings.ini`) |
| `ISAAC_NATIVE_BENCH=<load>[:<stage>]` | benchmark: time the frames of `ISAAC_NATIVE_BENCH_FROM`..`_TO` (1500..2700) and print one `BENCH {...}` line; `ISAAC_NATIVE_BENCH_MOD=<dir>` seeds the bench mod |
| `ISAAC_NATIVE_VSYNC=1` | Switch benchmark: keep vsync (a benchmark turns it off) |
| `ISAAC_NATIVE_GLQ=0` | GL on the game thread instead of the render thread (Switch play mode: `render_thread=0` in `settings.ini`) |
| `ISAAC_NATIVE_GLQ_CORE=<n>` | Switch: the render thread's core (default 1; the game thread runs on 0) |
| `ISAAC_NATIVE_VBLANK_PACE=0` | Switch with vsync: pace the passes with the render gate's wall clock instead of one vblank each (see [Frame pacing](#frame-pacing-native_glc)) |
| `ISAAC_NATIVE_WATCHDOG=<s>` | Switch: print the game thread's PC and LR every s seconds (`WATCHDOG` lines) |

## Measured results

All dates are 2026-10-25.

### PC

- Full boot: 117 C++ initialisers, `main`, GL/OpenAL/XInput, Lua
  (`enums.lua`, `main.lua`) and `Room 1.2(Start Room)`.
- `release` (fake GL): 3,200 presented frames with ticks at 30 Hz (150 every
  300 frames); 2,000 frames in a room cost about 0.6 s of CPU.
- `gl` profile (RTX 3060, GLES 3.2 through EGL): 2,001 frames, 2.17 M GL
  calls, 0 GL errors; captures of the intro, the starting room and Isaac
  moving and shooting.
- Audio (`gl`, 60 fps, 1,500 frames, `wave` driver): 25.6 s of continuous
  mix, 164 buffers uploaded, 145 stream buffers queued, 0 AL errors.
- Against the original: the same executable under Wine 9.0 reaches the
  same starting room with the same HUD, map and art. Its `log.txt` up to
  `Start Room` (307 lines) matches the native log at 75 %; the differences
  are paths, GL/AL vendor, seed and rooms, the EOS stub, language packs not
  mounted, the Lua reservation limited by the host (128 MiB), upstream's
  cutscene fixes, and the missing `Viewport/Framebuffer` and libtheora
  version lines.
- Saves: two starts in a row; the first writes `persistentgamedata1-3.dat`,
  `gamestate1.dat`, `options.ini` and `save_backups/` (the same files as the
  original under Wine) and the second restores them.
- Mods: the user's 17 mods (16 enabled, 12 with Lua; EID, Mod Config Menu,
  Enhanced Boss Bars…) with their saves: the three slots and the run in
  progress load (`[Continue, 1]`); a test mod calling `error()` logs
  `Error in "PostGameStart" call` as on PC and the game continues. Mod
  toggles: off → restart → on → restart leaves 16/17/16 enabled as chosen,
  ASan/UBSan clean.
- Switch glyphs: an ASan/UBSan harness over the files extracted with the
  game's own `tools/ResourceExtractor` gives the hand-checked `XboxOne`
  frames, and every other animation and `.anm2` stays byte-identical; with
  `0` nothing is touched.

### Switch (Switch Lite, Atmosphère, Sphaira 1.0.8, Application mode)

Every run below is a **global PASS** of `tools/run_switch_tests.py`, with the
return to Sphaira observed through a sys-botbase capture
(`operator-observation.json` next to the run). The receipts keep
`process_exit_verified=false`.

| Run | NRO SHA256 | What it showed |
| --- | --- | --- |
| `8d59abb76b1346a3b2d276d32edce6c5` | `cb5117e5…` | first PASS: scripted input to the starting room, `game_tick` 565, 1,501 frames in 40 s including boot, 0 GL/AL errors, same ticks as PC |
| `e392f8a1d3e1464b93c1b336292a40b9` | `0fbff04c…` | `--mods` with the user's PC saves: the same run continued (seed `LXC1 DPH4`), no mod errors |
| `e5360778ee0e4087b0500568bf581361` | `fff3afac…` | heap fix: third start in a row in one hbloader process, 18 mods, `game_tick` 4213 |
| `f823a9cdff3b4d45bda13437aa839814` | `f81d951b…` | pad by sys-botbase: Switch prompts on the file select (BACK on B, SELECT! on A), A advances and B goes back, holding + about 3 s moved from a later room to a new starting room, the floor drawing shows Switch glyphs |
| `283cb712de304675bf53868ea2d545f1` | `f81d951b…` | a tap of + opens the pause screen and B closes it |
| `8dceb132a08149fab437a0ce876fae68` | `22948434…` | 2026-10-27, mods as on a Steam PC: load order by `<name>` (MCM, EID, Enhanced Boss Bars first), six mod data folders read under their PC names, EID listed in Mod Config Menu, A selects and B goes back there, an EID option changed and restored |

Earlier runs without a valid PASS, kept for the record: `390ffe59…` (the NRO
rejected the runner's `--graphics` argument), `fea346e7…` and `1f848d09…`
(`eglTerminate` did not return), `b8a98708…` and `6101e6be…` (the shared-heap
crash reproduced), `d9fd869e…` (closing nvdrv without releasing the context
hung), `71a84e15…`, `ea156560…` and `c21d61d8…` (no crash, but `game_tick`
read 0 because the tick probe was still bounded by the old guest limit
`0x1bf00000`).

The user then played a 39,951-frame session on the console with their own
saves and mods; it ended normally with the heap fully released.

## Performance

### Benchmarks

`tools/bench_native.py` runs the same scenario on PC or on the console and
saves the result under `build/bench/<run>/`:

```sh
python3 tools/bench_native.py pc horde:3b --repeat 3
python3 tools/bench_native.py switch horde:3b --profile        # A57 samples, profile.txt
python3 tools/bench_native.py switch room --vsync --extra "--env ISAAC_RENDER_GATE=1"
python3 tools/bench_native.py report
```

A scenario is `<load>[:<stage>]`. The load is one of:

- `room`: the starting room;
- `horde`: 36 gapers;
- `swarm`: 72 attack flies;
- `tears`: 36 gapers, and Isaac fires four tears at once.

The stage is the debug console's `stage` argument (`1c` Downpour, `3b`
Flooded Caves, ...). The bench mod (`bench/isaac-bench`) does the rest:

- it starts the run with seed `GZQW B4T8`;
- it moves to the stage and spawns the load;
- the player takes no damage and the enemies have a million hit points;
- the scripted walk stays inside the starting room.

The clock is the deterministic one, so a scenario is the same game work on
every run and build. On the Switch, benchmarks turn vsync off unless asked.
`--profile` samples the game thread's PC from another core at 1 kHz and
resolves the samples against the build's symbols.

The `BENCH` line also gives the time per game tick (`tick_ms`). That is the
number that says whether the game keeps its speed: 33.3 ms or less is full
speed.

### What the profile showed

On the Switch the frame is bound by the CPU:

- the swap takes about 1 ms;
- the A57 profile is flat: no function above 5 %;
- about half of the time is lifted game code, about a fifth is Mesa,
  nouveau and IPC, and the rest is the host (indirect calls, uniform
  lookups, the guest heap).

The water floors cost about twice the Basement, because their reflections
draw every sprite twice.

### The render thread (`native_glq.c`)

The game thread does all the work on one core while the console has three
for an application. So the EGL context now lives on a second thread, pinned
to core 1, and the GL backend's GLES calls (`native_glq.h` renames them to
`glq_` wrappers) go into a 32 MB ring that thread replays:

- A call without a result is queued with a copy of the memory it reads
  (uniform values, buffer and texture bytes), so the caller may reuse that
  memory at once.
- A call with a result (`glGet*`, `glGen*`, `glCreate*`, a map, a readback)
  waits until the render thread has run it. These happen while textures and
  shaders load; in play, none happen per frame. The exit log lists them:
  `[native][glq] N commands, M waits; waited for: ...`.
- The swap is queued behind the frame's draws. The next swap first waits for
  it, so the game runs at most one frame ahead of what the GPU gets (one more
  frame of input latency).
- The game thread publishes its commands every 4 KB, not one at a time:
  publishing each one cost it 2 % of a frame.

In the heavy room this takes Mesa, nouveau and the swap's IPC (about a fifth
of the frame) off the game thread. `ISAAC_NATIVE_GLQ=0`, or
`render_thread=0` in `settings.ini`, keeps everything on one core.
Mesa's own glthread would do the same, but it does not start on libnx (see
below).

### Frame pacing (`native_gl.c`)

With the render thread the swap no longer blocked the game on the vblank:
the render gate's sleeps on the wall clock set the pace, and they did not
know where the vblank was. The image looked fluid but jumped. A thread on
core 2 now counts the display's vsync events (libnx's default window is
restated in `native_gl.c` so its display can give the event; a second open
of `Default` fails with `0x1272`), and every swap records how many vblanks
its frame and its tick took (`vsync Ns:` lines on nxlink every 10 s, and in
`browsing-last-run.txt` at exit).

The game ticks on every other pass of its loop, not on a clock, so the fix
is to give each pass exactly one vblank:

- a drawn frame is swapped right after the vblank before its slot, on the
  render thread (or the game thread without it); a pass whose in-between
  render is skipped takes its slot without a swap, and the frame before
  stays on screen; a late pass presents at once and the slots continue
  from there;
- the gate's sleeps are off (patch 0003);
- with `frame_skip=1` the display stays at 60 fps; there is no held 30 fps
  mode. Each tick estimates its pass and its in-between render, without the
  waits for the display, and skips that one in-between frame only when the
  estimate is within 0.5 ms of the 33.3 ms budget
  (`ISAAC_NATIVE_PACE_MARGIN=<ms>`), or within 6 ms right after a tick that
  took three vblanks. The frame before then shows for two vblanks and the
  game keeps its speed. An earlier version held 30 fps with hysteresis; it
  locked the heavy rooms at 30 although their ticks fit at 60 with 5 ms to
  spare on average, and switching was a visible jump of its own.

The `vsync` lines also give, per frame, how long the game thread waited for
the render thread (`game wait`), how long the render thread had nothing to
run (`render idle`), the mean estimate (`need`) and the in-between frames
skipped per decision.

Ticks that took the two vblanks they should (vsync, wall clock,
`frame_skip=1`, render thread, `horde:3b` unless noted, steady 10 s windows):

| CPU | Build | Ticks on 2 vblanks | Jumps per 10 s | In-between frames |
| --- | --- | --- | --- | --- |
| 1785 MHz | before, starting room | 98-99 % | 1-6 | almost all drawn |
| 1785 MHz | before | 93-95 % | 13-20 | mostly drawn |
| 1020 MHz | before | 78-82 % | 55-60 | 15-25 % drawn, alternating |
| 1020 MHz | before, `render_thread=0` | 78-80 % | 45-60 | none drawn |
| 1020 MHz | vblank slots (`77c8e020…`) | 99-100 % | 0-3 | none drawn (steady 30 fps) |
| 1020 MHz | vblank slots, starting room | 99.3-100 % | 0-2 | all drawn (60 fps), except about 3.5 s at 30 where the scenario's load nears the budget |
| 1020 MHz | 60 only, faster game thread (`0b5760ad…`) | 100 % | 0 | all drawn (600 frames in 600 vblanks); `swarm:5c` and `tears:1` the same |

The game keeps its speed (`tick_ms` 33.36 and 33.39). With vsync off (the
benchmarks) nothing changes.

### Results (Switch, CPU 1020 / GPU 307 / memory 1331 MHz, stock clocks)

`horde:3b` (Flooded Caves, 36 gapers), no vsync:

| Build | ms per frame | ms per tick |
| --- | --- | --- |
| before (NRO `4a68bb55…`) | 29.34 | about 58.7 (two frames) |
| const base, dispatch census out, sound filter (`9ee881fb…`) | 27.62 | 55.2 |
| + location front cache (`3acf9949…`) | 27.09 | 54.2 |
| same, `-O3` for the lifted units (`f818bd34…`) | 26.90 | 53.8 |
| `9ee881fb…` with `ISAAC_RENDER_GATE=4` (in-between frame skipped) | 32.86 | **32.9** |
| + render thread (`d6091595…`) | 19.30 | 38.6 |
| + render thread, publishing every 4 KB (`1f9e27af…`) | 19.04 | 38.1 |
| final (`94bd0543…`, adds `render_thread` to `settings.ini`) with `ISAAC_RENDER_GATE=4` | 23.44 | **23.4** |
| final with `ISAAC_NATIVE_GLQ=0` (the A/B: GL back on the game thread) | 26.14 | 52.3 |

The render gate is the large step:

- In the heavy room, skipping the in-between frame brings a tick from 55 ms
  down to 33 ms. That is full game speed at 30 fps, where drawing both
  frames ran the game at about 60 % speed.
- With vsync and `ISAAC_RENDER_GATE=1`, the starting room keeps 60 fps: only
  53 of 627 in-between frames were skipped.
- The horde rooms switch to a steady 30 fps instead of slowing down.

The render thread is the second step: 29 % less per frame. With vsync and
`ISAAC_RENDER_GATE=1` (what `frame_skip=1` plays), the heavy room needs
24.9 ms per tick of the 33.3 ms it has, and the starting room keeps 60 fps
(16.67 ms per frame, 60 of 630 in-between frames skipped). On PC the queue
also helps a little (2.77 to 2.45 ms per frame in `horde:3b`).

The `const` base makes the lifted code 8 % smaller (44.0 to 40.5 MB of
text), because GCC no longer reloads the base after every guest store.
`-O3` stayed within the noise, so the lifted units keep `-O2`.

Tried and dropped:

- **Mesa's glthread.** It is compiled into the portlibs Mesa, but its queue
  thread is not created on libnx (the marshal table stays NULL). The failed
  start then left the process hung.
- **PGO.** The instrumented NRO (`--tag pgogen`, `tools/bench_native.py
  pgo`) runs on PC and writes its profiles. On the Switch the whole process
  froze while the game's TLS callback ran, before any profile was written,
  and even a watchdog thread on another core printed nothing.

### Not verified yet

- On the console: the intro timing with the wall clock, and screens where the
  tick may keep advancing outside a run (for example the death screen),
  where B would accept instead of going back.
- The vblank slots in real play with the pad (the numbers above are the
  bench mod's scenarios on the wall clock with vsync), and in menus, the
  intro cutscene and with the console docked.
- A guarded guest map on Horizon (today it is taken from the heap with no
  guard page above it).
- Copying saves from the Switch back to a PC.
