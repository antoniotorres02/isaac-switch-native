#!/usr/bin/env python3
"""Steam Cloud access for the native build's saves (PC prototype).

Steam's ICloudService Web API lists, downloads and uploads a user's cloud
files for one app; Valve documents it for non-Steam versions of a game
(https://partner.steamgames.com/doc/webapi/icloudservice). It needs a user
access token, obtained here with Steam's own QR sign-in: the terminal shows a
QR code, the Steam mobile app scans and approves it, and the session's refresh
token is kept in build/steam-cloud/session.json (mode 0600, never committed).

    python host/steam_cloud.py login
    python host/steam_cloud.py list
    python host/steam_cloud.py download --out build/steam-cloud/download

This prototype only reads from Steam Cloud. Nothing is uploaded or deleted.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

APPID = 250900                      # The Binding of Isaac: Rebirth
API = "https://api.steampowered.com"
REPO = Path(__file__).resolve().parents[1]
SESSION = REPO / "build" / "steam-cloud" / "session.json"
PLATFORMS = {"client": 1, "web": 2, "mobile": 3}


# ---- protobuf, the few wire types the auth service needs -------------------

def _varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def pb_varint(field: int, value: int) -> bytes:
    return _varint(field << 3) + _varint(value)


def pb_bytes(field: int, value: bytes | str) -> bytes:
    if isinstance(value, str):
        value = value.encode()
    return _varint(field << 3 | 2) + _varint(len(value)) + value


def pb_fixed64(field: int, value: int) -> bytes:
    return _varint(field << 3 | 1) + value.to_bytes(8, "little")


def pb_decode(data: bytes) -> dict[int, list]:
    """Field number -> values (ints for varint/fixed, bytes for length-delimited)."""
    out: dict[int, list] = {}
    i = 0

    def varint() -> int:
        nonlocal i
        n = shift = 0
        while True:
            b = data[i]
            i += 1
            n |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return n
    while i < len(data):
        key = varint()
        field, wire = key >> 3, key & 7
        if wire == 0:
            v = varint()
        elif wire == 1:
            v = int.from_bytes(data[i:i + 8], "little")
            i += 8
        elif wire == 2:
            n = varint()
            v = data[i:i + n]
            i += n
        elif wire == 5:
            v = int.from_bytes(data[i:i + 4], "little")
            i += 4
        else:
            raise ValueError("protobuf wire type %d" % wire)
        out.setdefault(field, []).append(v)
    return out


def pb_get(msg: dict[int, list], field: int, default=None):
    return msg.get(field, [default])[0]


def pb_str(msg: dict[int, list], field: int) -> str:
    v = pb_get(msg, field, b"")
    return v.decode() if isinstance(v, bytes) else ""


# ---- HTTP ------------------------------------------------------------------

class SteamError(RuntimeError):
    pass


def call(interface: str, method: str, *, get: dict | None = None, pb: bytes | None = None,
         token: str | None = None):
    """One Web API service call. A GET returns the JSON `response` object; a
    protobuf POST returns the decoded protobuf fields. Steam reports failures
    in the x-eresult header (1 is OK)."""
    url = "%s/%s/%s/v1/" % (API, interface, method)
    data = None
    if get is not None:
        q = dict(get)
        if token:
            q["access_token"] = token
        url += "?" + urllib.parse.urlencode(q)
    else:
        form = {"input_protobuf_encoded": base64.b64encode(pb or b"").decode()}
        if token:
            form["access_token"] = token
        data = urllib.parse.urlencode(form).encode()
    req = urllib.request.Request(url, data=data, headers={"User-Agent": "isaac-switch-native"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            eresult = r.headers.get("x-eresult", "1")
            body = r.read()
    except urllib.error.HTTPError as e:
        raise SteamError("%s/%s: HTTP %d, eresult %s" % (interface, method, e.code,
                                                          e.headers.get("x-eresult")))
    if eresult != "1":
        raise SteamError("%s/%s: eresult %s" % (interface, method, eresult))
    if get is None:
        return pb_decode(body)
    return json.loads(body or b"{}").get("response", {})


# ---- session ---------------------------------------------------------------

def jwt_claims(token: str) -> dict:
    part = token.split(".")[1]
    return json.loads(base64.urlsafe_b64decode(part + "=" * (-len(part) % 4)))


def save_session(s: dict) -> None:
    SESSION.parent.mkdir(parents=True, exist_ok=True)
    tmp = SESSION.with_suffix(".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(s, f, indent=1)
    os.replace(tmp, SESSION)


def show_qr(url: str) -> None:
    print("\nScan this with the Steam mobile app (Steam Guard > scan a QR code):\n")
    if shutil.which("qrencode"):
        subprocess.run(["qrencode", "-t", "ANSIUTF8", "-m", "2", url])
    print("  ", url, "\n")


def login(platform: str) -> dict:
    ptype = PLATFORMS[platform]
    name = "Isaac on Switch (prototype)"
    req = (pb_bytes(1, name) + pb_varint(2, ptype)
           + pb_bytes(3, pb_bytes(1, name) + pb_varint(2, ptype)))
    begin = call("IAuthenticationService", "BeginAuthSessionViaQR", pb=req)
    client_id = pb_get(begin, 1)
    request_id = pb_get(begin, 3, b"")
    interval = 5.0
    if 4 in begin:      # float, wire type 5
        import struct
        interval = struct.unpack("<f", pb_get(begin, 4).to_bytes(4, "little"))[0] or 5.0
    show_qr(pb_str(begin, 2))
    deadline = time.time() + 300
    while time.time() < deadline:
        time.sleep(interval)
        poll = call("IAuthenticationService", "PollAuthSessionStatus",
                    pb=pb_varint(1, client_id) + pb_bytes(2, request_id))
        if pb_str(poll, 2):
            client_id = pb_get(poll, 1, client_id)
            show_qr(pb_str(poll, 2))
        if pb_str(poll, 3):
            refresh = pb_str(poll, 3)
            s = {"platform": platform, "account_name": pb_str(poll, 6),
                 "steamid": jwt_claims(refresh)["sub"], "refresh_token": refresh,
                 "access_token": pb_str(poll, 4)}
            save_session(s)
            print("signed in as %s (%s); session saved in %s" % (s["account_name"], s["steamid"], SESSION))
            return s
    raise SteamError("the QR code was not approved within 5 minutes")


def access_token() -> str:
    if not SESSION.exists():
        sys.exit("no session: run `steam_cloud.py login` first")
    s = json.loads(SESSION.read_text())
    tok = s.get("access_token", "")
    if tok and jwt_claims(tok).get("exp", 0) > time.time() + 60:
        return tok
    r = call("IAuthenticationService", "GenerateAccessTokenForApp",
             pb=pb_bytes(1, s["refresh_token"]) + pb_fixed64(2, int(s["steamid"])))
    s["access_token"] = pb_str(r, 1)
    if pb_str(r, 2):
        s["refresh_token"] = pb_str(r, 2)
    save_session(s)
    return s["access_token"]


# ---- cloud -----------------------------------------------------------------

def list_files(token: str) -> list[dict]:
    files, start = [], 0
    while True:
        r = call("ICloudService", "EnumerateUserFiles", token=token,
                 get={"appid": APPID, "extended_details": 1, "count": 500, "start_index": start})
        files += r.get("files", [])
        start = len(files)
        if start >= int(r.get("total_files", 0)) or not r.get("files"):
            return files


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    lg = sub.add_parser("login", help="sign in with a QR code from the Steam mobile app")
    lg.add_argument("--platform", choices=sorted(PLATFORMS), default="client")
    sub.add_parser("list", help="list this account's Isaac cloud files")
    dl = sub.add_parser("download", help="download every Isaac cloud file into a folder")
    dl.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    if args.cmd == "login":
        login(args.platform)
        return 0
    token = access_token()
    files = list_files(token)
    if args.cmd == "list":
        for f in sorted(files, key=lambda f: f["filename"]):
            when = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(int(f["timestamp"])))
            print("%-40s %8s bytes  %s  sha1 %s" % (f["filename"], f["file_size"], when, f.get("file_sha", "")))
        print("%d file(s)" % len(files))
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    for f in files:
        with urllib.request.urlopen(f["url"], timeout=60) as r:
            data = r.read()
        sha = hashlib.sha1(data).hexdigest()
        ok = sha == f.get("file_sha", sha).lower()
        (args.out / f["filename"].replace("/", "_")).write_bytes(data)
        print("%-40s %8d bytes  sha1 %s" % (f["filename"], len(data), "ok" if ok else "MISMATCH"))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except SteamError as e:
        sys.exit("steam: %s" % e)
