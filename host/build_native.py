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
LIFT_HOOKS = [
    ("lifted_000.c", "  RECOMP_VA(0x40ee80u);\n", "  isaac_hook_anm2_text(ESP);\n"),
]
HOOK_DECLS = "#include <stdint.h>\nvoid isaac_hook_anm2_text(uint32_t esp);\n"


def hooked_lift_sources(gu: Path, out: Path) -> dict:
    """Copies of the lifted files that carry hooks, rewritten only on change
    (a fresh mtime would recompile them every build)."""
    dst_dir = out / "lift-hooked"
    dst_dir.mkdir(parents=True, exist_ok=True)
    by_file = {}
    for name, anchor, call in LIFT_HOOKS:
        by_file.setdefault(name, []).append((anchor, call))
    result = {}
    for name, hooks in by_file.items():
        text = (gu / name).read_text()
        for anchor, call in hooks:
            if text.count(anchor) != 1:
                sys.exit("lift hook: %r occurs %d times in %s" % (anchor.strip(), text.count(anchor), name))
            text = text.replace(anchor, anchor + call)
        text = HOOK_DECLS + text
        dst = dst_dir / name
        if not dst.exists() or dst.read_text() != text:
            dst.write_text(text)
        result[name] = dst
    return result


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def prepare_sources(upstream: Path, src: Path) -> None:
    if src.exists():
        shutil.rmtree(src)
    rec = upstream / "scripts" / "recomp"
    (src / "scripts" / "recomp" / "lift").mkdir(parents=True)
    shutil.copytree(rec / "host" / "src", src / "scripts/recomp/host/src")
    shutil.copytree(rec / "host" / "include", src / "scripts/recomp/host/include")
    shutil.copytree(rec / "host" / "generated", src / "scripts/recomp/host/generated")
    for f in ("recomp_rt.c", "recomp_rt.h", "boot_integration.c"):
        shutil.copy2(rec / "lift" / f, src / "scripts/recomp/lift" / f)
    for patch in sorted((HERE / "patches").glob("*.patch")):
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
    conf.write_text(text.replace(old, "#define LUA_EXTRASPACE\t\t4"))
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
    ap.add_argument("--no-lifted", action="store_true",
                    help="skip recompiling lifted TUs whose objects are already up to date")
    args = ap.parse_args()

    switch = PROFILES[args.profile].get("switch", False)
    if switch and not os.environ.get("DEVKITPRO"):
        uid = "%d:%d" % (os.getuid(), os.getgid())
        cmd = ["docker", "run", "--rm", "--network", "none", "-u", uid,
               "-v", "%s:/work" % REPO, "-w", "/work", SDK_IMAGE,
               "python3", "host/build_native.py", "--profile", args.profile,
               "--jobs", str(args.jobs)]
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
    obj_dir = out / args.profile / "obj"
    obj_dir.mkdir(parents=True, exist_ok=True)

    prepare_sources(upstream, src)
    lua = prepare_lua(upstream, out)
    rec = src / "scripts" / "recomp"
    compat = HERE / "native" / "compat"
    inc = ["-I", str(compat), "-I", str(rec / "host" / "include"), "-I", str(rec / "host" / "generated"),
           "-I", str(rec / "lift"), "-I", str(gu), "-I", str(lua)]
    if switch:
        inc += ["-I", dkp + "/libnx/include", "-I", dkp + "/portlibs/switch/include"]

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
    host_flags = ["-std=gnu11", "-Wall", "-Wno-unused-parameter", "-Wno-unused-function"] + prof["host"] + COMMON + inc
    host_srcs = sorted((rec / "host" / "src").glob("*.c")) + sorted((rec / "host" / "generated").glob("*.c"))
    real_gl = prof.get("gl", False)
    skip = {"host_audio_web.c"} | (set() if real_gl else {"host_gl_webgl.c"})
    host_srcs = [p for p in host_srcs if p.name not in skip]
    for s in host_srcs + [rec / "lift" / "boot_integration.c", rec / "lift" / "recomp_rt.c"]:
        objs.append(add(s, host_flags, "host_"))
    for s in sorted((HERE / "native").glob("*.c")):
        if s.name in ("native_gl.c", "native_audio_al.c") and not real_gl:
            continue
        if s.name == "native_window.c" and (not real_gl or switch):
            continue
        objs.append(add(s, host_flags, "native_"))
    lift_flags = ["-std=gnu11", "-w"] + prof["lift"] + COMMON + inc
    hooked = hooked_lift_sources(gu, out)
    for s in [gu / "stubs.c", gu / "dispatch_tbl.c"] + sorted(gu.glob("lifted_*.c")):
        objs.append(add(hooked.get(s.name, s), lift_flags, "lift_"))

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
        exe = out / args.profile / "isaac-browsing.elf"
        r = sh([args.cc] + prof["ld"] + ["-specs=" + dkp + "/libnx/switch.specs",
               "-Wl,-Map=" + str(out / args.profile / "isaac-browsing.map"), "-o", str(exe)]
               + [str(o) for o in objs]
               + ["-L", dkp + "/portlibs/switch/lib", "-L", dkp + "/libnx/lib",
                  "-lopenal", "-lSDL2", "-lEGL", "-lGLESv2", "-lglapi", "-ldrm_nouveau",
                  "-lstdc++", "-lnx", "-lpthread", "-lm"])
    else:
        exe = out / args.profile / "isaac_native"
        r = sh([args.cc] + prof["ld"] + ["-rdynamic", "-o", str(exe)] + [str(o) for o in objs]
               + (["-lglfw", "-lEGL", "-lGLESv2", "-lopenal"] if real_gl else ["-lGL"]) + ["-lm"])
    if r.returncode:
        print(r.stderr[-6000:])
        return 1
    print("link: %s (%.1f MB) in %.1fs" % (exe, exe.stat().st_size / 1048576, time.time() - t0))
    if switch:
        romfs = out / args.profile / "romfs"
        if romfs.exists():
            shutil.rmtree(romfs)
        romfs.mkdir()
        shutil.copy2(segs, romfs / "isaac.segs.bin")
        index = upstream / ".scratch" / "game-dist" / "instance_index.json"
        if not index.exists():
            sys.exit("missing %s: run the upstream ship.py build first" % index)
        shutil.copy2(index, romfs / "instance_index.json")
        nacp = out / args.profile / "isaac-browsing.nacp"
        nro = out / args.profile / "isaac-browsing.nro"
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
    (out / args.profile / "build.json").write_text(json.dumps({
        "upstream": str(upstream), "segs": str(segs), "profile": args.profile,
        "cc": args.cc, "objects": len(objs)}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
