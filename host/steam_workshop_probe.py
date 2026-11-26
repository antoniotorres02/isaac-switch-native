#!/usr/bin/env python3
"""Host diagnostic for the native Workshop client, using the Cloud probe's CM.

An existing QR session file is required. Downloads go to a newly created
directory. The diagnostic never sends subscription mutations.
"""
import argparse
import contextlib
import ctypes as c
import http.client
import io
import json
import os
import secrets
import subprocess
import tempfile
import time
from pathlib import Path

from steam_cloud_cm import CM, cm_hosts, first, pb, pb_decode

NATIVE = Path(__file__).resolve().parent / "native"
APP = 250900
ERR = 192
PTR, STR, SIZE, U64, U32 = c.c_void_p, c.c_char_p, c.c_size_t, c.c_uint64, c.c_uint32
CALL = c.CFUNCTYPE(c.c_int, PTR, STR, PTR, SIZE, PTR, SIZE, c.POINTER(SIZE), PTR)
GET = c.CFUNCTYPE(c.c_int, PTR, STR, STR, PTR, SIZE, c.POINTER(SIZE), c.POINTER(c.c_int), PTR)
KEY = c.CFUNCTYPE(c.c_int, PTR, U32, U32, PTR, PTR)
CANCEL = c.CFUNCTYPE(c.c_int, PTR)
PROGRESS = c.CFUNCTYPE(None, PTR, U64, U64)
OPERATION = c.CFUNCTYPE(None, PTR, U32, U32)


class Network(c.Structure):
    _fields_ = [("ctx", PTR), ("steamid", U64), ("call", CALL), ("get", GET), ("key", KEY),
                ("cancelled", CANCEL), ("progress", PROGRESS), ("cell", U32), ("operation", OPERATION)]


DRIVER = r'''
#include "native_steamcdn.h"
#include <stdio.h>

int probe(const workshop_net *net, int command, const char *search, uint64_t id,
          const char *directory, char err[WORKSHOP_ERR]) {
    if (command == 0 || command == 1) {
        workshop_page page;
        int rc = command == 0
            ? workshop_query(net, *search ? WORKSHOP_RELEVANT : WORKSHOP_POPULAR, search, "", "", &page, err)
            : workshop_subscriptions(net, 1, &page, err);
        if (rc) return rc;
        printf("%u results\n", page.total);
        for (size_t i = 0; i < page.count; ++i)
            printf("%llu\t%s\n", (unsigned long long)page.items[i].id, page.items[i].title);
        return 0;
    }
    workshop_item item;
    if (workshop_details(net, id, &item, err)) return -1;
    printf("%llu\t%s\n", (unsigned long long)item.id, item.title);
    if (command == 2) {
        printf("Manifest %llu, %llu bytes, %zu dependencies\n", (unsigned long long)item.manifest,
               (unsigned long long)item.size, item.dep_count);
        return 0;
    }
    mod_store store;
    if (mod_store_init(&store, directory, NULL)) {
        snprintf(err, WORKSHOP_ERR, "Output path too long");
        return -1;
    }
    return workshop_install(net, &store, &item, err);
}
'''


def build(directory: Path) -> c.CDLL:
    driver = directory / "probe.c"
    driver.write_text(DRIVER)
    library = directory / "probe.so"
    libs = subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "libcrypto", "liblzma", "libzstd",
         "libpng", "libjpeg", "zlib"], text=True
    ).split()
    modules = ("steamproto", "sha1", "zip", "steamworkshop", "steamcdn", "workshop_media", "modmanager", "modstate")
    subprocess.run(
        [os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-shared", "-fPIC", "-I", str(NATIVE),
         str(driver), *(str(NATIVE / f"native_{name}.c") for name in modules), *libs, "-o", str(library)],
        check=True,
    )
    loaded = c.CDLL(str(library))
    loaded.probe.argtypes = [c.POINTER(Network), c.c_int, STR, U64, STR, PTR]
    loaded.probe.restype = c.c_int
    return loaded


def app_request(cm: CM, request: int, response: int, body: bytes) -> dict:
    job = secrets.randbelow(2**63 - 1) + 1
    cm._send(request, cm.header(jobid=job), body)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        for emsg, header, payload in cm._recv():
            if emsg == response and first(header, 11, job) == job:
                return pb_decode(payload)
    raise TimeoutError("Steam app request timed out")


def error_at(address: int, message: str) -> int:
    c.memmove(address, message.encode() + b"\0", len(message) + 1)
    return -1


def make_network(cm: CM, cell: int) -> Network:
    last_heartbeat = time.monotonic()

    @CALL
    def call(_, method, req, n, buf, cap, length, error):
        try:
            result = cm.service(method.decode("ascii"), c.string_at(req, n))
        except Exception:
            return error_at(error, f"{method.decode('ascii')} failed")
        if len(result) > cap:
            return error_at(error, "Steam response exceeded the probe limit")
        c.memmove(buf, result, len(result))
        length[0] = len(result)
        return 0

    @GET
    def get(_, hostname, path, buf, cap, length, status, error):
        nonlocal last_heartbeat
        host = hostname.decode("ascii")
        try:
            if time.monotonic() - last_heartbeat >= 5:
                cm.heartbeat()
                last_heartbeat = time.monotonic()
            connection = http.client.HTTPSConnection(host, timeout=10)
            try:
                connection.request("GET", path.decode("ascii"))
                response = connection.getresponse()
                status[0] = response.status
                if response.status != 200:
                    return 0
                result = response.read(cap + 1)
                if len(result) > cap:
                    return error_at(error, "Steam response exceeded the probe limit")
                c.memmove(buf, result, len(result))
                length[0] = len(result)
                return 0
            finally:
                connection.close()
        except Exception:
            return error_at(error, "Steam CDN request failed")

    @KEY
    def key(_, app, depot, out, error):
        try:
            reply = app_request(cm, 5438, 5439, pb(1, depot) + pb(2, app))
            value = first(reply, 3, b"")
            if first(reply, 1) == 1 and first(reply, 2) == depot and isinstance(value, bytes) and len(value) == 32:
                c.memmove(out, value, 32)
                return 0
        except Exception:
            pass
        return error_at(error, "Steam did not grant the Workshop depot key")

    @PROGRESS
    def progress(_, current, total):
        if current == 0 or current == total:
            print(f"{current:,} / {total:,} bytes verified", flush=True)

    return Network(None, cm.steamid, call, get, key, CANCEL(), progress, cell, OPERATION())


def read_session(path: Path) -> dict:
    raw = path.read_text().strip()
    if raw.startswith("{"):
        return json.loads(raw)
    values = dict(line.split("=", 1) for line in raw.splitlines() if "=" in line)
    return {"steamid": values["steamid"], "refresh_token": values["token"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("browse", "subscriptions", "details", "download"))
    parser.add_argument("--session", type=Path, required=True,
                        help="Existing Steam Cloud QR session JSON or Switch steam-token.txt")
    parser.add_argument("--search", default="")
    parser.add_argument("--id", type=int, default=0)
    parser.add_argument("--output", type=Path, help="New, empty directory for the diagnostic download")
    args = parser.parse_args()
    if args.command in ("details", "download") and not 0 < args.id < 2**64:
        parser.error("Use --id with the published Workshop ID")
    if args.command == "download" and (args.output is None or args.output.exists()):
        parser.error("Use --output with a directory that does not exist")
    try:
        session = read_session(args.session)
        hosts = [(host, port) for host, port in cm_hosts()
                 if host.endswith((".steamserver.net", ".steampowered.com")) and 1 <= port <= 65535]
        cm = CM(hosts)
        with tempfile.TemporaryDirectory(prefix="isaac-workshop-probe-") as tmp:
            library = build(Path(tmp))
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    cm.connect()
                    cm.hello()
                    logon = cm.logon(session, "Isaac Workshop diagnostic")
                if first(logon, 1) != 1:
                    parser.exit(1, "Steam refused the existing QR session\n")
                command = ("browse", "subscriptions", "details", "download").index(args.command)
                if command == 3:
                    owns = app_request(cm, 857, 858, pb(1, APP))
                    if first(owns, 1) != 1:
                        parser.exit(1, "Steam did not confirm ownership of Isaac\n")
                    args.output.mkdir(parents=False, exist_ok=False)
                network = make_network(cm, first(logon, 7, 0))
                error = c.create_string_buffer(ERR)
                rc = library.probe(c.byref(network), command, args.search.encode(), args.id,
                                   os.fsencode(args.output) if args.output else b"", error)
                if rc:
                    parser.exit(1, error.value.decode("utf-8", "replace") + "\n")
            finally:
                cm.close()
    except (OSError, RuntimeError, ValueError, TypeError, KeyError, subprocess.SubprocessError):
        parser.exit(1, "Could not complete the Workshop probe. Check the session, toolchain and network.\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
