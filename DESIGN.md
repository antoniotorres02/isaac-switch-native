# Design and measurements

How the native host works, what was changed in the upstream pipeline and why,
and what was measured on PC and on the Switch. For building and playing, see
[BUILDING.md](BUILDING.md) and [PLAYING.md](PLAYING.md).

## Layout

```
host/
  build_native.py               build driver (profiles debug, asan, release, gl, switch)
  upstream.json                 pinned upstream commit, PE hashes and toolchain versions
  extra_code_pointer_starts.py  the 519 function entries the upstream lift misses
  import_pc_saves.py            Steam profile -> native save store
  patches/0001-native-guest-window.patch   changes to the upstream host sources
  native/
    native_main.c               entry, memory image, instance/RAM-FS seeding, saves, mods, clock
    native_mem.c                guest address window
    native_gl.c                 EGL context (pbuffer, GLFW window or Switch nwindow)
    native_audio_al.c           OpenAL Soft backend for the host's audio hooks
    native_input.c              scripted input, game tick probe, injected keys
    native_window.c             PC window and keyboard/gamepad (GLFW)
    native_switch.c             Switch entry: pad, settings.ini, test protocol, heap hygiene
    native_glyphs.c             Switch button prompts (in-memory .anm2 rewrite)
    native_lua_ramfs.c          Lua loads files through the RAM-FS
    compat/                     small newlib/glibc gaps
tools/
  run_switch_tests.py           send an NRO over nxlink and require a fresh receipt
  probe_switch_nxlink.py        nxlink UDP discovery
  switch_botbase.py             remote pad and screen captures through sys-botbase
```

The upstream checkout is kept outside Git at `build/browsing-port/repo`
(commit `22be498c60aad2f8abcf010cd408422385fb58a3`). `host/build_native.py`
copies its host sources into `build/browsing-native/src`, applies our patch
and compiles them together with the lifted translation units, Lua 5.3.3 and
`host/native/`. Nothing is written back into the upstream checkout.

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

### Changes to the upstream host (`host/patches/`)

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
  rewrites it).

Mod save data (`Isaac.SaveModData`) lives in `<game>/data/<metadata
directory>/` on PC. With Steam stubbed the engine uses the mod's folder name
instead, so the importer copies it to `data/<folder>/`.

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
- Mods: a real set of 17 Workshop mods (16 enabled, 12 with Lua; EID, Mod Config Menu,
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
| `e392f8a1d3e1464b93c1b336292a40b9` | `0fbff04c…` | `--mods` with real PC saves: the same run continued (seed `LXC1 DPH4`), no mod errors |
| `e5360778ee0e4087b0500568bf581361` | `fff3afac…` | heap fix: third start in a row in one hbloader process, 18 mods, `game_tick` 4213 |
| `f823a9cdff3b4d45bda13437aa839814` | `f81d951b…` | pad by sys-botbase: Switch prompts on the file select (BACK on B, SELECT! on A), A advances and B goes back, holding + about 3 s moved from a later room to a new starting room, the floor drawing shows Switch glyphs |
| `283cb712de304675bf53868ea2d545f1` | `f81d951b…` | a tap of + opens the pause screen and B closes it |

Earlier runs without a valid PASS, kept for the record: `390ffe59…` (the NRO
rejected the runner's `--graphics` argument), `fea346e7…` and `1f848d09…`
(`eglTerminate` did not return), `b8a98708…` and `6101e6be…` (the shared-heap
crash reproduced), `d9fd869e…` (closing nvdrv without releasing the context
hung), `71a84e15…`, `ea156560…` and `c21d61d8…` (no crash, but `game_tick`
read 0 because the tick probe was still bounded by the old guest limit
`0x1bf00000`).

A 39,951-frame play session on the console with real saves and mods followed; it ended normally with the heap fully released.

### Not verified yet

- On the console: the intro timing with the wall clock, and screens where the
  tick may keep advancing outside a run (for example the death screen),
  where B would accept instead of going back.
- Performance in heavy rooms on the Switch's A57.
- A guarded guest map on Horizon (today it is taken from the heap with no
  guard page above it).
- Copying saves from the Switch back to a PC.
