#!/usr/bin/env python3
"""Build a native (x86-64 Linux) executable from the browsing-of-isaac lift.

Inputs come from an upstream checkout that has already run its own pipeline
up to `build_boot.py` (so `output/recomp/lift/gu` holds the patched lifted
TUs, dispatch table and stubs, and `output/recomp/host/isaac.segs.bin` the
memory image). Nothing is written into that checkout.

    python host/build_native.py --profile debug

Steps:
  1. copy the upstream host sources into <out>/src and apply patches/*.patch
     (the native addressing mode and the few identity casts it replaces);
  2. build Lua 5.3.3 natively with the shipped DLL's ABI (LUA_EXTRASPACE 4);
  3. compile host, runtime, generated shims, lifted TUs and our driver;
  4. link <out>/<profile>/isaac_native.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_UPSTREAM = REPO / "build" / "browsing-port" / "repo"
DEFAULT_OUT = REPO / "build" / "browsing-native"

LUA_CORE = ("lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes "
            "lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib "
            "lbitlib lcorolib ldblib liolib lmathlib loslib lstrlib ltablib lutf8lib "
            "loadlib linit").split()

PROFILES = {
    "debug": {
        "host": ["-O1", "-g"],
        "lift": ["-O1", "-g1"],
        "ld": ["-g"],
    },
    "asan": {
        "host": ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"],
        "lift": ["-O1", "-g1", "-fsanitize=address", "-fno-omit-frame-pointer"],
        "ld": ["-g", "-fsanitize=address,undefined"],
    },
    "release": {
        "host": ["-O2", "-g1"],
        "lift": ["-O2"],
        "ld": [],
    },
    # Release with the real GLES3 backend on an EGL context (native_gl.c)
    # and the OpenAL Soft audio backend (native_audio_al.c).
    "gl": {
        "host": ["-O2", "-g1", "-DISAAC_NATIVE_GL=1"],
        "lift": ["-O2"],
        "ld": [],
        "gl": True,
    },
}

# What --no-steam leaves out. Nothing outside native_switch.c calls these, and
# it compiles those calls out with ISAAC_NO_STEAM.
STEAM_SOURCES = {"native_steamcm.c", "native_steamproto.c", "native_steamsync.c", "native_steamauth.c",
                 "native_qr.c", "native_sha1.c", "native_zip.c"}

# Nintendo Switch NRO: devkitA64/libnx inside the SDK image pinned below
# (run with --profile switch; the script re-executes itself in the
# container, without network).
SWITCH_ARCH = ["-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE"]
PROFILES["switch"] = {
    "host": ["-O2", "-g1", "-DISAAC_NATIVE_GL=1", "-D__SWITCH__", "-DAL_LIBTYPE_STATIC=1",
             "-ffunction-sections"] + SWITCH_ARCH,
    "lift": ["-O2", "-D__SWITCH__", "-ffunction-sections"] + SWITCH_ARCH,
    "ld": SWITCH_ARCH,
    "gl": True,
    "switch": True,
}
SDK_IMAGE = "devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282"

COMMON = ["-fno-strict-aliasing", "-fwrapv", "-DRECOMP_MEM_IDENTITY=0",
          "-DRECOMP_MEM_CHECK=0", "-DISAAC_NATIVE=1"]


# Host hooks inside the lifted code: (file, anchor, call). The anchor is the
# lifted instruction marker the call goes after; it must occur exactly once.
#   0x40ee80, in the .anm2 loader (sub_0040ed50): the XML text pointer for
#   rapidxml's parse (call at 0x40ee8a) has just been pushed, so [ESP] is it;
#   native_glyphs.c may hand the parser a rewritten copy.
#   0xa178d0, the engine's stream open (the upstream probe wrapper): the path
#   is at [ESP+4]; native_stream_path.c collapses a doubled '/' in it.
LIFT_HOOKS = [
    ("lifted_000.c", "  RECOMP_VA(0x40ee80u);\n", "  isaac_hook_anm2_text(ESP);\n"),
    ("lifted_032.c", "  if (on) isaac_probe_str(0xa178d0u, \"stream open\", MEMR32(s->ESP + 4u));\n",
     "  isaac_hook_stream_path(s->ESP);\n"),
]
HOOK_DECLS = ("#include <stdint.h>\nvoid isaac_hook_anm2_text(uint32_t esp);\n"
              "void isaac_hook_stream_path(uint32_t esp);\n")

# Replacements in the generated files: (file, old, new); old must occur once.
#   dispatch_tbl.c, the cache hit of every indirect call: the per-entry census
#   (a random increment into a 400 KB array, read only by the exit report)
#   and the hit counter leave the hot path. The call count stays: it is
#   recomp_run_pending's progress signal. The census still counts misses.
#   The call count, the cache and its enable flag lose `static` so host_trap.c
#   can probe the cache inline (patch 0012).
LIFT_EDITS = [
    ("dispatch_tbl.c", "  ++g_dcalls; ++g_dchits;\n  g_dcount[c->id[w]]++;\n  c->fn[w](cpu);\n",
     "  ++g_dcalls;\n  c->fn[w](cpu);\n"),
    ("dispatch_tbl.c", "static uint32_t g_dcalls, g_dblocks, g_dmisses;",
     "uint32_t g_dcalls;\nstatic uint32_t g_dblocks, g_dmisses;"),
    ("dispatch_tbl.c", "static dcache_set g_dcache[1u << DCACHE_BITS];", "dcache_set g_dcache[1u << DCACHE_BITS];"),
    ("dispatch_tbl.c", "static int g_dfast;", "int g_dfast;"),
]


def hooked_lift_sources(gu: Path, out: Path) -> dict:
    """Copies of the lifted files that carry hooks, rewritten only on change
    (a fresh mtime would recompile them every build)."""
    dst_dir = out / "lift-hooked"
    dst_dir.mkdir(parents=True, exist_ok=True)
    by_file = {}
    for name, anchor, call in LIFT_HOOKS:
        by_file.setdefault(name, []).append((anchor, anchor + call))
    for name, old, new in LIFT_EDITS:
        by_file.setdefault(name, []).append((old, new))
    hooked = {name for name, _, _ in LIFT_HOOKS}
    result = {}
    for name, edits in by_file.items():
        text = (gu / name).read_text()
        for old, new in edits:
            if text.count(old) != 1:
                sys.exit("lift edit: %r occurs %d times in %s" % (old.strip()[:60], text.count(old), name))
            text = text.replace(old, new)
        if name in hooked:
            text = HOOK_DECLS + text
        dst = dst_dir / name
        if not dst.exists() or dst.read_text() != text:
            dst.write_text(text)
        result[name] = dst
    return result


def optimized_lift_sources(paths, out: Path) -> list:
    """--lift-opt: lift_opt.transform(name, text) over each lifted TU, into
    <out>/lift-opt, rewritten only on change (see hooked_lift_sources)."""
    sys.path.insert(0, str(HERE))
    import lift_opt
    dst_dir = out / "lift-opt"
    dst_dir.mkdir(parents=True, exist_ok=True)
    result = []
    for p in paths:
        text = lift_opt.transform(p.name, p.read_text())
        dst = dst_dir / p.name
        if not dst.exists() or dst.read_text() != text:
            dst.write_text(text)
        result.append(dst)
    return result


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def prepare_sources(upstream: Path, src: Path, patches_upto: int = 9999, extra: Path | None = None) -> None:
    if src.exists():
        shutil.rmtree(src)
    rec = upstream / "scripts" / "recomp"
    (src / "scripts" / "recomp" / "lift").mkdir(parents=True)
    shutil.copytree(rec / "host" / "src", src / "scripts/recomp/host/src")
    shutil.copytree(rec / "host" / "include", src / "scripts/recomp/host/include")
    shutil.copytree(rec / "host" / "generated", src / "scripts/recomp/host/generated")
    for f in ("recomp_rt.c", "recomp_rt.h", "boot_integration.c"):
        shutil.copy2(rec / "lift" / f, src / "scripts/recomp/lift" / f)
    patches = [p for p in sorted((HERE / "patches").glob("*.patch")) if int(p.name[:4]) <= patches_upto]
    if extra:
        patches += sorted(extra.glob("*.patch"))
    for patch in patches:
        r = sh(["patch", "-p1", "--forward", "--batch", "-d", str(src), "-i", str(patch)])
        if r.returncode:
            sys.exit("patch %s failed:\n%s%s" % (patch.name, r.stdout, r.stderr))
        print("patch: %s" % patch.name)


def prepare_lua(upstream: Path, out: Path) -> Path:
    lua_src = upstream / "tools" / "lua-5.3.3" / "src"
    if not (lua_src / "lua.h").exists():
        sys.exit("missing %s (extract the verified lua-5.3.3 tarball there)" % lua_src)
    dst = out / "lua-5.3.3"
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(lua_src, dst)
    conf = dst / "luaconf.h"
    text = conf.read_text()
    old = "#define LUA_EXTRASPACE\t\t(sizeof(void *))"
    if old not in text:
        sys.exit("luaconf.h: LUA_EXTRASPACE definition not found")
    # The game was built against a 32-bit DLL: lua_getextraspace(L) is L-4.
    # Lua's own state hooks tell the host which state is the game's
    # (native/native_lua_state.c).
    hooks = ("\nstruct lua_State;\n"
             "void isaac_native_lua_opened(struct lua_State *L);\n"
             "void isaac_native_lua_closed(struct lua_State *L);\n"
             "#define luai_userstateopen(L)\tisaac_native_lua_opened(L)\n"
             "#define luai_userstateclose(L)\tisaac_native_lua_closed(L)\n")
    conf.write_text(text.replace(old, "#define LUA_EXTRASPACE\t\t4" + hooks))
    # luaL_loadfilex goes through the host RAM-FS first (native/native_lua_ramfs.c);
    # the original stays as the disk fallback.
    aux = dst / "lauxlib.c"
    text = aux.read_text()
    sig = "LUALIB_API int luaL_loadfilex (lua_State *L, const char *filename,"
    if text.count(sig) != 1:
        sys.exit("lauxlib.c: luaL_loadfilex definition not found")
    aux.write_text(text.replace(sig, "LUALIB_API int luaL_loadfilex_disk (lua_State *L, const char *filename,"))
    return dst


def compile_jobs(jobs, nproc):
    failed = []
    t0 = time.time()
    with cf.ThreadPoolExecutor(max_workers=nproc) as ex:
        futs = {ex.submit(sh, cmd): (src, obj) for src, obj, cmd in jobs}
        for fut in cf.as_completed(futs):
            src, obj = futs[fut]
            r = fut.result()
            if r.returncode:
                failed.append((src, r.stderr))
    return failed, time.time() - t0


def obj_is_fresh(src: Path, obj: Path, cmd, stamp_dir: Path):
    key = hashlib.sha256((" ".join(cmd)).encode()).hexdigest()[:16]
    stamp = stamp_dir / (obj.name + ".cmd")
    fresh = (obj.exists() and stamp.exists() and stamp.read_text() == key
             and obj.stat().st_mtime >= src.stat().st_mtime)
    return fresh, stamp, key


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--upstream", type=Path, default=DEFAULT_UPSTREAM)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--profile", choices=sorted(PROFILES), default="debug")
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    ap.add_argument("--tag", default="",
                    help="build a variant in <out>/<profile>-<tag> (objects and NRO apart from the default)")
    ap.add_argument("--lift-flags", default="", help="extra compiler flags for the lifted units")
    ap.add_argument("--host-flags", default="", help="extra compiler flags for the host units")
    ap.add_argument("--ld-flags", default="", help="extra linker flags")
    ap.add_argument("--patches-upto", type=int, default=9999,
                    help="apply only patches/NNNN-*.patch up to this number (a reference build; use --out)")
    ap.add_argument("--no-lifted", action="store_true",
                    help="skip recompiling lifted TUs whose objects are already up to date")
    ap.add_argument("--extra-patches", type=Path,
                    help="also apply this directory's *.patch after patches/ (work in progress; use with --out)")
    ap.add_argument("--no-steam", action="store_true",
                    help="leave out everything that talks to Steam: Cloud sync, sign-in, ownership check "
                         "and the network code behind them (variant <profile>-nosteam unless --tag says otherwise)")
    ap.add_argument("--lift-opt", action="store_true",
                    help="rewrite the lifted TUs with lift_opt.py before compiling them")
    args = ap.parse_args()

    switch = PROFILES[args.profile].get("switch", False)
    if switch and not os.environ.get("DEVKITPRO"):
        uid = "%d:%d" % (os.getuid(), os.getgid())
        lift_env = [a for k, v in os.environ.items() if k.startswith("ISAAC_LIFT_OPT_") for a in ("-e", k + "=" + v)]
        cmd = ["docker", "run", "--rm", "--network", "none", "-u", uid,
               "-v", "%s:/work" % REPO, "-w", "/work"] + lift_env + [SDK_IMAGE,
               "python3", "host/build_native.py", "--profile", args.profile,
               "--jobs", str(args.jobs), "--tag=" + args.tag, "--lift-flags=" + args.lift_flags,
               "--host-flags=" + args.host_flags, "--ld-flags=" + args.ld_flags]
        def in_work(p: Path) -> str:
            return "/work/" + str(p.resolve().relative_to(REPO))
        if args.out != DEFAULT_OUT:
            cmd += ["--out", in_work(args.out)]
        if args.extra_patches:
            cmd += ["--extra-patches", in_work(args.extra_patches)]
        if args.lift_opt:
            cmd += ["--lift-opt"]
        if args.no_steam:
            cmd += ["--no-steam"]
        return subprocess.run(cmd).returncode
    if switch:
        dkp = os.environ["DEVKITPRO"]
        args.cc = dkp + "/devkitA64/bin/aarch64-none-elf-gcc"

    upstream = args.upstream.resolve()
    gu = upstream / "output" / "recomp" / "lift" / "gu"
    segs = upstream / "output" / "recomp" / "host" / "isaac.segs.bin"
    for p in (gu / "dispatch_tbl.c", gu / "stubs.c", segs):
        if not p.exists():
            sys.exit("missing %s: run the upstream pipeline through build_boot.py first" % p)

    out = args.out.resolve()
    src = out / "src"
    prof = PROFILES[args.profile]
    if args.no_steam:
        args.tag = args.tag or "nosteam"
    variant = args.profile + ("-" + args.tag if args.tag else "")
    obj_dir = out / variant / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)

    prepare_sources(upstream, src, args.patches_upto, args.extra_patches.resolve() if args.extra_patches else None)
    lua = prepare_lua(upstream, out)
    rec = src / "scripts" / "recomp"
    compat = HERE / "native" / "compat"
    inc = ["-I", str(compat), "-I", str(HERE / "native"), "-I", str(rec / "host" / "include"), "-I", str(rec / "host" / "generated"),
           "-I", str(rec / "lift"), "-I", str(gu), "-I", str(lua)]
    if switch:
        inc += ["-I", dkp + "/libnx/include", "-I", dkp + "/portlibs/switch/include",
                "-I", dkp + "/portlibs/switch/include/freetype2"]

    jobs = []
    stamps = []
    # Header fingerprints: a changed header rebuilds its dependants. The
    # lifted TUs only see the lift-tree headers and recomp_rt.h.
    def fingerprint(paths):
        h = hashlib.sha256()
        for p in sorted(paths):
            h.update(p.name.encode())
            h.update(p.read_bytes())
        return h.hexdigest()[:16]
    host_hdrs = fingerprint(list((rec / "host" / "include").glob("*.h"))
                            + list((rec / "host" / "generated").glob("*.h"))
                            + list((rec / "lift").glob("*.h")) + list(compat.rglob("*.h"))
                            + list((HERE / "native").glob("*.h"))
                            + list(gu.glob("*.h")) + [lua / "luaconf.h"])
    lift_hdrs = fingerprint(list(gu.glob("*.h")) + [rec / "lift" / "recomp_rt.h"])

    def add(srcf: Path, flags, prefix: str):
        obj = obj_dir / (prefix + srcf.stem + ".o")
        cmd = [args.cc, "-c"] + flags + [str(srcf), "-o", str(obj)]
        cmd_key = cmd + ["#" + (lift_hdrs if prefix == "lift_" else host_hdrs)]
        fresh, stamp, key = obj_is_fresh(srcf, obj, cmd_key, obj_dir)
        if not fresh:
            jobs.append((srcf, obj, cmd))
            stamps.append((stamp, key))
        return obj

    objs = []
    # newlib has no popen/mkstemp: the Switch Lua is C89 for os/io.
    lua_flags = ["-std=gnu99", "-DLUA_USE_C89" if switch else "-DLUA_USE_POSIX", "-DLUA_USE_LONGJMP", "-w"] \
        + prof["host"] + COMMON
    for name in LUA_CORE:
        objs.append(add(lua / (name + ".c"), lua_flags, "lua_"))
    host_flags = ["-std=gnu11", "-Wall", "-Wno-unused-parameter", "-Wno-unused-function"] + prof["host"] + COMMON + inc \
        + args.host_flags.split() + (["-DISAAC_NO_STEAM=1"] if args.no_steam else [])
    host_srcs = sorted((rec / "host" / "src").glob("*.c")) + sorted((rec / "host" / "generated").glob("*.c"))
    real_gl = prof.get("gl", False)
    skip = {"host_audio_web.c"} | (set() if real_gl else {"host_gl_webgl.c"})
    host_srcs = [p for p in host_srcs if p.name not in skip]
    for s in host_srcs + [rec / "lift" / "boot_integration.c", rec / "lift" / "recomp_rt.c"]:
        objs.append(add(s, host_flags, "host_"))
    for s in sorted((HERE / "native").glob("*.c")):
        if s.name in ("native_gl.c", "native_glq.c", "native_audio_al.c") and not real_gl:
            continue
        if s.name == "native_window.c" and (not real_gl or switch):
            continue
        if args.no_steam and s.name in STEAM_SOURCES:
            continue
        objs.append(add(s, host_flags, "native_"))
    lift_flags = ["-std=gnu11", "-w"] + prof["lift"] + COMMON + inc + args.lift_flags.split()
    hooked = hooked_lift_sources(gu, out)
    lifted = [gu / "stubs.c", gu / "dispatch_tbl.c"] + sorted(gu.glob("lifted_*.c"))
    lifted = [hooked.get(s.name, s) for s in lifted]
    if args.lift_opt:
        lifted = optimized_lift_sources(lifted, out)
    for s in lifted:
        objs.append(add(s, lift_flags, "lift_"))

    print("compile: %d of %d objects out of date, %d jobs, %s" % (len(jobs), len(objs), args.jobs, args.cc))
    failed, secs = compile_jobs(jobs, args.jobs)
    failed_srcs = {srcf for srcf, _ in failed}
    for (srcf, _, _), (stamp, key) in zip(jobs, stamps):
        if srcf not in failed_srcs:
            stamp.write_text(key)
    if failed:
        for srcf, err in failed:
            print("FAILED %s" % srcf)
            for line in err.splitlines():
                if "error" in line:
                    print("   ", line)
        print("compile: %d failed in %.1fs" % (len(failed), secs))
        return 1
    print("compile: ok in %.1fs" % secs)

    t0 = time.time()
    if switch:
        exe = out / variant / "isaac-browsing.elf"
        r = sh([args.cc] + prof["ld"] + args.ld_flags.split() + ["-specs=" + dkp + "/libnx/switch.specs",
               "-Wl,-Map=" + str(out / variant / "isaac-browsing.map"), "-o", str(exe)]
               + [str(o) for o in objs]
               + ["-L", dkp + "/portlibs/switch/lib", "-L", dkp + "/libnx/lib",
                  "-lopenal", "-lSDL2", "-lEGL", "-lGLESv2", "-lglapi", "-ldrm_nouveau",
                  # the launch screen's text (native_launcher.c); FreeType and
                  # HarfBuzz need each other
                  "-Wl,--start-group", "-lfreetype", "-lharfbuzz", "-Wl,--end-group",
                  "-lpng16", "-lbz2", "-lstdc++", "-lnx", "-lpthread", "-lz", "-lm"])
    else:
        exe = out / variant / "isaac_native"
        r = sh([args.cc] + prof["ld"] + args.ld_flags.split() + ["-rdynamic", "-o", str(exe)] + [str(o) for o in objs]
               + (["-lglfw", "-lEGL", "-lGLESv2", "-lopenal"] if real_gl else ["-lGL"]) + ["-lz", "-lm"])
    if r.returncode:
        print(r.stderr[-6000:])
        return 1
    print("link: %s (%.1f MB) in %.1fs" % (exe, exe.stat().st_size / 1048576, time.time() - t0))
    if switch:
        romfs = out / variant / "romfs"
        if romfs.exists():
            shutil.rmtree(romfs)
        romfs.mkdir()
        shutil.copy2(segs, romfs / "isaac.segs.bin")
        index = upstream / ".scratch" / "game-dist" / "instance_index.json"
        if not index.exists():
            sys.exit("missing %s: run the upstream ship.py build first" % index)
        shutil.copy2(index, romfs / "instance_index.json")
        shutil.copytree(HERE / "bench" / "isaac-bench", romfs / "isaac-bench")   # --bench
        nacp = out / variant / "isaac-browsing.nacp"
        nro = out / variant / "isaac-browsing.nro"
        for cmd in ([dkp + "/tools/bin/nacptool", "--create", "Isaac (browsing lift)", "isaacport",
                     "0.1.0", str(nacp)],
                    [dkp + "/tools/bin/elf2nro", str(exe), str(nro), "--nacp=" + str(nacp),
                     "--romfsdir=" + str(romfs)]):
            r = sh(cmd)
            if r.returncode:
                print(r.stdout + r.stderr)
                return 1
        print("nro: %s (%.1f MB), sha256 %s" % (nro, nro.stat().st_size / 1048576,
                                                hashlib.sha256(nro.read_bytes()).hexdigest()))
    (out / variant / "build.json").write_text(json.dumps({
        "upstream": str(upstream), "segs": str(segs), "profile": args.profile,
        "cc": args.cc, "objects": len(objs)}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
