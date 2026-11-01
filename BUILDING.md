# Building the native host from your own copy of the game

This is the exact sequence that produced the PC executable and the Switch NRO
described in [DESIGN.md](DESIGN.md). Every artifact stays under the ignored
`build/` folder: never commit or share the executable, the lifted C, the
memory image, the game files or the NRO.

The commands are run from the repository root on x86-64 Linux. Shell
variables used throughout:

```sh
R=$PWD                                   # this repository
U=$R/build/browsing-port/repo            # upstream checkout
V=$R/build/browsing-port/venv            # its Python environment
GAME="$HOME/.local/share/Steam/steamapps/common/The Binding of Isaac Rebirth"
```

## 1. Requirements

- **The game**: your own purchased copy of The Binding of Isaac: Rebirth with
  Repentance+, version **v1.9.7.17**, and its executable in the form described
  in step 2. Every address used here is tied to that exact binary.
- Python 3.11+, Node.js 20+, Git, GCC, Docker.
- [Ghidra 12.1.2](https://github.com/NationalSecurityAgency/ghidra/releases)
  (`ghidra_12.1.2_PUBLIC_20260605.zip`, SHA256
  `b62e81a0390618466c019c60d8c2f796ced2509c4c1aea4a37644a77272cf99d`) and the
  JDK it requires (21).
- [emsdk](https://github.com/emscripten-core/emsdk) with Emscripten 6.0.5:
  upstream's pipeline builds its web module, and its outputs (patched lifted
  units, memory image, file index) are what the native build consumes.
- PC build libraries (Debian/Ubuntu names): `libglfw3-dev libegl-dev
  libgles-dev libopenal-dev`.
- Disk: the measured footprint was 2.0 GB for upstream's `output/`, 1.1 GB for
  its `.scratch/` (the game bundle) and 2.8 GB for `build/browsing-native/`
  (all profiles plus test runs).

The pinned versions are also recorded in [upstream.json](upstream.json).

## 2. The executable

The pipeline reads the game's executable as a plain Windows PE image, the same
input the-browsing-of-isaac expects (`tools/isaac-ng.unpacked.exe`). The image
this guide was measured with has SHA256
`b4743967f3d31050f7b4222e42ac6beae6a0b7850b1f1adaac07ef7b94d01ec8` and the
standard section names (`.text`, `.rdata`, `.data`, …). A different file will
not work.

**This project does not include, link to or describe any tool or method for
removing the game's copy protection, and it contains no game code or data.**
Whether and how you obtain that image from your own copy is your decision and
your responsibility under the law that applies to you; check it first.

```sh
sha256sum /path/to/isaac-ng.unpacked.exe   # must print b4743967f3d3…
```

## 3. Upstream checkout

```sh
git clone https://github.com/xreamm1/the-browsing-of-isaac "$U"
git -C "$U" checkout 22be498c60aad2f8abcf010cd408422385fb58a3
python3 -m venv "$V"
"$V/bin/pip" install pypcode==4.0.1 capstone pefile psutil brotli zopfli pillow
mkdir -p "$U/tools"
cp /path/to/isaac-ng.unpacked.exe "$U/tools/isaac-ng.unpacked.exe"
```

pypcode must be **4.0.1**, not the 3.3.3 upstream documents (see
[DESIGN.md](DESIGN.md#corrections-to-the-upstream-pipeline)).

Upstream's PE toolkit checks for the hash of their executable. Ours is
structurally the same image, so change the expected hash locally:

```sh
sed -i 's/^EXPECTED_SHA256 = "1FBA9A37BCEBF9276C9A80372E8C4DCAD59C8F652D2FD41727B396DCDCD6B89C"/EXPECTED_SHA256 = "B4743967F3D31050F7B4222E42AC6BEAE6A0B7850B1F1ADAAC07EF7B94D01EC8"/' \
  "$U/scripts/decomp/tools/pe.py"
```

Lua 5.3.3, extracted by hand (upstream's `lua_build.py` fails to extract it):

```sh
cd "$U/tools"
curl -sSLO https://www.lua.org/ftp/lua-5.3.3.tar.gz
echo "5113c06884f7de453ce57702abaac1d618307f33f6789fa870e87a59d772aca2  lua-5.3.3.tar.gz" | sha256sum -c
tar -xzf lua-5.3.3.tar.gz
cd "$R"
```

## 4. Upstream pipeline

All of this runs inside the upstream checkout with its virtual environment
and emsdk on the `PATH`:

```sh
cd "$U"
export PATH="$V/bin:$PATH" GHIDRA_HOME="$HOME/Tools/ghidra_12.1.2_PUBLIC"
source "$HOME/emsdk/emsdk_env.sh"
```

**Ghidra export** of every function. It took about 27 minutes here with 8
threads (analyze 11, recover 8, decompile 8). Do not pass `--max-functions`,
which also caps the inventory.

```sh
node scripts/recomp/export-all.mjs --stage all --threads 8 --timeout 240 --heap 6G
```

**Host tables, memory image, shims and Lua**:

```sh
for s in census/run_all.py host/boot_tables.py host/memimage.py \
         host/verify_memimage.py host/gen_shims.py "host/lua_build.py --build"; do
  python scripts/recomp/$s || break
done
python scripts/decomp/tools/build-pe-index.py
```

`gen_shims.py` regenerates `scripts/recomp/host/generated/shim_*` for our
executable, so `git status` in the upstream checkout afterwards shows those
files and `pe.py` as modified. That is expected.

**Function starts**, plus the 519 code pointers the lift would otherwise
miss (mod APIs need them):

```sh
python scripts/recomp/lift/prepare_starts.py
cp output/recomp/lift/starts_union.txt output/recomp/lift/starts_union.orig.txt
python "$R/host/extra_code_pointer_starts.py" \
  --exe tools/isaac-ng.unpacked.exe --starts output/recomp/lift/starts_union.orig.txt \
  > output/recomp/lift/extra_code_pointer_starts.txt
cat output/recomp/lift/starts_union.orig.txt output/recomp/lift/extra_code_pointer_starts.txt \
  | sort -u > output/recomp/lift/starts_union.txt
wc -l output/recomp/lift/extra_code_pointer_starts.txt   # 519
```

**Lift** (93 s here with 6 jobs; 38 units, 26,968 functions), keeping an
untouched copy:

```sh
python scripts/recomp/lift/lift_parallel.py --jobs 6 \
  --exe tools/isaac-ng.unpacked.exe \
  --ghidra-functions output/recomp/export/functions.jsonl \
  --fragments-tsv output/recomp/export/recovered-functions.tsv \
  --imports output/recomp/census/imports.json \
  --shim-table output/recomp/host/shim-table.json \
  --va-file output/recomp/lift/starts_union.txt \
  --hand-written scripts/recomp/host/src/missing_fns.c \
  --dispatch auto --max-insns 100000 --split-va 0x30000 --trace-va \
  --out output/recomp/lift/gu --module lifted \
  --stats output/recomp/lift/gu/stats.json
cp -a output/recomp/lift/gu output/recomp/lift/gu.pristine
```

**Patched units and web module**. `build_boot.py` rewrites `gu/` in place;
`ISAAC_CALL_SLIM=0` keeps the EIP spills that exception and `longjmp`
resumption need. If you ever run it again, restore `gu/` from
`gu.pristine/` first.

```sh
ISAAC_CALL_SLIM=0 python scripts/recomp/lift/build_boot.py --web --fast --jobs 4
```

This leaves the patched lifted units, `dispatch_tbl.c` and `stubs.c` in
`output/recomp/lift/gu` and the memory image in
`output/recomp/host/isaac.segs.bin`.

**Game bundle and file index**. The bundle hard-links the Steam files. The
native host only reads them, but editing a file inside the bundle would edit
your Steam install too; break the links into copies if you plan to touch it.

```sh
python scripts/recomp/assets/bundle.py build "$GAME" .scratch/game-bundle
python scripts/recomp/assets/ship.py build
cd "$R"
```

`ship.py` writes `.scratch/game-dist/instance_index.json`, the list of game
files the Switch build embeds.

## 5. Native PC build

```sh
python3 host/build_native.py --profile gl
```

Profiles:

| Profile | Use |
| --- | --- |
| `gl` | real GLES3 on EGL plus OpenAL Soft: the one to play with (`build/browsing-native/gl/isaac_native`) |
| `release` | no GPU (fake GL), for fast headless runs |
| `debug` | `-O1 -g` |
| `asan` | AddressSanitizer + UBSan |
| `switch` | the Nintendo Switch NRO (next section) |

The first build compiles the 38 lifted units of generated C and takes a
while; later builds only recompile what changed (`--no-lifted` skips the
lifted units entirely when their objects are up to date).

A quick headless check that boots to the starting room (build the `release`
profile first):

```sh
ISAAC_MAX_FRAMES=1500 build/browsing-native/release/isaac_native \
  "$U/output/recomp/host/isaac.segs.bin" "$U/.scratch/game-bundle" main
```

The log should reach `Room 1.2(Start Room)` and show the game tick
advancing. To play on PC, see [PLAYING.md](PLAYING.md#playing-on-pc).

## 6. Switch NRO

```sh
docker pull devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282
python3 host/build_native.py --profile switch
```

The script re-runs itself inside that pinned devkitA64 image, without
network, compiles everything for the Cortex-A57 and writes
`build/browsing-native/switch/isaac-browsing.nro` (title "Isaac (browsing
lift)", with `isaac.segs.bin` and `instance_index.json` in its RomFS). It
prints the NRO's SHA256 at the end. Installation is described in
[PLAYING.md](PLAYING.md#installing-on-the-switch).

To leave out everything that talks to Steam (Cloud sync, QR sign-in, the ownership
check and the network code behind them), add `--no-steam`. The NRO goes to
`build/browsing-native/switch-nosteam/`; see
[STEAM-CLOUD.md](STEAM-CLOUD.md#building-without-steam).

## 7. Testing on hardware (developers)

The runs in [DESIGN.md](DESIGN.md#measured-results) used the repository's
test tools over the network (Sphaira's nxlink in Application mode; `nxlink`
is devkitPro's client, kept here in `build/sdk-tools/`):

```sh
python3 tools/probe_switch_nxlink.py --address <switch-ip>
python3 tools/run_switch_tests.py --address <switch-ip> \
  --nro build/browsing-native/switch/isaac-browsing.nro --nxlink build/sdk-tools/nxlink \
  --graphics --audio --return-to-sphaira --timeout 600
```

`--nro-args="--mods --no-script --frames 6000"` passes extra arguments to the
NRO: mount the mods, leave the game to the pad (driven remotely with
sys-botbase through `tools/switch_botbase.py`) and run for 6,000 frames. Test
runs always use `sdmc:/switch/isaac-recomp/saves-autotest/`, never the
player's saves. Probe nxlink with the UDP discovery tool above before sending;
never probe it by opening a TCP connection and closing it without completing
the protocol, because Sphaira's receiver then waits for a transfer that never
comes.

Benchmarks use the same transport: `tools/bench_native.py switch <scenario>`
sends the NRO with `--bench` and reads its `BENCH` line (see
[DESIGN.md](DESIGN.md#performance)). `build_native.py --tag NAME` builds a
variant in `build/browsing-native/<profile>-NAME/`. `--lift-flags`,
`--host-flags` and `--ld-flags` add compiler and linker flags to it, and
`--patches-upto N` builds a reference with only the first N patches (use it
with `--out`).
