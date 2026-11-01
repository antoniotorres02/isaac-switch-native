#!/usr/bin/env python3
"""Benchmarks for the native host of the browsing-of-isaac lift.

Every scenario runs the autotest's scripted start and walk with the bench mod
(host/bench/isaac-bench) on the deterministic clock, so a
scenario is the same work on every build: only the time per frame changes.

  pc      run the PC build (profile gl by default) locally
  switch  send the Switch NRO through nxlink (the console must be in Sphaira
          with NXlink on, Application mode) and wait for its BENCH line;
          --profile adds the A57 sampler and resolves its samples
  report  print the results saved under build/bench/
  pgo     profile-guided optimization: train an instrumented NRO on the
          console (--tag pgogen build) and fetch its .gcda files by FTP into
          the obj directory of the -fprofile-use build (--tag pgouse)

Results: build/bench/<run>/result.json (and log, samples, profile.txt).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import uuid
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "build/browsing-port/repo"
BENCH_MOD = ROOT / "host/bench/isaac-bench"
NATIVE = ROOT / "build/browsing-native"
OUT = Path(os.environ.get("ISAAC_BENCH_OUT") or ROOT / "build/bench")
SCENARIOS = ["room", "horde", "swarm", "tears"]
SDK_IMAGE = "devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282"


def autotest_input(frames: int) -> str:
    """native_switch.c autotest_input: Enter until a run starts, then walk and shoot."""
    s = "".join("%d:enter," % f for f in range(120, 841, 40))
    f = 900
    while f + 160 <= frames:
        s += "%d:d:40,%d:w:40,%d:a:40,%d:s:40,%d:right:30,%d:up:30," % (f, f + 40, f + 80, f + 120, f, f + 80)
        f += 160
    return s


def parse_bench(text: str):
    for line in text.splitlines():
        i = line.find("BENCH {")
        if i >= 0:
            return json.loads(line[i + 6:])
    return None


def run_pc(args, scenario: str) -> dict:
    run = "pc-%s-%s-%s-%s" % (args.pc_profile, scenario.replace(":", "@"), time.strftime("%Y%m%d-%H%M%S"),
                              uuid.uuid4().hex[:4])
    d = OUT / run
    d.mkdir(parents=True)
    frames = args.to + 2
    env = dict(os.environ,
               ISAAC_SAVE_DIR=str(d / "saves"), ISAAC_MAX_FRAMES=str(frames),
               ISAAC_INPUT=autotest_input(frames), ISAAC_NATIVE_BENCH=scenario,
               ISAAC_NATIVE_BENCH_MOD=str(BENCH_MOD), ISAAC_NATIVE_BENCH_FROM=str(args.frm),
               ISAAC_NATIVE_BENCH_TO=str(args.to), ISAAC_NATIVE_AUDIO="0")
    if args.shot:
        env.update(ISAAC_NATIVE_SHOTS=str(args.shot), ISAAC_NATIVE_SHOT_DIR=str(d))
    exe = Path(args.exe).resolve() if args.exe else NATIVE / args.pc_profile / "isaac_native"
    cmd = [str(exe), str(UPSTREAM / "output/recomp/host/isaac.segs.bin"),
           str(UPSTREAM / ".scratch/game-bundle"), "main"]
    if args.perf:
        cmd = ["perf", "record", "-F", "2000", "-g", "-o", str(d / "perf.data"), "--"] + cmd
    t0 = time.monotonic()
    p = subprocess.run(cmd, env=env, capture_output=True, cwd=d)
    log = p.stdout + p.stderr
    (d / "run.log").write_bytes(log)
    text = log.decode("utf-8", "replace")
    result = {"run": run, "platform": "pc", "profile": args.pc_profile, "scenario": scenario,
              "rc": p.returncode, "wall_s": round(time.monotonic() - t0, 2),
              "bench": parse_bench(text), "lua": re.findall(r"\[bench\] [^\r\n]*", text)}
    (d / "result.json").write_text(json.dumps(result, indent=2))
    return result


def summary_line(r: dict) -> str:
    b = r.get("bench") or {}
    if not b:
        return "%-28s no BENCH line (rc %s)" % (r["run"], r.get("rc"))
    extra = ""
    if "cpu_mhz" in b:
        extra = "  cpu %s gpu %s emc %s MHz" % (b.get("cpu_mhz"), b.get("gpu_mhz"), b.get("emc_mhz"))
    return ("%-6s %-6s mean %7.2f ms  p50 %7.2f  p90 %7.2f  p99 %7.2f  max %7.2f  swap %6.2f  "
            "pair p90 %7.2f  >16.7 %5.1f%%  >33.3 %5.1f%%%s" % (
                r["platform"], b["scenario"].ljust(9), b["mean_ms"], b["p50_ms"], b["p90_ms"], b["p99_ms"],
                b["max_ms"], b["swap_mean_ms"], b["pair_p90_ms"], b["over_60fps_pct"],
                b["over_30fps_pct"], extra))


# ---- Switch -----------------------------------------------------------------

def run_switch(args, scenario: str) -> dict:
    run_id = uuid.uuid4().hex
    d = OUT / ("switch-%s-%s" % (scenario.replace(":", "@"), run_id[:8]))
    d.mkdir(parents=True)
    nro = Path(args.nro)
    nro_args = "--bench %s --bench-from %d --bench-to %d" % (scenario, args.frm, args.to)
    if args.profile:
        nro_args += " --profile-hz %d" % args.profile_hz
    if args.vsync:
        nro_args += " --vsync"
    if args.game_core is not None:
        nro_args += " --game-core %d" % args.game_core
    if args.extra:
        nro_args += " " + args.extra
    cmd = [sys.executable, "-B", str(ROOT / "tools/run_switch_tests.py"), "--address", args.address,
           "--nro", str(nro), "--nxlink", args.nxlink, "--timeout", str(args.timeout), "--return-to-sphaira",
           "--graphics", "--audio", "--output-dir", str(d), "--nro-args", nro_args]
    elf_symbols(Path(args.elf))                 # the symbols of exactly this build
    shutil.copy2(Path(args.elf).with_suffix(".syms"), d / "elf.syms")
    t0 = time.monotonic()
    # After a run the NRO hands the console back to Sphaira, whose receiver
    # takes a while to listen again: wait for its discovery reply, and send
    # again when the connection is refused anyway.
    out = ""
    for attempt in range(3):
        ready = wait_ready(args.address, 90)   # a receiver that already answered stays quiet: send anyway
        p = subprocess.run(cmd, capture_output=True, text=True)
        out += "# attempt %d, nxlink discovery %s\n" % (attempt + 1, ready) + p.stdout + p.stderr
        if "TRANSPORT_ERROR" not in p.stdout + p.stderr:
            break
        time.sleep(10)
    (d / "runner.out").write_text(out)
    logs = sorted(d.rglob("nxlink.log"))
    text = logs[-1].read_bytes().decode("utf-8", "replace") if logs else ""
    result = {"run": d.name, "platform": "switch", "scenario": scenario, "nro_args": nro_args,
              "runner_rc": p.returncode, "wall_s": round(time.monotonic() - t0, 2),
              "nro_sha256": sha256(nro), "bench": parse_bench(text),
              "lua": re.findall(r"\[bench\] [^\r\n]*", text),
              "receipt": [l for l in text.splitlines() if "SWITCH_TEST" in l][-1:]}
    if args.profile and text:
        prof = symbolize(text, d / "elf.syms", args.top)
        if prof:
            (d / "profile.txt").write_text(prof["text"])
            result["profile_top"] = prof["top"][:25]
            result["profile_samples"] = prof["samples"]
    (d / "result.json").write_text(json.dumps(result, indent=2))
    return result


def wait_ready(address: str, limit: float):
    sys.path.insert(0, str(ROOT / "tools"))
    from probe_switch_nxlink import probe
    t0 = time.monotonic()
    while time.monotonic() - t0 < limit:
        r = probe(address, attempts=2, timeout=0.5)
        if r.get("nxlink_ready"):
            return "ready after %.1f s" % (time.monotonic() - t0)
        time.sleep(3)
    return "no reply in %.0f s" % limit


def sha256(p: Path) -> str:
    import hashlib
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def elf_symbols(elf: Path):
    """(address, size, name) of the ELF's functions, sorted, via the SDK's nm.
    A .syms file is used as is (each run keeps the one of the ELF it sent)."""
    cache = elf if elf.suffix == ".syms" else elf.with_suffix(".syms")
    if elf.suffix != ".syms" and (not cache.exists() or cache.stat().st_mtime < elf.stat().st_mtime):
        rel = elf.resolve().relative_to(ROOT)
        cmd = ["docker", "run", "--rm", "--network", "none", "-u", "%d:%d" % (os.getuid(), os.getgid()),
               "-v", "%s:/work" % ROOT, "-w", "/work", SDK_IMAGE,
               "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm", "-n", "-S", "--defined-only", str(rel)]
        cache.write_text(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout)
    syms = []
    for line in cache.read_text().splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in "tTwW":
            syms.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
        elif len(parts) == 3 and parts[1] in "tTwW":
            syms.append((int(parts[0], 16), 0, parts[2]))
    syms.sort()
    return syms


CATEGORIES = [
    ("indirect calls", re.compile(r"^(isaac_lifted_dispatch|isaac_indirect_call|recomp_call_indirect|recomp_run_pending|dispatch_block)")),
    ("guest heap", re.compile(r"^(guest_malloc|guest_free|guest_realloc|guest_calloc|isaac_snd_lazy|isaac_fast_mutex|imp_.*heap)")),
    ("host fastpaths", re.compile(r"^isaac_fast_")),
    ("host GL backend", re.compile(r"^(isaac_gl|isaac_glc|imp_opengl32|glu_|uniform_range)")),
    ("host shims/runtime", re.compile(r"^(imp_|isaac_|recomp_|guest_|host_)")),
    ("Mesa / nouveau", re.compile(r"^(nvc0_|nv50_|nouveau_|st_|_mesa_|mesa_|u_|util_|pipe_|cso_|draw_|tgsi_|glsl|vbo_|_glapi|pushbuf|cli_|nv30_|_hw_select|_save_|gl[A-Z]|vl_|tc_|ralloc|_vbo_|_math_|_tnl_|_swrast_|egl|dri|loader_|hash_table|set_|bufferobj|ir_|nir_|lp_|sp_|fd_|nvk|translate_|rtasm_|slab_)")),
    ("libnx IPC / sync", re.compile(r"^(svc|binder|ipc|_ipc|serviceIpc|nvIoctl|nvFence|nv[A-Z]|mutex|rwlock|condvar|waitObj|waitSingle|__nx|nwindow|vi[A-Z]|bq|parcel|arm|thread|_sync)")),
    ("libc", re.compile(r"^(mem|str|_svfprintf|_vfprintf|__s|_malloc|_free|_realloc|malloc|free|realloc|calloc|__ssputs|_printf|sprintf|snprintf|vsnprintf|__mprec|_dtoa|__multiply|__pow5|__b2d|__d2b|qsort|bsearch|__aeabi|_strtod|__hexnan)")),
    ("lua", re.compile(r"^(lua|luaH_|luaV_|luaD_|luaC_|luaS_|luaF_|luaG_|luaT_|luaO_|luaK_|luaX_|luaY_|luaZ_|luaL_|propagatemark|sweeplist|reallymarkobject|traverse|singlestep|atomic|clearvalues|clearkeys|GCTM|index2addr|aux_|str_|math_|tab_)")),
    ("libm", re.compile(r"^(sin|cos|tan|atan|exp|log|pow|sqrt|floor|ceil|fmod|__ieee754|__kernel|__math|round|lrint|f[a-z]+f$)")),
]


def category(f: str) -> str:
    if f.startswith("sub_") or f.startswith("lifted_"):
        return "lifted game code"
    for name, rx in CATEGORIES:
        if rx.search(f):
            return name
    return "other"


def symbolize(text: str, elf: Path, top: int):
    """PROFBASE main=<runtime address>, then PROF <pc> <lr> <count> rows."""
    m = re.search(r"PROFBASE main=0x([0-9a-f]+)", text)
    rows = re.findall(r"^PROF ([0-9a-f]+) ([0-9a-f]+) (\d+)$", text, re.M)
    if not m or not rows:
        return None
    syms = elf_symbols(elf)
    main = next(a for a, _, n in syms if n == "main")
    slide = int(m.group(1), 16) - main
    import bisect
    addrs = [a for a, _, _ in syms]

    def name(pc: int) -> str:
        a = pc - slide
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0:
            return "?%x" % pc
        s = syms[i]
        if s[1] and a >= s[0] + s[1] and i + 1 < len(syms) and a >= syms[i + 1][0]:
            return "?%x" % pc
        if a - s[0] > 0x400000:
            return "?outside+%x" % a
        return s[2]

    self_c, caller_c = Counter(), Counter()
    total = 0
    for pc, lr, n in rows:
        n = int(n)
        total += n
        f = name(int(pc, 16))
        self_c[f] += n
        caller_c[(f, name(int(lr, 16)))] += n
    lines = ["%d samples" % total, "", "self   %   function"]
    topl = []
    for f, n in self_c.most_common(top):
        lines.append("%6d %5.1f %s" % (n, 100.0 * n / total, f))
        topl.append({"fn": f, "samples": n, "pct": round(100.0 * n / total, 2)})
    lines += ["", "self   %   function <- caller (lr)"]
    for (f, c), n in caller_c.most_common(top):
        lines.append("%6d %5.1f %s <- %s" % (n, 100.0 * n / total, f, c))
    groups = Counter()
    for f, n in self_c.items():
        groups[category(f)] += n
    lines += ["", "groups"] + ["%6d %5.1f %s" % (n, 100.0 * n / total, g) for g, n in groups.most_common()]
    return {"text": "\n".join(lines) + "\n", "top": topl, "samples": total,
            "groups": dict(groups)}


# The instrumented objects are /work/build/browsing-native/switch-pgogen/obj/X.o
# in the SDK container: libgcov writes X.gcda under GCOV_PREFIX once the five
# leading directories are stripped. Runs add to the files already there.
PGO_DIR = "/switch/isaac-recomp/pgo"
PGO_ENV = "--env GCOV_PREFIX=%s --env GCOV_PREFIX_STRIP=5" % PGO_DIR


def pgo_ftp(address: str, wait: float = 300):
    """Sphaira's FTP server; it is back only once the NRO has exited, so once
    it answers, the profiles of the last run are written."""
    import ftplib
    deadline = time.monotonic() + wait
    while True:
        try:
            f = ftplib.FTP()
            f.connect(address, 5000, timeout=60)
            f.login()
            return f
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(5)


def pgo(args) -> int:
    f = pgo_ftp(args.address)
    try:
        f.mkd(PGO_DIR)
    except Exception:
        pass
    if args.clear:
        for name in f.nlst(PGO_DIR):
            if name.endswith(".gcda"):
                f.delete(name if name.startswith("/") else PGO_DIR + "/" + name)
    f.quit()
    if not args.fetch_only:
        for s in args.scenarios:
            sa = argparse.Namespace(**{**vars(args), "nro": args.nro, "elf": str(Path(args.nro).with_suffix(".elf")),
                                       "frm": 1500, "to": 2700, "timeout": 1800.0,
                                       "nxlink": str(ROOT / "build/sdk-tools/nxlink"), "profile": False,
                                       "profile_hz": 0, "top": 0, "vsync": False, "game_core": None,
                                       "extra": PGO_ENV})
            r = run_switch(sa, s)
            print(summary_line(r), flush=True)
    dest = Path(args.dest)
    dest.mkdir(parents=True, exist_ok=True)
    f = pgo_ftp(args.address)
    names = [n for n in f.nlst(PGO_DIR) if n.endswith(".gcda")]
    total = 0
    for n in names:
        base = n.rsplit("/", 1)[-1]
        with open(dest / base, "wb") as out:
            f.retrbinary("RETR " + PGO_DIR + "/" + base, out.write)
        total += (dest / base).stat().st_size
    f.quit()
    print("pgo: %d profiles, %.1f MB -> %s" % (len(names), total / 1048576, dest))
    return 0 if names else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("pc", "switch"):
        s = sub.add_parser(name)
        s.add_argument("scenarios", nargs="*", default=["horde"],
                       help="load[:stage], load one of " + "|".join(SCENARIOS) + " (or all); stage as the "
                            "console's stage command (1c Downpour, 3b Flooded Caves, ...)")
        s.add_argument("--from", dest="frm", type=int, default=1500)
        s.add_argument("--to", type=int, default=2700)
        s.add_argument("--repeat", type=int, default=1)
    pc = sub.choices["pc"]
    pc.add_argument("--pc-profile", default="gl")
    pc.add_argument("--perf", action="store_true", help="record with perf (perf.data in the run dir)")
    pc.add_argument("--shot", type=int, help="save this presented frame as PPM")
    pc.add_argument("--exe", help="the isaac_native to run (default: build/browsing-native/<pc-profile>)")
    sw = sub.choices["switch"]
    sw.add_argument("--address", required=True)
    sw.add_argument("--nro", default=str(NATIVE / "switch/isaac-browsing.nro"))
    sw.add_argument("--elf", default=str(NATIVE / "switch/isaac-browsing.elf"))
    sw.add_argument("--timeout", type=float, default=600)
    sw.add_argument("--nxlink", default=str(ROOT / "build/sdk-tools/nxlink"))
    sw.add_argument("--profile", action="store_true", help="sample the game thread's PC on the console")
    sw.add_argument("--profile-hz", type=int, default=1000)
    sw.add_argument("--top", type=int, default=60)
    sw.add_argument("--vsync", action="store_true", help="keep vsync (the pacing players get)")
    sw.add_argument("--game-core", type=int, help="pin the game thread to this core (0-2)")
    sw.add_argument("--extra", default="", help="more NRO arguments")
    pp = sub.add_parser("profile", help="merge the A57 samples of several Switch runs")
    pp.add_argument("runs", nargs="+", help="run directories under build/bench (globs allowed)")
    pp.add_argument("--elf", default=str(NATIVE / "switch/isaac-browsing.elf"))
    pp.add_argument("--top", type=int, default=80)
    gp = sub.add_parser("pgo", help="train an instrumented NRO and fetch its profiles")
    gp.add_argument("scenarios", nargs="*", default=["horde:3b", "swarm:5c", "tears:1"])
    gp.add_argument("--address", required=True)
    gp.add_argument("--nro", default=str(NATIVE / "switch-pgogen/isaac-browsing.nro"))
    gp.add_argument("--dest", default=str(NATIVE / "switch-pgouse/obj"))
    gp.add_argument("--fetch-only", action="store_true", help="only fetch what earlier runs wrote")
    gp.add_argument("--clear", action="store_true", help="delete the console's profiles first")
    rp = sub.add_parser("report")
    rp.add_argument("filter", nargs="?", default="")
    args = ap.parse_args()

    if args.cmd == "profile":
        chunks = []
        for pat in args.runs:
            for d in sorted(OUT.glob(pat)) or [Path(pat)]:
                syms = d / "elf.syms"
                if not syms.exists():
                    print("skip %s: no elf.syms (run before symbols were kept)" % d.name)
                    continue
                for log in d.rglob("nxlink.log"):
                    t2 = log.read_bytes().decode("utf-8", "replace")
                    if "PROFBASE" in t2:
                        chunks.append((t2, syms))
        merged_self, merged_groups, total = Counter(), Counter(), 0
        for chunk, syms in chunks:
            prof = symbolize(chunk, syms, 10 ** 9)
            if not prof:
                continue
            total += prof["samples"]
            for row in prof["top"]:
                merged_self[row["fn"]] += row["samples"]
            merged_groups.update(prof["groups"])
        print("%d samples" % total)
        for g, n in merged_groups.most_common():
            print("%6d %5.1f %s" % (n, 100.0 * n / total, g))
        print()
        for f, n in merged_self.most_common(args.top):
            print("%6d %5.1f %-40s %s" % (n, 100.0 * n / total, f, category(f)))
        return 0
    if args.cmd == "pgo":
        return pgo(args)
    if args.cmd == "report":
        for f in sorted(OUT.glob("*/result.json"), key=lambda p: p.stat().st_mtime):
            r = json.loads(f.read_text())
            if args.filter in r["run"]:
                print("%-34s %s" % (r["run"], summary_line(r)))
        return 0
    scenarios = SCENARIOS if args.scenarios == ["all"] else args.scenarios
    for s in scenarios:
        if s.split(":")[0] not in SCENARIOS:
            ap.error("unknown scenario %s (load[:stage], load one of %s)" % (s, ", ".join(SCENARIOS)))
    rc = 0
    for _ in range(args.repeat):
        for s in scenarios:
            r = run_pc(args, s) if args.cmd == "pc" else run_switch(args, s)
            print(summary_line(r), flush=True)
            if r.get("profile_top"):
                print((OUT / r["run"] / "profile.txt").read_text()[:4000])
            if not r.get("bench"):
                rc = 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
