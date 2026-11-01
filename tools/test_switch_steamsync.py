#!/usr/bin/env python3
"""Host tests for the pieces the Switch Steam Cloud client is built from.

SHA-1, the ZIP wrapper and the protobuf/framing helpers are plain C with no
console dependencies, so they are checked here against Python's hashlib,
zipfile, zlib and gzip: the archives and digests Steam exchanges are the
oracle, not a copy of our own code.

    python3 tools/test_switch_steamsync.py
"""
import gzip
import base64
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import urllib.parse
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "host/native"
BUILD = ROOT / "build/agent-steamsync"

sys.path.insert(0, str(ROOT / "host"))
from steam_cloud_cm import pb, pb_decode, first, text  # noqa: E402  (the reference's own encoding)

HARNESS = r'''
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_sha1.h"
#include "native_zip.h"
#include "native_steamproto.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n ? (size_t)n : 1);
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int dump(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

/* Count of messages seen by the split callback, with their shape. */
static struct {
    unsigned count;
    unsigned emsg[16];
    size_t hlen[16], blen[16];
    unsigned char hdr[16][64];
    unsigned char body[16][64];
} seen;

static void on_msg(void *user, const isaac_cmmsg *m)
{
    (void)user;
    if (seen.count >= 16) return;
    unsigned i = seen.count++;
    seen.emsg[i] = m->emsg;
    seen.hlen[i] = m->hlen < 64 ? m->hlen : 64;
    seen.blen[i] = m->blen < 64 ? m->blen : 64;
    if (m->hdr) memcpy(seen.hdr[i], m->hdr, seen.hlen[i]);
    memcpy(seen.body[i], m->body, seen.blen[i]);
}

static int pb_checks(void)
{
    unsigned char buf[256];
    isaac_pbw w;
    isaac_pbw_init(&w, buf, sizeof buf);
    isaac_pbw_varint(&w, 1, 65580);
    isaac_pbw_str(&w, 6, "english");
    isaac_pbw_fixed64(&w, 22, 76561198000000003ull);
    isaac_pbw_bytes(&w, 30, "abc", 3);
    isaac_pbw_fixed32(&w, 40, 0xdeadbeefu);
    REQUIRE(!w.overflow);

    isaac_pbr r;
    isaac_pbfield f;
    isaac_pbr_init(&r, buf, w.len);
    REQUIRE(isaac_pbr_next(&r, &f) == 1 && f.field == 1 && f.value == 65580);
    REQUIRE(isaac_pbr_next(&r, &f) == 1 && f.field == 6 && f.len == 7 && !memcmp(f.bytes, "english", 7));
    REQUIRE(isaac_pbr_next(&r, &f) == 1 && f.field == 22 && f.value == 76561198000000003ull);
    REQUIRE(isaac_pbr_next(&r, &f) == 1 && f.field == 30 && f.len == 3 && !memcmp(f.bytes, "abc", 3));
    REQUIRE(isaac_pbr_next(&r, &f) == 1 && f.field == 40 && f.value == 0xdeadbeefu);
    REQUIRE(isaac_pbr_next(&r, &f) == 0);
    REQUIRE(isaac_pbr_uint(buf, w.len, 1, 0) == 65580);
    REQUIRE(isaac_pbr_uint(buf, w.len, 99, 7) == 7);
    char text[16];
    REQUIRE(isaac_pbr_str(buf, w.len, 6, text, sizeof text) == 7 && !strcmp(text, "english"));

    /* Truncated messages must be rejected, not read past the end. */
    isaac_pbr_init(&r, buf, w.len - 3);
    int rc, fields = 0;
    while ((rc = isaac_pbr_next(&r, &f)) == 1) ++fields;
    REQUIRE(rc == -1);

    /* Framing: [emsg|proto][hlen][hdr][body] with no body length. */
    unsigned char frame[128];
    size_t n = isaac_cm_frame(frame, sizeof frame, ISAAC_EMSG_CLIENT_HEARTBEAT, "\x08\x01", 2, "body", 4);
    REQUIRE(n == 8 + 2 + 4);
    REQUIRE(frame[0] == (unsigned char)(ISAAC_EMSG_CLIENT_HEARTBEAT & 0xff));
    REQUIRE(frame[3] == 0x80);
    REQUIRE(frame[4] == 2 && frame[8] == 0x08 && frame[10] == 'b');
    REQUIRE(isaac_cm_frame(frame, 8, ISAAC_EMSG_CLIENT_HEARTBEAT, NULL, 0, NULL, 0) == 8);
    REQUIRE(isaac_cm_frame(frame, 7, ISAAC_EMSG_CLIENT_HEARTBEAT, NULL, 0, NULL, 0) == 0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <cmd> ...\n", argv[0]); return 2; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "sha1")) {
        size_t len = 0;
        unsigned char *data = slurp(argv[2], &len);
        if (!data) return 1;
        unsigned char digest[ISAAC_SHA1_DIGEST];
        isaac_sha1_buf(data, len, digest);
        char hex[41];
        isaac_sha1_hex(digest, hex);
        printf("%s\n", hex);
        unsigned char back[ISAAC_SHA1_DIGEST];
        REQUIRE(isaac_sha1_from_hex(hex, back) == 0);
        REQUIRE(!memcmp(back, digest, sizeof digest));
        free(data);
        return 0;
    }
    if (!strcmp(cmd, "sha1-stream")) {
        /* Same digest when fed in awkward pieces. */
        size_t len = 0;
        unsigned char *data = slurp(argv[2], &len);
        if (!data) return 1;
        isaac_sha1 s;
        isaac_sha1_init(&s);
        for (size_t off = 0; off < len; off += 37) {
            size_t take = len - off < 37 ? len - off : 37;
            isaac_sha1_update(&s, data + off, take);
        }
        unsigned char digest[ISAAC_SHA1_DIGEST];
        isaac_sha1_final(&s, digest);
        char hex[41];
        isaac_sha1_hex(digest, hex);
        printf("%s\n", hex);
        free(data);
        return 0;
    }
    if (!strcmp(cmd, "zip-pack")) {
        size_t len = 0;
        unsigned char *data = slurp(argv[2], &len);
        if (!data) return 1;
        size_t cap = len * 2 + 1024;
        unsigned char *out = malloc(cap);
        size_t n = isaac_zip_pack(out, cap, data, len);
        REQUIRE(n > 0);
        REQUIRE(dump(argv[3], out, n) == 0);
        printf("%zu\n", n);
        free(out);
        free(data);
        return 0;
    }
    if (!strcmp(cmd, "zip-unpack")) {
        size_t len = 0, raw_cap = (size_t)strtoul(argv[4], NULL, 10);
        unsigned char *data = slurp(argv[2], &len);
        if (!data) return 1;
        unsigned char *out = malloc(raw_cap ? raw_cap : 1);
        size_t out_len = 0;
        int rc = isaac_zip_unpack(data, len, out, raw_cap, &out_len);
        if (rc != 0) { printf("FAIL\n"); free(out); free(data); return 0; }
        REQUIRE(dump(argv[3], out, out_len) == 0);
        printf("%zu\n", out_len);
        free(out);
        free(data);
        return 0;
    }
    if (!strcmp(cmd, "frame-split")) {
        size_t len = 0;
        unsigned char *data = slurp(argv[2], &len);
        if (!data) return 1;
        unsigned char *scratch = malloc(1 << 20);
        int rc = isaac_cm_split(data, len, scratch, 1 << 20, on_msg, NULL);
        REQUIRE(rc == 0);
        for (unsigned i = 0; i < seen.count; ++i) {
            printf("msg %u emsg %u hdr ", i, seen.emsg[i]);
            for (size_t k = 0; k < seen.hlen[i]; ++k) printf("%02x", seen.hdr[i][k]);
            printf(" body ");
            for (size_t k = 0; k < seen.blen[i]; ++k) printf("%02x", seen.body[i][k]);
            printf("\n");
        }
        free(scratch);
        free(data);
        return 0;
    }
    if (!strcmp(cmd, "zip-looks")) {
        size_t len = 0;
        unsigned char *data = slurp(argv[2], &len);
        if (!data) return 1;
        printf("%d\n", isaac_zip_looks_like_zip(data, len));
        free(data);
        return 0;
    }
    if (!strcmp(cmd, "pb-checks")) return pb_checks();
    fprintf(stderr, "unknown command %s\n", cmd);
    return 2;
}
'''


def build_harness() -> Path:
    BUILD.mkdir(parents=True, exist_ok=True)
    src = BUILD / "harness.c"
    src.write_text(HARNESS)
    exe = BUILD / "harness"
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-g", "-O1",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(NATIVE)]
    cmd = ["cc"] + flags + [str(src), str(NATIVE / "native_sha1.c"), str(NATIVE / "native_zip.c"),
                            str(NATIVE / "native_steamproto.c"), "-lz", "-o", str(exe)]
    subprocess.run(cmd, check=True)
    return exe


class Runner:
    def __init__(self, exe: Path):
        self.exe = exe
        self.failures = []

    def run(self, *args) -> str:
        r = subprocess.run([str(self.exe)] + [str(a) for a in args], capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError("harness %s failed (%d):\n%s\n%s"
                                 % (" ".join(map(str, args)), r.returncode, r.stdout, r.stderr))
        return r.stdout

    def check(self, label: str, ok: bool, detail: str = ""):
        print("%-52s %s %s" % (label, "PASS" if ok else "FAIL", detail))
        if not ok:
            self.failures.append(label)


def main() -> int:
    exe = build_harness()
    run = Runner(exe)
    work = BUILD / "data"
    work.mkdir(parents=True, exist_ok=True)

    # --- SHA-1: FIPS vectors and Python's hashlib on random data ------------
    vectors = {b"": "da39a3ee5e6b4b0d3255bfef95601890afd80709",
               b"abc": "a9993e364706816aba3e25717850c26c9cd0d89d",
               b"a" * 1000000: "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
               bytes(range(256)) * 3: None}
    for data, want in vectors.items():
        path = work / "sha1.bin"
        path.write_bytes(data)
        got = run.run("sha1", path).strip()
        want = want or hashlib.sha1(data).hexdigest()
        run.check("sha1 len=%d" % len(data), got == want, got[:12])
        if len(data) < 200000:
            streamed = run.run("sha1-stream", path).strip()
            run.check("sha1 streamed len=%d" % len(data), streamed == want, streamed[:12])

    # --- ZIP: our pack read by Python, Python's zip read by us --------------
    for label, data in (("small", b"hello steam cloud\n"),
                        ("save-like", bytes((i * 37) & 0xff for i in range(60000))),
                        ("incompressible", os.urandom(4096))):
        raw = work / ("raw-%s.bin" % label)
        raw.write_bytes(data)
        zpath = work / ("ours-%s.zip" % label)
        size = int(run.run("zip-pack", raw, zpath).strip())
        with zipfile.ZipFile(zpath) as z:
            names = z.namelist()
            inner = z.read(names[0])
            bad = z.testzip()
        run.check("zip pack %s: python reads it" % label,
                  names == ["z"] and inner == data and bad is None,
                  "%d B zip, %d B raw" % (size, len(data)))

        theirs = work / ("theirs-%s.zip" % label)
        with zipfile.ZipFile(theirs, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("z", data)
        out = work / ("out-%s.bin" % label)
        got = run.run("zip-unpack", theirs, out, len(data)).strip()
        run.check("zip unpack %s: we read python's" % label,
                  got == str(len(data)) and out.read_bytes() == data, got)

    # stored (method 0) archives must work too: that is what we emit when
    # deflate would grow the file.
    tiny = work / "tiny.bin"
    tiny.write_bytes(b"x")
    stored = work / "stored.zip"
    with zipfile.ZipFile(stored, "w", zipfile.ZIP_STORED) as z:
        z.writestr("z", b"stored payload")
    got = run.run("zip-unpack", stored, work / "stored.out", 64).strip()
    run.check("zip unpack stored entry", got == "14" and (work / "stored.out").read_bytes() == b"stored payload", got)

    # corrupt archives must be rejected, never trusted: cutting into the deflate
    # stream and flipping a payload byte both have to fail (inflate or CRC).
    good = (work / "theirs-save-like.zip").read_bytes()
    truncated = work / "broken.zip"
    truncated.write_bytes(good[:60])
    out = run.run("zip-unpack", truncated, work / "broken.out", 60000).strip()
    run.check("zip rejects a truncated deflate stream", out == "FAIL", out)
    flipped = bytearray(good)
    flipped[40] ^= 0xff
    corrupt = work / "corrupt.zip"
    corrupt.write_bytes(bytes(flipped))
    out = run.run("zip-unpack", corrupt, work / "corrupt.out", 60000).strip()
    run.check("zip rejects a corrupted payload (CRC)", out == "FAIL", out)
    out = run.run("zip-unpack", work / "theirs-save-like.zip", work / "short.out", 100).strip()
    run.check("zip refuses a buffer that is too small", out == "FAIL", out)

    # a cloud file is a ZIP only when Steam compressed it; a save is not
    raw_save = work / "raw-save.dat"
    raw_save.write_bytes(b"ISAACNGSAVE09R  " + bytes(200))
    run.check("zip-looks: a save is not a ZIP", run.run("zip-looks", raw_save).strip() == "0")
    run.check("zip-looks: our own archive is",
              run.run("zip-looks", work / "ours-save-like.zip").strip() == "1")
    (work / "empty.bin").write_bytes(b"")
    run.check("zip-looks: an empty file is not", run.run("zip-looks", work / "empty.bin").strip() == "0")

    # --- framing: one frame, then a Multi, then a gzipped Multi -------------
    def frame(emsg: int, hdr: bytes, body: bytes) -> bytes:
        return struct.pack("<II", emsg | 0x80000000, len(hdr)) + hdr + body

    plain = work / "frame-plain.bin"
    plain.write_bytes(frame(751, b"\x08\x01", b"\x08\x0f"))
    out = run.run("frame-split", plain).strip().splitlines()
    run.check("frame: single protobuf message",
              len(out) == 1 and "emsg 751" in out[0] and "body 080f" in out[0], out and out[0])

    sub1, sub2 = frame(5501, b"", b"\x0a\x00"), frame(751, b"\x08\x01", b"\x08\x01\x50\x09")
    multi_body = b"\x12" + bytes([len(sub1) + 4 + len(sub2) + 4]) + \
                 struct.pack("<I", len(sub1)) + sub1 + struct.pack("<I", len(sub2)) + sub2
    multi = work / "frame-multi.bin"
    multi.write_bytes(frame(1, b"", multi_body))
    out = run.run("frame-split", multi).strip().splitlines()
    run.check("frame: plain CMsgMulti", len(out) == 2 and "emsg 5501" in out[0] and "emsg 751" in out[1],
              " | ".join(out))

    blob = struct.pack("<I", len(sub1)) + sub1 + struct.pack("<I", len(sub2)) + sub2
    packed = gzip.compress(blob)
    gz_body = b"\x08" + bytes([len(blob)]) + b"\x12" + bytes([len(packed)]) + packed
    gzm = work / "frame-multi-gz.bin"
    gzm.write_bytes(frame(1, b"", gz_body))
    out = run.run("frame-split", gzm).strip().splitlines()
    run.check("frame: gzipped CMsgMulti", len(out) == 2 and "emsg 5501" in out[0] and "emsg 751" in out[1],
              " | ".join(out))

    # a struct message (no protobuf flag) carries a 20-byte header
    structmsg = work / "frame-struct.bin"
    structmsg.write_bytes(struct.pack("<I", 768) + b"\x00" * 16 + b"payload")
    out = run.run("frame-split", structmsg).strip().splitlines()
    run.check("frame: struct message", len(out) == 1 and "emsg 768" in out[0] and "body 7061796c6f6164" in out[0],
              out and out[0])

    run.check("protobuf field roundtrip + frame builder", run.run("pb-checks").strip() == "")

    sync_engine_tests(run)
    token_file_tests(run)
    qr_tests(run)
    sign_in_tests(run)
    forget_tests(run)
    game_files_tests(run)

    if run.failures:
        print("\n%d failure(s)" % len(run.failures))
        return 1
    print("\nall steamsync host tests passed")
    return 0




# --- the sync engine, driven with a fake Steam that is just a folder --------

SYNC_HARNESS = r'''
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <errno.h>

#include "native_sha1.h"
#include "native_steamsync.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static const char *g_cloud_dir;
static const char *g_fail_name;
static int g_prompt_answer = -1;      /* -1: no prompt installed */
static const char *g_unowned;         /* --unowned=: app ids Steam says no to */
static int g_owns_fail_at = -1;       /* --owns-fail-at=: the question that gets no answer */
static int g_owns_asked;

static int fake_owns(void *state, uint32_t appid, char *err, size_t errcap)
{
    (void)state;
    printf("OWNS? %u\n", (unsigned)appid);
    if (g_owns_asked++ == g_owns_fail_at) {
        snprintf(err, errcap, "no answer to the ownership request");
        return -1;
    }
    char want[16];
    snprintf(want, sizeof want, "%u", (unsigned)appid);
    return !(g_unowned && strstr(g_unowned, want));
}

static void on_owned(void *user, int status, const char *missing)
{
    (void)user;
    printf("OWNED %d [%s]\n", status, missing);
}

static void on_log(void *user, const char *line)
{
    (void)user;
    printf("%s\n", line);
}

/* native_steamsync.c is built with -Drename=strict_rename: Horizon's file
 * system does not replace an existing file on rename, and neither does this. */
#undef rename
extern int rename(const char *from, const char *to);
int strict_rename(const char *from, const char *to)
{
    struct stat st;
    if (stat(to, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    return rename(from, to);
}

static void on_slot(void *user, int slot, int status)
{
    (void)user;
    printf("SLOT %d %d\n", slot, status);
}

static int on_ask(void *user, const char *slot, const char *local_desc, const char *cloud_desc)
{
    (void)user;
    printf("ASK %s local=%s cloud=%s\n", slot, local_desc, cloud_desc);
    return g_prompt_answer;
}

static int cloud_path(char *out, size_t cap, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", g_cloud_dir, name);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int fake_enumerate(void *state, uint32_t appid, isaac_cloud_file *out, size_t max,
                          size_t *count, char *err, size_t errcap)
{
    (void)state; (void)appid; (void)err; (void)errcap;
    size_t n = 0;
    DIR *d = opendir(g_cloud_dir);
    if (!d) { *count = 0; return 0; }
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        char path[512];
        if (cloud_path(path, sizeof path, e->d_name) != 0) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        static unsigned char buf[1 << 20];
        size_t len = fread(buf, 1, sizeof buf, f);
        fclose(f);
        unsigned char digest[ISAAC_SHA1_DIGEST];
        isaac_sha1_buf(buf, len, digest);
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
        isaac_sha1_hex(digest, out[n].sha_hex);
        out[n].timestamp = (uint64_t)st.st_mtime;
        out[n].size = (uint32_t)len;
        ++n;
    }
    closedir(d);
    *count = n;
    return 0;
}

static int fake_download(void *state, uint32_t appid, const char *name, void *raw, size_t cap,
                         size_t *raw_len, char sha_hex[41], uint64_t *timestamp,
                         char *err, size_t errcap)
{
    (void)state; (void)appid;
    if (g_fail_name && !strcmp(g_fail_name, name)) {
        snprintf(err, errcap, "injected download failure");
        return -1;
    }
    char path[512];
    if (cloud_path(path, sizeof path, name) != 0) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errcap, "missing %s", name); return -1; }
    size_t len = fread(raw, 1, cap, f);
    fclose(f);
    if (len == cap && cap) { snprintf(err, errcap, "too large"); return -1; }
    unsigned char digest[ISAAC_SHA1_DIGEST];
    isaac_sha1_buf(raw, len, digest);
    isaac_sha1_hex(digest, sha_hex);
    struct stat st;
    if (stat(path, &st) == 0 && timestamp) *timestamp = (uint64_t)st.st_mtime;
    *raw_len = len;
    return 0;
}

static int fake_upload(void *state, uint32_t appid, const char *name, const void *raw, size_t raw_len,
                       uint64_t *timestamp, char *err, size_t errcap)
{
    (void)state; (void)appid;
    char path[512];
    if (cloud_path(path, sizeof path, name) != 0) { snprintf(err, errcap, "path"); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(err, errcap, "cannot write %s", name); return -1; }
    if (fwrite(raw, 1, raw_len, f) != raw_len) { fclose(f); snprintf(err, errcap, "short write"); return -1; }
    fclose(f);
    struct stat st;
    if (stat(path, &st) == 0 && timestamp) *timestamp = (uint64_t)st.st_mtime;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--read-token")) {
        uint64_t steamid = 0;
        char token[1024];
        char err[160] = "";
        if (isaac_steamsync_read_token(argv[2], &steamid, token, sizeof token, err, sizeof err) != 0) {
            printf("FAIL %s\n", err);
            return 0;
        }
        printf("steamid %llu token_len %zu\n", (unsigned long long)steamid, strlen(token));
        return 0;
    }
    if (argc == 5 && !strcmp(argv[1], "--write-token")) {
        char err[160] = "";
        int rc = isaac_steamsync_write_token(argv[2], strtoull(argv[3], NULL, 10), argv[4], err, sizeof err);
        printf("%s\n", rc == 0 ? "OK" : err);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--forget")) {
        isaac_steamsync_forget(argv[2]);
        return 0;
    }
    if (argc < 5) {
        fprintf(stderr, "usage: %s <save_dir> <cloud_dir> <now> pull|push [--prompt=local|cloud] [--fail-download=NAME]\n", argv[0]);
        return 2;
    }
    const char *save_dir = argv[1];
    g_cloud_dir = argv[2];
    int64_t now = strtoll(argv[3], NULL, 10);
    int push = !strcmp(argv[4], "push");
    int check_owned = 0;
    for (int i = 5; i < argc; ++i) {
        if (!strncmp(argv[i], "--prompt=", 9)) g_prompt_answer = !strcmp(argv[i] + 9, "cloud");
        else if (!strncmp(argv[i], "--fail-download=", 16)) g_fail_name = argv[i] + 16;
        else if (!strncmp(argv[i], "--unowned=", 10)) { g_unowned = argv[i] + 10; check_owned = 1; }
        else if (!strncmp(argv[i], "--owns-fail-at=", 15)) { g_owns_fail_at = atoi(argv[i] + 15); check_owned = 1; }
        else if (!strcmp(argv[i], "--check-owned")) check_owned = 1;
    }

    isaac_steam_transport t;
    memset(&t, 0, sizeof t);
    t.enumerate = fake_enumerate;
    t.download = fake_download;
    t.upload = fake_upload;
    if (check_owned) t.owns = fake_owns;
    isaac_steamsync_set_own_fn(on_owned, NULL);

    isaac_steamsync_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.save_dir = save_dir;
    cfg.prompt = g_prompt_answer >= 0;
    isaac_steamsync_set_log(on_log, NULL);
    isaac_steamsync_set_slot_fn(on_slot, NULL);
    if (cfg.prompt) isaac_steamsync_set_ask(on_ask, NULL);

    int rc = push ? isaac_steamsync_push_with(&cfg, &t, now)
                  : isaac_steamsync_pull_with(&cfg, &t, now);
    printf("RC %d\n", rc);
    return 0;
}
'''


def build_sync_harness() -> Path:
    BUILD.mkdir(parents=True, exist_ok=True)
    src = BUILD / "sync_harness.c"
    src.write_text(SYNC_HARNESS)
    exe = BUILD / "sync_harness"
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-g", "-O1",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(NATIVE)]
    engine = BUILD / "native_steamsync.o"
    subprocess.run(["cc", "-c"] + flags + ["-Drename=strict_rename", str(NATIVE / "native_steamsync.c"),
                                           "-o", str(engine)], check=True)
    subprocess.run(["cc"] + flags + [str(src), str(engine),
                                     str(NATIVE / "native_sha1.c"), str(NATIVE / "native_steamcm.c"),
                                     "-o", str(exe)], check=True)
    return exe


DOCS = "documents/my games/binding of isaac repentance+"
SLOT_NAMES = [("rep+persistentgamedata%d.dat" % n, "persistentgamedata%d.dat" % n) for n in (1, 2, 3)] + \
             [("rep+gamestate%d.dat" % n, "gamestate%d.dat" % n) for n in (1, 2, 3)]


# ISAAC_SYNC_SLOT_* in native_steamsync.h
EMPTY, SAME, DOWNLOADED, UPLOAD, LOCAL_ONLY, FAILED = range(6)


def slots(out: str) -> dict:
    """What the pull reported for each slot."""
    found = {}
    for line in out.splitlines():
        if line.startswith("SLOT "):
            _, slot, status = line.split()
            found[int(slot)] = int(status)
    return found


class SyncFixture:
    def __init__(self, exe: Path, root: Path, runner: Runner):
        self.exe = exe
        self.root = root
        self.run = runner
        shutil.rmtree(root, ignore_errors=True)     # each fixture starts empty
        self.saves = root / "saves"
        self.cloud = root / "cloud"
        self.docs = self.saves / DOCS
        self.docs.mkdir(parents=True, exist_ok=True)
        self.cloud.mkdir(parents=True, exist_ok=True)

    def local(self, slot: int) -> Path:
        return self.docs / SLOT_NAMES[slot][1]

    def cloud_file(self, slot: int) -> Path:
        return self.cloud / SLOT_NAMES[slot][0]

    def put(self, path: Path, data: bytes, mtime: int):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        os.utime(path, (mtime, mtime))

    def run_sync(self, command: str, now: int, *args) -> tuple[str, int]:
        r = subprocess.run([str(self.exe), str(self.saves), str(self.cloud), str(now), command] + list(args),
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError("sync harness failed:\n%s\n%s" % (r.stdout, r.stderr))
        rc = 0
        for line in r.stdout.splitlines():
            if line.startswith("RC "):
                rc = int(line[3:])
        return r.stdout, rc

    def state(self) -> str:
        p = self.saves / "steam-sync-state.txt"
        return p.read_text() if p.exists() else ""

    def backups(self) -> list[Path]:
        base = self.saves / "save_backups"
        return sorted(p for p in base.glob("steam-*/*")) if base.exists() else []


def sync_engine_tests(run: Runner) -> None:
    exe = build_sync_harness()
    t0 = 1_760_000_000

    # 1. first pull fills an empty console from the cloud, with a marker
    f = SyncFixture(exe, BUILD / "sync1", run)
    for slot in range(6):
        f.put(f.cloud_file(slot), b"cloud-%d" % slot, t0)
    out, rc = f.run_sync("pull", t0 + 10)
    run.check("pull: first run downloads all six",
              rc == 0 and all(f.local(s).read_bytes() == b"cloud-%d" % s for s in range(6)),
              "%d file(s)" % len(list(f.docs.glob("*.dat"))))
    run.check("pull: marker written", (f.saves / "steam-sync-pull.txt").exists())
    run.check("pull: reports six downloads", slots(out) == {s: DOWNLOADED for s in range(6)}, str(slots(out)))

    # 2. a second pull with nothing changed must not touch anything
    before = {p.name: p.stat().st_mtime for p in f.docs.glob("*.dat")}
    out, rc = f.run_sync("pull", t0 + 20)
    after = {p.name: p.stat().st_mtime for p in f.docs.glob("*.dat")}
    run.check("pull: no-op when both sides match", rc == 0 and before == after and "downloaded" not in out)
    run.check("pull: reports six in sync", slots(out) == {s: SAME for s in range(6)}, str(slots(out)))

    # 3. the game writes a save: pull does nothing, push uploads exactly that one
    f.put(f.local(0), b"played on switch", t0 + 30)
    out, rc = f.run_sync("pull", t0 + 40)
    run.check("pull: local change is left for push", rc == 0 and f.local(0).read_bytes() == b"played on switch")
    run.check("pull: reports the local change as an upload", slots(out).get(0) == UPLOAD, str(slots(out)))
    out, rc = f.run_sync("push", t0 + 50)
    uploads = [l for l in out.splitlines() if "uploaded" in l and "push done" not in l]
    run.check("push: uploads the changed slot only",
              rc == 0 and f.cloud_file(0).read_bytes() == b"played on switch"
              and f.cloud_file(1).read_bytes() == b"cloud-1" and len(uploads) == 1,
              "%d upload(s)" % len(uploads))
    run.check("push: marker cleared", not (f.saves / "steam-sync-pull.txt").exists())

    # 4. push without a pull must refuse (the console cannot compare)
    f.put(f.local(2), b"uncompared change", t0 + 60)
    out, rc = f.run_sync("push", t0 + 70)
    run.check("push: refuses without a pull this session",
              rc != 0 and f.cloud_file(2).read_bytes() == b"cloud-2" and "not uploading" in out, out.strip().splitlines()[-2])

    # 5. the cloud moved (played on PC): pull downloads it and backs up the old copy
    f.put(f.cloud_file(1), b"played on pc", t0 + 100)
    out, rc = f.run_sync("pull", t0 + 110)
    backups = f.backups()
    run.check("pull: cloud change is downloaded", rc == 0 and f.local(1).read_bytes() == b"played on pc")
    run.check("pull: the replaced copy is backed up",
              any(b.read_bytes() == b"cloud-1" for b in backups), "%d backup(s)" % len(backups))

    # 6. both sides moved: newest wins without a prompt (cloud newer here)
    f.put(f.local(3), b"switch newer", t0 + 300)
    f.put(f.cloud_file(3), b"cloud older", t0 + 200)
    out, rc = f.run_sync("pull", t0 + 400)
    run.check("pull: conflict keeps the newest copy",
              rc == 0 and f.local(3).read_bytes() == b"switch newer" and "conflict" in out
              and "keeping the console copy" in out)

    # 7. and the console copy is then pushed even if the game did not touch it
    out, rc = f.run_sync("push", t0 + 410)
    run.check("push: a conflict won by the console is uploaded",
              rc == 0 and f.cloud_file(3).read_bytes() == b"switch newer")

    # 8. the prompt decides, and its descriptions carry both dates
    f.put(f.local(4), b"switch copy", t0 + 500)
    f.put(f.cloud_file(4), b"cloud copy", t0 + 600)
    out, rc = f.run_sync("pull", t0 + 700, "--prompt=cloud")
    run.check("pull: prompt shows both dates and the cloud answer wins",
              rc == 0 and f.local(4).read_bytes() == b"cloud copy" and "ASK gamestate2.dat local=" in out
              and "cloud=" in out, out.strip().splitlines()[-3:] if out else "no output")
    f.put(f.local(5), b"switch copy 2", t0 + 500)
    f.put(f.cloud_file(5), b"cloud copy 2", t0 + 600)
    out, rc = f.run_sync("pull", t0 + 700, "--prompt=local")
    run.check("pull: prompt answer 'local' keeps the console copy",
              rc == 0 and f.local(5).read_bytes() == b"switch copy 2")

    # 9. a failed download leaves the save alone (and the backup already exists)
    f = SyncFixture(exe, BUILD / "sync2", run)
    f.put(f.local(0), b"precious", t0)
    f.put(f.cloud_file(0), b"newer cloud", t0 + 100)
    out, rc = f.run_sync("pull", t0 + 200, "--fail-download=" + SLOT_NAMES[0][0])
    run.check("pull: failed download keeps the save",
              f.local(0).read_bytes() == b"precious" and "download failed" in out
              and any(b.read_bytes() == b"precious" for b in f.backups()))
    run.check("pull: reports the failed download", slots(out).get(0) == FAILED, str(slots(out)))
    run.check("pull: a failed download fails the pull", rc != 0 and "pull incomplete" in out)
    f.put(f.local(1), b"other slot", t0 + 210)
    out, rc = f.run_sync("push", t0 + 220)
    run.check("push: leaves the slot whose download failed alone",
              f.cloud_file(0).read_bytes() == b"newer cloud" and f.cloud_file(1).read_bytes() == b"other slot",
              out.strip().splitlines()[-1] if out else "no output")
    out, rc = f.run_sync("pull", t0 + 300)
    run.check("pull: retries the failed download next time",
              rc == 0 and f.local(0).read_bytes() == b"newer cloud", out.strip().splitlines()[-1] if out else "")

    # 10. a save that only exists on the console is uploaded, not duplicated
    f = SyncFixture(exe, BUILD / "sync3", run)
    f.put(f.local(2), b"only on switch", t0)
    out, rc = f.run_sync("pull", t0 + 10)
    run.check("pull: console-only save is noted", rc == 0 and "only on the console" in out)
    run.check("pull: reports the console-only save and five empty slots",
              slots(out) == {s: UPLOAD if s == 2 else EMPTY for s in range(6)}, str(slots(out)))
    out, rc = f.run_sync("push", t0 + 20)
    run.check("push: console-only save reaches Steam", rc == 0 and f.cloud_file(2).read_bytes() == b"only on switch")

    # 11. ownership: what Steam says decides the sync, and only the game itself stops it
    f = SyncFixture(exe, BUILD / "sync-own", run)
    f.put(f.local(0), b"console save", t0)
    f.put(f.cloud_file(1), b"cloud save", t0)
    out, rc = f.run_sync("pull", t0 + 10, "--check-owned")
    run.check("owned: asks about the game and the four expansions, the game first",
              [l for l in out.splitlines() if l.startswith("OWNS?")] ==
              ["OWNS? %d" % a for a in (250900, 401920, 570660, 1426300, 3353470)], out)
    run.check("owned: everything owned syncs as usual", rc == 0 and "OWNED 1 []" in out and f.local(1).exists())

    g = SyncFixture(exe, BUILD / "sync-own-none", run)
    g.put(g.local(0), b"console save", t0)
    g.put(g.cloud_file(0), b"another save", t0)
    out, rc = g.run_sync("pull", t0 + 10, "--unowned=250900")
    run.check("not owned: the pull says so", rc == -2 and "OWNED 3 [The Binding of Isaac: Rebirth]" in out, out)
    run.check("not owned: stops at the game, no more questions", out.count("OWNS?") == 1, out)
    run.check("not owned: both sides untouched",
              g.local(0).read_bytes() == b"console save" and g.cloud_file(0).read_bytes() == b"another save")
    run.check("not owned: leaves no pull marker or state", not g.state() and not (g.saves / "steam-sync-pull.txt").exists())
    out, rc = g.run_sync("push", t0 + 20)
    run.check("not owned: a push afterwards uploads nothing",
              rc == -1 and g.cloud_file(0).read_bytes() == b"another save", out)

    g = SyncFixture(exe, BUILD / "sync-own-part", run)
    g.put(g.cloud_file(1), b"cloud save", t0)
    out, rc = g.run_sync("pull", t0 + 10, "--unowned=1426300,3353470")
    run.check("expansions missing: names them", "OWNED 2 [Repentance, Repentance+]" in out, out)
    run.check("expansions missing: the sync goes on", rc == 0 and g.local(1).read_bytes() == b"cloud save")

    for at, what in ((0, "the game"), (3, "an expansion")):
        g = SyncFixture(exe, BUILD / ("sync-own-unknown%d" % at), run)
        g.put(g.cloud_file(1), b"cloud save", t0)
        out, rc = g.run_sync("pull", t0 + 10, "--owns-fail-at=%d" % at)
        run.check("no answer about %s: not verified, and no more questions" % what,
                  "OWNED 0 []" in out and out.count("OWNS?") == at + 1, out)
        run.check("no answer about %s: the sync goes on" % what, rc == 0 and g.local(1).exists())

    g = SyncFixture(exe, BUILD / "sync-own-off", run)
    g.put(g.cloud_file(1), b"cloud save", t0)
    out, rc = g.run_sync("pull", t0 + 10)
    run.check("a transport without the check syncs and says nothing", rc == 0 and "OWNED" not in out and "OWNS?" not in out)


GAMEFILES_HARNESS = r'''
#include <stdio.h>
#include "native_gamefiles.h"
int main(int argc, char **argv) {
    isaac_gamefiles f;
    int rc = isaac_gamefiles_check(argv[1], argv[2], &f);
    printf("rc=%d total=%d missing=%d truncated=%d different=%d blocking=%d", rc, f.total, f.missing, f.truncated,
           f.different, isaac_gamefiles_blocking(&f));
    for (int i = 0; i < ISAAC_GAMEFILES_NAMES; ++i) if (f.bad[i][0]) printf(" bad=%s", f.bad[i]);
    printf("\n");
    return 0;
}
'''


def game_files_tests(run: Runner) -> None:
    src = BUILD / "gamefiles_harness.c"
    src.write_text(GAMEFILES_HARNESS)
    exe = BUILD / "gamefiles_harness"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-g", "-O1", "-fsanitize=address,undefined",
                    "-I", str(NATIVE), str(src), str(NATIVE / "native_gamefiles.c"), "-o", str(exe)], check=True)
    root = BUILD / "gamefiles"
    shutil.rmtree(root, ignore_errors=True)
    guest = root / "guest"
    big = 2_000_000
    files = {"resources/packed/a.a": 100, "resources/packed/big.a": big, "resources/scripts/socket/c.lua": 30,
             "savedatapath.txt": 289}
    index = root / "index.json"
    guest.mkdir(parents=True)
    index.write_text(json.dumps([{"p": p, "s": s} for p, s in files.items()], separators=(",", ":")))

    def put(name: str, size: int) -> None:
        (guest / name).parent.mkdir(parents=True, exist_ok=True)
        with open(guest / name, "wb") as f:
            f.truncate(size)

    def check() -> str:
        return subprocess.run([str(exe), str(index), str(guest)], capture_output=True, text=True, check=True).stdout.strip()

    clean = "rc=0 total=4 missing=0 truncated=0 different=0 blocking=0"
    run.check("files: an empty folder misses all of them, naming three",
              check() == "rc=0 total=4 missing=4 truncated=0 different=0 blocking=1"
                          " bad=resources/packed/a.a bad=resources/packed/big.a bad=resources/scripts/socket/c.lua", check())
    for name, size in files.items():
        put(name, size)
    run.check("files: a complete copy passes", check() == clean, check())
    # the two differences of a card that plays fine, measured 2026-11-01
    put("savedatapath.txt", 195)
    put("resources/packed/big.a", big + 467)
    run.check("files: a patched archive and a shorter text file do not block",
              check() == "rc=0 total=4 missing=0 truncated=0 different=2 blocking=0", check())
    put("savedatapath.txt", 289)
    put("resources/packed/big.a", big - big // 100)
    run.check("files: an archive 1 % short still passes",
              check() == "rc=0 total=4 missing=0 truncated=0 different=1 blocking=0", check())
    put("resources/packed/big.a", big - big // 100 - 1)
    run.check("files: over 1 % gone is a copy cut short",
              check() == "rc=0 total=4 missing=0 truncated=1 different=0 blocking=1 bad=resources/packed/big.a", check())
    put("resources/packed/big.a", big)
    put("resources/packed/a.a", 40)
    run.check("files: a small file of another size is not judged",
              check() == "rc=0 total=4 missing=0 truncated=0 different=1 blocking=0", check())
    put("resources/packed/a.a", 0)
    run.check("files: an empty file is cut short whatever its size",
              check() == "rc=0 total=4 missing=0 truncated=1 different=0 blocking=1 bad=resources/packed/a.a", check())
    (guest / "resources/packed/a.a").unlink()
    (guest / "resources/packed/a.a").mkdir()
    run.check("files: a folder where a file belongs is missing",
              check() == "rc=0 total=4 missing=1 truncated=0 different=0 blocking=1 bad=resources/packed/a.a", check())
    (guest / "resources/packed/a.a").rmdir()
    put("resources/packed/a.a", 100)
    put("extra.a", 1)
    run.check("files: files the build does not list do not matter", check() == clean, check())
    index.write_text("[]")
    run.check("files: an empty list checks nothing", check().startswith("rc=-1 "), check())
    index.unlink()
    run.check("files: no list checks nothing", check().startswith("rc=-1 "), check())


AUTH_HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native_qr.h"
#include "native_steamauth.h"

/* Steam, as files: the reply and X-eresult of call N, and where its request goes. */
static const char *g_dir;
static int g_call;

static int fake_webapi(void *user, const char *iface, const char *method, const void *req, size_t req_len,
                       void *resp, size_t cap, size_t *resp_len, int *eresult, char *err, size_t errcap)
{
    (void)user;
    char path[512];
    snprintf(path, sizeof path, "%s/req-%d-%s-%s.bin", g_dir, g_call, iface, method);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(req, 1, req_len, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/reply-%d.bin", g_dir, g_call);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errcap, "no network");
        ++g_call;
        return -1;
    }
    *resp_len = fread(resp, 1, cap, f);
    fclose(f);
    snprintf(path, sizeof path, "%s/eresult-%d.txt", g_dir, g_call);
    f = fopen(path, "r");
    *eresult = 1;
    if (f) {
        if (fscanf(f, "%d", eresult) != 1) return -1;
        fclose(f);
    }
    ++g_call;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "qr")) {
        static uint8_t m[ISAAC_QR_MAX_SIZE * ISAAC_QR_MAX_SIZE];
        int size = isaac_qr_encode(argv[3], strlen(argv[3]), atoi(argv[2]), m);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) putchar(m[y * size + x] ? '#' : ' ');
            putchar('\n');
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "steamid")) {
        printf("%llu\n", (unsigned long long)isaac_steamauth_token_steamid(argv[2]));
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "form")) {
        FILE *f = fopen(argv[2], "rb");
        unsigned char in[512];
        char out[4096];
        size_t n = fread(in, 1, sizeof in, f);
        fclose(f);
        size_t len = isaac_steamauth_form(in, n, out, sizeof out);
        printf("%zu %s %zu\n", len, out, isaac_steamauth_form(in, n, out, len));
        return 0;
    }
    if (argc != 4 || strcmp(argv[1], "flow")) return 2;
    g_dir = argv[2];
    char err[160] = "";
    static isaac_steamauth_session s;
    static isaac_steamauth_result r;
    if (isaac_steamauth_begin(fake_webapi, NULL, "Isaac on Switch (NRO)", &s, err, sizeof err) != 0) {
        printf("begin failed: %s\n", err);
        return 0;
    }
    printf("begin client=%llu url=%s interval=%d rid=%zu\n", (unsigned long long)s.client_id, s.url,
           s.interval_ms, s.request_id_len);
    for (int i = 0; i < atoi(argv[3]); ++i) {
        err[0] = 0;
        int state = isaac_steamauth_poll(fake_webapi, NULL, &s, &r, err, sizeof err);
        printf("poll state=%d client=%llu url=%s", state, (unsigned long long)s.client_id, s.url);
        if (state == ISAAC_STEAMAUTH_APPROVED)
            printf(" steamid=%llu account=%s token=%s", (unsigned long long)r.steamid, r.account, r.token);
        if (state == ISAAC_STEAMAUTH_ERROR) printf(" err=%s", err);
        printf("\n");
    }
    return 0;
}
'''

STEAMID = "76561198000000001"


def fake_token(sub: str = STEAMID, padding: int = 0) -> str:
    def part(obj: dict) -> str:
        return base64.urlsafe_b64encode(json.dumps(obj).encode()).decode().rstrip("=")
    return "%s.%s.%s" % (part({"typ": "JWT"}), part({"iss": "steam", "sub": sub, "pad": "x" * padding}), "c2ln")


def build_auth_harness() -> Path:
    BUILD.mkdir(parents=True, exist_ok=True)
    src = BUILD / "auth_harness.c"
    src.write_text(AUTH_HARNESS)
    exe = BUILD / "auth_harness"
    flags = ["-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-g", "-O1", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", "-I", str(NATIVE)]
    subprocess.run(["cc"] + flags + [str(src)] + [str(NATIVE / n) for n in
                   ("native_qr.c", "native_steamauth.c", "native_steamproto.c")] + ["-lz", "-o", str(exe)], check=True)
    return exe


def qr_tests(run: Runner) -> None:
    """Every length, against qrencode's matrix under the same mask."""
    if not shutil.which("qrencode"):
        run.check("qr: qrencode is installed (the oracle)", False)
        return
    exe = build_auth_harness()

    def ours(mask: int, data: str) -> list:
        return subprocess.run([str(exe), "qr", str(mask), data], capture_output=True, text=True,
                              check=True).stdout.splitlines()

    def theirs(data: str) -> list:
        out = subprocess.run(["qrencode", "-8", "-l", "M", "-m", "0", "-t", "ASCII", data], capture_output=True,
                             text=True, check=True).stdout
        return [row[::2] for row in out.splitlines()]

    bad = []
    used = 0
    for n in list(range(1, 214)):
        data = ("https://s.team/q/1/" + "0123456789abcdefXYZ-_" * 11)[:n]
        want = theirs(data)
        if not any(ours(m, data) == want for m in range(8)):
            bad.append(n)
        used = max(used, len(want))
    run.check("qr: lengths 1-213 match qrencode, module by module", not bad and used == 57, "bad %s" % bad[:8])
    for n in (1, 38, 62, 213):
        data = ("https://s.team/q/1/" + "0123456789" * 30)[:n]
        chosen = ours(-1, data)
        run.check("qr: the mask picked for %d bytes is one of the eight" % n,
                  any(chosen == ours(m, data) for m in range(8)))
    run.check("qr: 214 bytes do not fit", ours(-1, "x" * 214) == [])


def sign_in_tests(run: Runner) -> None:
    exe = build_auth_harness()
    work = BUILD / "auth"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    def script(*replies) -> None:
        for old in work.iterdir():
            old.unlink()
        for i, (eresult, body) in enumerate(replies):
            if body is None:
                continue
            (work / ("reply-%d.bin" % i)).write_bytes(body)
            (work / ("eresult-%d.txt" % i)).write_text(str(eresult))

    def flow(polls: int) -> list:
        r = subprocess.run([str(exe), "flow", str(work), str(polls)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError("auth harness failed:\n%s\n%s" % (r.stdout, r.stderr))
        return r.stdout.splitlines()

    def request(call: int, method: str) -> dict:
        return pb_decode((work / ("req-%d-IAuthenticationService-%s.bin" % (call, method))).read_bytes())

    begin = pb(1, 111) + pb(2, "https://s.team/q/1/111") + pb(3, b"RID-0123456789ab") + \
        pb(4, 0x40a00000, "f32") + pb(6, 1)
    token = fake_token()
    script((1, begin), (1, b""), (1, pb(1, 222) + pb(2, "https://s.team/q/1/222")), (1, b""),
           (1, pb(3, token) + pb(4, "access") + pb(6, "someone")), (27, b""))
    out = flow(5)
    run.check("sign-in: begin reads client, URL, request id and interval",
              out[0] == "begin client=111 url=https://s.team/q/1/111 interval=5000 rid=16", out[0])
    req = request(0, "BeginAuthSessionViaQR")
    details = pb_decode(first(req, 3))
    run.check("sign-in: begin asks for a Steam client token, named",
              text(req, 1) == "Isaac on Switch (NRO)" and first(req, 2) == 1 and
              text(details, 1) == "Isaac on Switch (NRO)" and first(details, 2) == 1, str(req))
    req = request(1, "PollAuthSessionStatus")
    run.check("sign-in: poll sends the session's ids",
              first(req, 1) == 111 and first(req, 2) == b"RID-0123456789ab", str(req))
    run.check("sign-in: an empty reply is still pending", out[1].startswith("poll state=0 client=111"), out[1])
    run.check("sign-in: a new code replaces the old one",
              out[2] == "poll state=1 client=222 url=https://s.team/q/1/222", out[2])
    req = request(3, "PollAuthSessionStatus")
    run.check("sign-in: later polls ask for the new client id", first(req, 1) == 222, str(req))
    run.check("sign-in: approval gives steamid, account and token",
              out[4] == "poll state=3 client=222 url=https://s.team/q/1/222 steamid=%s account=someone token=%s"
              % (STEAMID, token), out[4])
    run.check("sign-in: eresult 27 ends the session", out[5].startswith("poll state=2"), out[5])

    script((1, begin), (2, b""), (1, None), (1, pb(3, fake_token(padding=1200))), (1, pb(3, "not-a-jwt")))
    out = flow(4)
    run.check("sign-in: another eresult is an error", "state=-1" in out[1] and "eresult 2" in out[1], out[1])
    run.check("sign-in: no network is an error", "state=-1" in out[2] and "no network" in out[2], out[2])
    run.check("sign-in: a token too long for the file is refused", "state=-1" in out[3] and "too long" in out[3], out[3])
    run.check("sign-in: a token without an account is refused", "state=-1" in out[4] and "no account" in out[4], out[4])

    script((2, begin))
    run.check("sign-in: a refused begin is reported", "eresult 2" in flow(0)[0])
    script((1, pb(1, 111)))
    run.check("sign-in: an incomplete begin is reported", "incomplete" in flow(0)[0])

    for sub, want in ((STEAMID, STEAMID), ("", "0")):
        got = subprocess.run([str(exe), "steamid", fake_token(sub)], capture_output=True, text=True).stdout.strip()
        run.check("sign-in: subject %r" % sub, got == want, got)

    for raw in (b"\xfb\xef\xff", b"\xfb\xef", b"\xff", bytes(range(200))):
        path = work / "form.bin"
        path.write_bytes(raw)
        length, form, small = subprocess.run([str(exe), "form", str(path)], capture_output=True,
                                             text=True).stdout.split()
        decoded = urllib.parse.parse_qs(form, strict_parsing=True)["input_protobuf_encoded"][0]
        run.check("sign-in: the form decodes back (%d bytes)" % len(raw),
                  base64.b64decode(decoded) == raw and int(length) == len(form) and small == "0", form[:40])


def forget_tests(run: Runner) -> None:
    exe = build_sync_harness()
    t0 = 1760000000
    f = SyncFixture(exe, BUILD / "forget", run)
    f.put(f.local(0), b"same", t0)
    f.put(f.cloud_file(0), b"same", t0)
    f.run_sync("pull", t0 + 10)
    f.put(f.cloud_file(0), b"another account's", t0 + 20)
    subprocess.run([str(exe), "--forget", str(f.saves)], check=True)
    run.check("forget: the sync's notes are gone", f.state() == "" and
              not (f.saves / "steam-sync-pull.txt").exists() and f.local(0).read_bytes() == b"same")
    out, rc = f.run_sync("pull", t0 + 30, "--prompt=local")
    run.check("forget: the next pull asks instead of replacing the save",
              rc == 0 and f.local(0).read_bytes() == b"same", out[-200:])
    token_path = BUILD / "forget" / "steam-token.txt"
    r = subprocess.run([str(exe), "--write-token", str(token_path), STEAMID, "abc.def.ghi"],
                       capture_output=True, text=True)
    r2 = subprocess.run([str(exe), "--write-token", str(token_path), "76561198000000002", "new.token.here"],
                        capture_output=True, text=True)
    read = subprocess.run([str(exe), "--read-token", str(token_path)], capture_output=True, text=True).stdout.strip()
    run.check("token file: written, replaced and read back",
              r.stdout.strip() == "OK" and r2.stdout.strip() == "OK" and
              read == "steamid 76561198000000002 token_len %d" % len("new.token.here"), read)


def token_file_tests(run: Runner) -> None:
    exe = build_sync_harness()
    work = BUILD / "token"
    work.mkdir(parents=True, exist_ok=True)

    def read(path: Path) -> str:
        r = subprocess.run([str(exe), "--read-token", str(path)], capture_output=True, text=True)
        return (r.stdout + r.stderr).strip()

    token = "eyJhbGciOiJIUzI1NiJ9." + "x" * 40 + ".sig"
    good = work / "steam-token.txt"
    good.write_text("steamid=76561198000000003\r\ntoken=%s\r\n" % token)
    run.check("token file: parses steamid and token (CRLF, as a FAT editor writes)",
              read(good) == "steamid 76561198000000003 token_len %d" % len(token), read(good))

    missing = work / "nope.txt"
    run.check("token file: missing file is an error", read(missing).startswith("FAIL"), read(missing))

    half = work / "half.txt"
    half.write_text("token=%s\n" % token)
    run.check("token file: a token without steamid is rejected", read(half).startswith("FAIL"), read(half))


if __name__ == "__main__":
    sys.exit(main())
