#!/usr/bin/env python3
"""The Steam CM protocol for app 250900 (cloud files), as the console speaks it.

This is the reference implementation for the Switch client: native_steamcm.c
mirrors every message, field number and framing step here, and the
measurements in `STEAM-CLOUD.md` (next to this file) were produced with it. It
is also the provisioning
tool: `login` mints the account's refresh token and `token-file` writes the two
lines the console reads.

    python3 host/steam_cloud_cm.py login       # QR, once
    python3 host/steam_cloud_cm.py token-file  # for the SD
    python3 host/steam_cloud_cm.py cloud --details
    python3 host/steam_cloud_cm.py download --only gamestate1
    python3 host/steam_cloud_cm.py sync-dir --dir <save folder> [--dry-run]
    python3 host/steam_cloud_cm.py revoke --session <file>  # ends that session

`cloud` and `download` only read; `upload` writes, and only the file it is
asked for.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import random
import socket
import ssl
import struct
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SESSION = REPO / "build" / "steam-cloud" / "session.json"
DIRECTORY = "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets"

PROTOCOL_VERSION = 65580
APPID = 250900

EMsg = {
    "ClientHeartBeat": 703,
    "ClientLogOff": 706,
    "ClientLogOnResponse": 751,
    "ClientLoggedOff": 757,
    "ClientHello": 9805,
    "ClientLogon": 5514,
    "ServiceMethodCallFromClient": 151,
    "ServiceMethodResponse": 147,
    "ServiceMethod": 146,
}


# ---- protobuf ---------------------------------------------------------------

def varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def pb(field: int, value, wire: str = "auto") -> bytes:
    """Encode one field. wire: auto (int->varint, str/bytes->bytes), v, b, f64, f32."""
    if wire == "auto":
        wire = "b" if isinstance(value, (str, bytes)) else "v"
    if wire == "v":
        return varint(field << 3) + varint(value)
    if wire == "f64":
        return varint(field << 3 | 1) + value.to_bytes(8, "little")
    if wire == "f32":
        return varint(field << 3 | 5) + value.to_bytes(4, "little")
    data = value.encode() if isinstance(value, str) else value
    return varint(field << 3 | 2) + varint(len(data)) + data


def pb_decode(data: bytes) -> dict[int, list]:
    out: dict[int, list] = {}
    i = 0

    def read_varint() -> int:
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
        key = read_varint()
        field, wire = key >> 3, key & 7
        if wire == 0:
            out.setdefault(field, []).append(read_varint())
        elif wire == 1:
            out.setdefault(field, []).append(int.from_bytes(data[i:i + 8], "little"))
            i += 8
        elif wire == 5:
            out.setdefault(field, []).append(int.from_bytes(data[i:i + 4], "little"))
            i += 4
        elif wire == 2:
            n = read_varint()
            out.setdefault(field, []).append(data[i:i + n])
            i += n
        else:
            raise ValueError("wire type %d at %d" % (wire, i))
    return out


def first(msg: dict, field: int, default=None):
    v = msg.get(field)
    return v[0] if v else default


def text(msg: dict, field: int) -> str:
    v = first(msg, field, b"")
    return v.decode("utf-8", "replace") if isinstance(v, bytes) else str(v)


# ---- WebSocket client (RFC 6455, binary frames only) ------------------------

class WebSocket:
    def __init__(self, host: str, port: int, timeout: float = 20.0, path: str = "/cmsocket/"):
        ctx = ssl.create_default_context()
        raw = socket.create_connection((host, port), timeout=timeout)
        self.sock = ctx.wrap_socket(raw, server_hostname=host)
        self.sock.settimeout(timeout)
        key = os.urandom(16)
        req = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "User-Agent: Valve/Steam HTTP Client 1.0\r\n"
            "\r\n"
        ) % (path, host, port, base64_b(key))
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed during handshake")
            head += chunk
        status = head.split(b"\r\n", 1)[0].decode("latin-1")
        if " 101 " not in status:
            raise ConnectionError("handshake: %s" % status)
        self.rest = head.split(b"\r\n\r\n", 1)[1]

    def _read_exact(self, n: int) -> bytes:
        buf = self.rest[:n]
        self.rest = self.rest[len(buf):]
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("socket closed")
            buf += chunk
        return buf

    def send(self, payload: bytes) -> None:
        mask = os.urandom(4)
        n = len(payload)
        header = bytearray([0x82])
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        header += mask
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def recv(self) -> bytes:
        while True:
            b0, b1 = self._read_exact(2)
            opcode = b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._read_exact(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read_exact(8))[0]
            data = self._read_exact(n)
            if opcode == 0x8:
                raise ConnectionError("websocket close")
            if opcode == 0x9:
                self.sock.sendall(b"\x8a\x80" + os.urandom(4))
                continue
            if opcode in (0x1, 0x2):
                return data
            # pong / continuation: keep reading


def base64_b(data: bytes) -> str:
    import base64
    return base64.b64encode(data).decode()


# ---- CM connection ----------------------------------------------------------

class CM:
    def __init__(self, hosts: list[tuple[str, int]]):
        self.hosts = hosts
        self.ws: WebSocket | None = None
        self.session_id = 0
        self.steamid = 0
        self.job = random.randrange(1, 2 ** 63)

    def connect(self) -> None:
        errors = []
        for host, port in self.hosts:
            try:
                self.ws = WebSocket(host, port)
                print("connected: %s:%d" % (host, port), flush=True)
                return
            except Exception as e:
                errors.append("%s:%d %s" % (host, port, e))
        raise ConnectionError("no CM reachable: " + "; ".join(errors))

    # Wire format (SteamKit2 MsgHdrProtoBuf + ClientMsgProtobuf.Serialize):
    #   [uint32 emsg|0x80000000][uint32 header_len][header protobuf][body to end]
    # There is no body-length field on the websocket transport.
    def _send(self, emsg: int, header: bytes, body: bytes) -> None:
        assert self.ws
        self.ws.send(struct.pack("<II", emsg | 0x80000000, len(header)) + header + body)

    def _recv(self) -> list[tuple[int, dict, bytes]]:
        """One websocket frame, unwrapped: CMsgMulti bundles several messages."""
        assert self.ws
        out: list[tuple[int, dict, bytes]] = []
        self._parse(self.ws.recv(), out)
        return out

    def _parse(self, data: bytes, out: list[tuple[int, dict, bytes]]) -> None:
        raw_emsg = struct.unpack("<I", data[:4])[0]
        emsg = raw_emsg & 0x7FFFFFFF
        if not raw_emsg & 0x80000000:
            # Struct message: MsgHdr is emsg + target jobid + source jobid (20 B).
            out.append((emsg, {}, data[20:]))
            return
        hlen = struct.unpack("<I", data[4:8])[0]
        header = pb_decode(data[8:8 + hlen])
        body = data[8 + hlen:]
        if emsg == 1:                                   # CMsgMulti
            multi = pb_decode(body)
            blob = first(multi, 2, b"")
            if first(multi, 1):                         # size_unzipped: gzip
                import gzip
                blob = gzip.decompress(blob)
            i = 0
            while i + 4 <= len(blob):
                n = struct.unpack("<I", blob[i:i + 4])[0]
                i += 4
                self._parse(blob[i:i + n], out)
                i += n
            return
        out.append((emsg, header, body))

    def header(self, target_job_name: str | None = None, jobid: int | None = None) -> bytes:
        h = pb(1, self.steamid, "f64") + pb(2, self.session_id)
        if jobid is not None:
            h += pb(10, jobid, "f64")
        if target_job_name:
            h += pb(12, target_job_name)
        return h

    def hello(self) -> None:
        self._send(EMsg["ClientHello"], self.header(), pb(1, PROTOCOL_VERSION))

    def logon(self, session: dict, machine_name: str) -> dict:
        self.steamid = int(session["steamid"])
        machine_id = hashlib.sha1(os.urandom(16)).digest() + \
                     hashlib.sha1(os.urandom(16)).digest() + \
                     hashlib.sha1(os.urandom(16)).digest()
        body = b"".join([
            pb(1, PROTOCOL_VERSION),          # protocol_version
            pb(3, 0),                          # cell_id
            pb(5, 1771),                       # client_package_version
            pb(6, "english"),                  # client_language
            pb(7, 16),                         # client_os_type: Windows 10
            pb(8, 1),                          # should_remember_password
            pb(22, self.steamid, "f64"),       # client_supplied_steam_id
            pb(30, machine_id),                # machine_id (3 x SHA1)
            pb(33, 2),                         # chat_mode: new chat
            pb(96, machine_name),              # machine_name
            pb(97, machine_name),              # machine_name_userchosen
            pb(100, random.randrange(1, 2 ** 63), "f64"),   # client_instance_id
            pb(102, 1),                        # supports_rate_limit_response
            # The CM only accepts refresh tokens here (iss=steam), not the
            # access tokens derived from them: steam-user does the same.
            pb(108, session["refresh_token"]),  # access_token
        ])
        self._send(EMsg["ClientLogon"], self.header(), body)
        deadline = time.time() + 30
        while time.time() < deadline:
            for emsg, header, payload in self._recv():
                if emsg == EMsg["ClientLogOnResponse"]:
                    self.session_id = first(header, 2, 0)
                    resp = pb_decode(payload)
                    print("logon: eresult=%d heartbeat=%s cell=%s session=%d extended=%s" % (
                        first(resp, 1, 0), first(resp, 3), first(resp, 7), self.session_id,
                        first(resp, 10)), flush=True)
                    return resp
                print("  (pre-logon message %d)" % emsg, flush=True)
        raise TimeoutError("no logon response")

    def service(self, name: str, request: bytes, timeout: float = 30.0) -> bytes:
        job = random.randrange(1, 2 ** 63)
        self._send(EMsg["ServiceMethodCallFromClient"], self.header(name, job), request)
        deadline = time.time() + timeout
        while time.time() < deadline:
            for emsg, header, payload in self._recv():
                if emsg not in (EMsg["ServiceMethodResponse"], EMsg["ServiceMethod"]):
                    print("  (message %d while waiting for %s)" % (emsg, name), flush=True)
                    continue
                got = text(header, 12)
                if got != name:
                    print("  (service message for %s, ignoring)" % got, flush=True)
                    continue
                eresult = first(header, 13, 2)
                if eresult != 1:
                    raise RuntimeError("%s: eresult %d (%s)" % (name, eresult, text(header, 14)))
                return payload
        raise TimeoutError(name)

    def heartbeat(self) -> None:
        self._send(EMsg["ClientHeartBeat"], self.header(), b"")

    def close(self) -> None:
        try:
            assert self.ws
            self.ws.sock.close()
        except Exception:
            pass


# ---- QR sign-in (one-off token provisioning) --------------------------------

API = "https://api.steampowered.com"


def call(interface: str, method: str, pb_body: bytes) -> dict[int, list]:
    """POST a protobuf body to a Web API service method; returns decoded fields."""
    import base64
    form = urllib.parse.urlencode({"input_protobuf_encoded": base64.b64encode(pb_body).decode()})
    req = urllib.request.Request("%s/%s/%s/v1/" % (API, interface, method), data=form.encode(),
                                 headers={"User-Agent": "isaac-switch-native"})
    with urllib.request.urlopen(req, timeout=20) as r:
        eresult = r.headers.get("x-eresult", "1")
        body = r.read()
    if eresult != "1":
        raise RuntimeError("%s/%s: eresult %s" % (interface, method, eresult))
    return pb_decode(body)


QR_PNG = REPO / "build" / "steam-cloud" / "qr.png"


def show_qr(url: str) -> None:
    print("\nQR (valid ~2 min): %s" % url, flush=True)
    os.system("qrencode -t ASCII -m 2 %s 2>/dev/null" % ("'" + url + "'"))
    QR_PNG.parent.mkdir(parents=True, exist_ok=True)
    os.system("qrencode -t PNG -s 8 -m 2 -o '%s' '%s' 2>/dev/null" % (QR_PNG, url))
    print("image: %s" % QR_PNG, flush=True)
    sys.stdout.flush()


def login(wait: float, session_path: Path, device: str) -> dict:
    """BeginAuthSessionViaQR + PollAuthSessionStatus: a client refresh token.

    Steam expires the whole auth session after a couple of minutes and hands
    out a new challenge URL meanwhile, so this restarts the session and prints
    the new code until one is approved."""
    deadline = time.time() + wait
    while time.time() < deadline:
        begin = call("IAuthenticationService", "BeginAuthSessionViaQR",
                     pb(1, device) + pb(2, 1) + pb(3, pb(1, device) + pb(2, 1), "b"))
        client_id = first(begin, 1)
        request_id = first(begin, 3, b"")
        show_qr(text(begin, 2))
        while time.time() < deadline:
            time.sleep(3)
            try:
                poll = call("IAuthenticationService", "PollAuthSessionStatus",
                            pb(1, client_id) + pb(2, request_id, "b"))
            except RuntimeError as e:
                if "eresult 27" not in str(e):     # 27: Expired
                    raise
                print("auth session expired; starting a new one", flush=True)
                break
            if text(poll, 3):
                refresh = text(poll, 3)
                import base64
                part = refresh.split(".")[1]
                sub = json.loads(base64.urlsafe_b64decode(part + "=" * (-len(part) % 4)))["sub"]
                session = {"platform": "client", "account_name": text(poll, 6),
                           "steamid": sub, "refresh_token": refresh,
                           "access_token": text(poll, 4)}
                session_path.parent.mkdir(parents=True, exist_ok=True)
                if session_path.exists():
                    session_path.with_suffix(".old.json").write_text(session_path.read_text())
                fd = os.open(session_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
                with os.fdopen(fd, "w") as f:
                    json.dump(session, f, indent=1)
                print("signed in as %s (%s)" % (session["account_name"], sub), flush=True)
                print("session written to %s" % session_path, flush=True)
                return session
            client_id = first(poll, 1) or client_id
            if text(poll, 2) and text(poll, 2) != text(begin, 2):
                show_qr(text(poll, 2))
    raise TimeoutError("QR not approved in %.0fs" % wait)


def cm_hosts(limit: int = 4) -> list[tuple[str, int]]:
    with urllib.request.urlopen(DIRECTORY, timeout=20) as r:
        data = json.loads(r.read())
    out = []
    for s in data["response"]["serverlist"][:limit]:
        host, _, port = s["endpoint"].rpartition(":")
        out.append((host, int(port)))
    return out


# ---- two-way sync of a save folder (the same rules the console applies) -----

SLOTS = [("rep+persistentgamedata%d.dat" % n, "persistentgamedata%d.dat" % n) for n in (1, 2, 3)] + \
        [("rep+gamestate%d.dat" % n, "gamestate%d.dat" % n) for n in (1, 2, 3)]
DOCS = Path("documents/my games/binding of isaac repentance+")


def cloud_files(cm) -> dict[str, dict]:
    """name -> {sha_hex, ts, size} from Cloud.EnumerateUserFiles."""
    out = {}
    for raw in P_enumerate(cm):
        f = pb_decode(raw)
        out[text(f, 3)] = {"sha_hex": text(f, 10), "ts": int(first(f, 4, 0)), "size": int(first(f, 5, 0))}
    return out


def P_enumerate(cm):
    resp = pb_decode(cm.service("Cloud.EnumerateUserFiles#1",
                                pb(1, APPID) + pb(2, 1) + pb(3, 100) + pb(4, 0)))
    return resp.get(1, [])


def cloud_fetch(cm, name: str) -> tuple[bytes, str, int]:
    """Download and unwrap one cloud file; returns (raw, sha1 hex, timestamp)."""
    info = pb_decode(cm.service("Cloud.ClientFileDownload#1", pb(1, APPID) + pb(2, name)))
    url = "%s://%s%s" % ("https" if first(info, 9, 1) else "http", text(info, 7), text(info, 8))
    headers = {}
    for raw_h in info.get(10, []):
        h = pb_decode(raw_h)
        headers[text(h, 1)] = text(h, 2)
    data = urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60).read()
    # Steam stores a file compressed or as-is (file_size == raw_file_size then),
    # so only unwrap it when it really is a ZIP.
    if data[:2] == b"PK":
        z = zipfile.ZipFile(io.BytesIO(data))
        raw = z.read(z.namelist()[0])
    else:
        raw = data
    digest = hashlib.sha1(raw).hexdigest()
    want = first(info, 4, b"")
    if want and digest != want.hex():
        raise RuntimeError("%s: SHA-1 mismatch" % name)
    return raw, digest, int(first(info, 5, 0))


def cloud_push(cm, name: str, raw: bytes) -> str:
    """Wrap and upload; returns the server's answer. A duplicate is success."""
    packed = pack_zip(raw)
    sha = hashlib.sha1(raw).digest()
    try:
        begin = pb_decode(cm.service("Cloud.BeginHTTPUpload#1", b"".join([
            pb(1, APPID), pb(2, len(packed)), pb(3, name), pb(4, sha.hex()),
            pb(5, 0), pb(6, "All"), pb(9, 0, "f64")])))
    except RuntimeError as e:
        if "eresult 29" in str(e):
            return "already on Steam (same bytes)"
        raise
    url = "%s://%s%s" % ("https" if first(begin, 5, 1) else "http",
                         text(begin, 3), text(begin, 4))
    headers = {}
    for raw_h in begin.get(6, []):
        h = pb_decode(raw_h)
        headers[text(h, 1)] = text(h, 2)
    req = urllib.request.Request(url, data=packed, headers=headers, method="PUT")
    with urllib.request.urlopen(req, timeout=120) as r:
        status = r.status
    commit = pb_decode(cm.service("Cloud.CommitHTTPUpload#1", b"".join([
        pb(1, 1), pb(2, APPID), pb(3, sha.hex()), pb(4, name)])))
    if first(commit, 1, 0) != 1:
        raise RuntimeError("%s: commit refused" % name)
    return "uploaded (PUT %d)" % status


def pack_zip(raw: bytes) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("z", raw)
    return buf.getvalue()


def sync_dir(args) -> int:
    """Compare a save folder with the cloud, slot by slot, and move the newer side.

    This is the same rule the console applies (newest wins, the replaced copy is
    backed up under save_backups/steam-<date>/), so running it on the PC keeps
    the two machines from drifting apart when the console could not sync.
    """
    root = args.dir
    docs = root / DOCS
    session = json.loads(args.session.read_text())
    cm = CM(cm_hosts())
    cm.connect()
    try:
        cm.hello()
        time.sleep(0.2)
        cm.logon(session, args.machine_name)
        cloud = cloud_files(cm)
        print("%-34s %-12s %-12s %s" % ("file", "console", "steam", "action"))
        plan, failed = [], 0
        for cloud_name, local_name in SLOTS:
            if args.only and args.only not in local_name:
                continue
            path = docs / local_name
            local_sha = hashlib.sha1(path.read_bytes()).hexdigest() if path.exists() else None
            local_ts = int(path.stat().st_mtime) if path.exists() else 0
            entry = cloud.get(cloud_name)
            cloud_sha, cloud_ts = (entry["sha_hex"], entry["ts"]) if entry else (None, 0)
            if local_sha and cloud_sha and local_sha == cloud_sha:
                action = "in sync"
            elif not cloud_sha:
                action = "upload (not on Steam)" if local_sha else "nothing"
            elif not local_sha:
                action = "download (not on the console)"
            else:
                use_local = local_ts >= cloud_ts if args.prefer != "cloud" else False
                if args.prefer == "local":
                    use_local = True
                action = "upload (newer here)" if use_local else "download (newer on Steam)"
            plan.append((cloud_name, local_name, path, action, local_ts, cloud_ts, local_sha))
            print("%-34s %-12s %-12s %s" % (
                local_name,
                time.strftime("%m-%d %H:%M", time.localtime(local_ts)) if local_ts else "-",
                time.strftime("%m-%d %H:%M", time.localtime(cloud_ts)) if cloud_ts else "-",
                action + (" [dry run]" if args.dry_run else "")))

        if args.dry_run:
            return 0
        for cloud_name, local_name, path, action, local_ts, cloud_ts, local_sha in plan:
            if action.startswith("upload"):
                raw = path.read_bytes()
                print("%-34s %s" % (local_name, cloud_push(cm, cloud_name, raw)))
            elif action.startswith("download"):
                raw, sha, ts = cloud_fetch(cm, cloud_name)
                if path.exists():
                    backup = root / "save_backups" / ("steam-" + time.strftime("%Y%m%d-%H%M%S"))
                    backup.mkdir(parents=True, exist_ok=True)
                    (backup / local_name).write_bytes(path.read_bytes())
                    print("%-34s replaced, old copy in %s" % (local_name, backup))
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(raw)
                os.utime(path, (ts, ts))
                print("%-34s downloaded %d bytes" % (local_name, len(raw)))
        return 0 if not failed else 1
    finally:
        cm.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["login", "logon", "cloud", "download", "upload", "token-file",
                                        "sync-dir", "revoke", "owned"])
    ap.add_argument("--appid", type=int, default=APPID)
    ap.add_argument("--details", action="store_true", help="extended_details on EnumerateUserFiles")
    ap.add_argument("--session", type=Path, default=SESSION)
    ap.add_argument("--apps", default="250900,401920,570660,1426300,3353470",
                    help="owned: app ids to ask about, comma separated")
    ap.add_argument("--machine-name", default="DESKTOP-ISAAC")
    ap.add_argument("--wait", type=float, default=600.0, help="login: seconds to wait for the QR")
    ap.add_argument("--out", type=Path, default=REPO / "build" / "steam-cloud" / "download")
    ap.add_argument("--only", help="download: substring of the file name")
    ap.add_argument("--file", type=Path, help="upload: local file to send")
    ap.add_argument("--name", help="upload: cloud file name (e.g. rep+persistentgamedata3.dat)")
    ap.add_argument("--mode", choices=["http", "client"], default="http", help="upload: which Cloud upload flow")
    ap.add_argument("--dir", type=Path, help="sync-dir: the save folder (contains documents/)")
    ap.add_argument("--dry-run", action="store_true", help="sync-dir: only report what would move")
    ap.add_argument("--prefer", choices=["newest", "local", "cloud"], default="newest",
                    help="sync-dir: which side wins when both differ")
    args = ap.parse_args()

    if args.command == "login":
        login(args.wait, args.session, "Isaac on Switch (NRO)")
        return 0

    if args.command == "sync-dir":
        if not args.dir:
            sys.exit("sync-dir needs --dir <save folder>")
        return sync_dir(args)

    if args.command == "token-file":
        # What the console reads from sdmc:/switch/isaac-recomp/steam-token.txt:
        # the account id and the refresh token, nothing else.
        session = json.loads(args.session.read_text())
        out = args.out if args.out != REPO / "build" / "steam-cloud" / "download" else \
            REPO / "build" / "steam-cloud" / "steam-token.txt"
        out.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write("steamid=%s\ntoken=%s\n" % (session["steamid"], session["refresh_token"]))
        print("wrote %s (mode 0600); copy it to sdmc:/switch/isaac-recomp/steam-token.txt" % out)
        return 0

    session = json.loads(args.session.read_text())
    cm = CM(cm_hosts())
    cm.connect()
    try:
        cm.hello()
        time.sleep(0.2)
        resp = cm.logon(session, args.machine_name)
        if first(resp, 1, 0) != 1:
            return 1
        if args.command == "revoke":
            # What the console's sign-out sends; a second logon shows whether
            # Steam really dropped the token. The session file goes only then.
            cm.service("Authentication.RevokeToken#1", pb(1, session["refresh_token"]) + pb(2, 1))
            print("RevokeToken: eresult 1")
            cm.close()
            cm = CM(cm_hosts())
            cm.connect()
            cm.hello()
            time.sleep(0.2)
            again = first(cm.logon(session, args.machine_name), 1, 0)
            if again == 1:
                print("the token still logs on: NOT revoked")
                return 1
            args.session.unlink()
            print("the token is refused (eresult %d): revoked; %s removed" % (again, args.session))
            return 0
        if args.command == "owned":
            # What the console's pull asks (cm_owns in native_steamcm.c).
            for appid in [int(a) for a in args.apps.split(",")]:
                cm._send(857, cm.header(), pb(1, appid))
                deadline = time.time() + 20
                reply = None
                while reply is None and time.time() < deadline:
                    for emsg, _, body in cm._recv():
                        if emsg == 858:
                            reply = pb_decode(body)
                if reply is None:
                    print("%-8d no answer" % appid)
                    continue
                print("%-8d eresult=%-3s app_id=%s ticket=%d bytes" % (
                    appid, first(reply, 1, 2), first(reply, 2), len(first(reply, 3, b""))))
            return 0
        if args.command == "logon":
            cm.heartbeat()
            print("heartbeat sent")
            return 0

        if args.command == "upload":
            import zipfile
            raw = args.file.read_bytes()
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
                z.writestr("z", raw)
            packed = buf.getvalue()
            sha = hashlib.sha1(raw).digest()
            print("upload %s: raw=%d zip=%d sha1=%s" % (args.name, len(raw), len(packed), sha.hex()))
            if args.mode == "http":
                begin = pb_decode(cm.service("Cloud.BeginHTTPUpload#1", b"".join([
                    pb(1, args.appid), pb(2, len(packed)), pb(3, args.name),
                    pb(4, sha.hex()), pb(5, 0), pb(6, "All"), pb(9, 0, "f64")])))
                print("  begin: ugcid=%s host=%s path=%s https=%s" % (
                    first(begin, 1), text(begin, 3), text(begin, 4), first(begin, 5)))
                url = "%s://%s%s" % ("https" if first(begin, 5, 1) else "http",
                                     text(begin, 3), text(begin, 4))
                headers = {}
                for raw_h in begin.get(6, []):
                    h = pb_decode(raw_h)
                    headers[text(h, 1)] = text(h, 2)
                req = urllib.request.Request(url, data=packed, headers=headers, method="PUT")
                with urllib.request.urlopen(req, timeout=120) as r:
                    print("  put: http %d (%d B)" % (r.status, len(packed)))
                commit = pb_decode(cm.service("Cloud.CommitHTTPUpload#1", b"".join([
                    pb(1, 1), pb(2, args.appid), pb(3, sha.hex()), pb(4, args.name)])))
                print("  commit: file_committed=%s" % first(commit, 1, 0))
                return 0
            batch = pb_decode(cm.service("Cloud.BeginAppUploadBatch#1", b"".join([
                pb(1, args.appid), pb(2, args.machine_name), pb(3, args.name),
                pb(5, random.randrange(1, 2 ** 63), "f64")])))
            batch_id = first(batch, 1, 0)
            print("  batch=%s app_change=%s" % (batch_id, first(batch, 4)))
            begin = pb_decode(cm.service("Cloud.ClientBeginFileUpload#1", b"".join([
                pb(1, args.appid), pb(2, len(packed)), pb(3, len(raw)), pb(4, sha, "b"),
                pb(5, int(time.time()), "f64"), pb(6, args.name), pb(7, 0xFFFFFFFF),
                pb(10, 0), pb(11, 0), pb(13, batch_id, "f64")])))
            print("  begin: encrypt=%s blocks=%d" % (first(begin, 1, 0), len(begin.get(2, []))))
            for raw_block in begin.get(2, []):
                b = pb_decode(raw_block)
                url = "%s://%s%s" % ("https" if first(b, 3, 1) else "http", text(b, 1), text(b, 2))
                headers = {}
                for raw_h in b.get(5, []):
                    h = pb_decode(raw_h)
                    headers[text(h, 1)] = text(h, 2)
                off, length = first(b, 6, 0), first(b, 7, 0)
                body = first(b, 8) or packed[off:off + length]
                method = {0: "GET", 1: "POST", 2: "PUT"}.get(first(b, 4, 2), "PUT")
                print("  block %d+%d via %s %s" % (off, length, method, url.split("?")[0]))
                req = urllib.request.Request(url, data=body, headers=headers, method=method)
                with urllib.request.urlopen(req, timeout=120) as r:
                    print("    http %d (%d B sent)" % (r.status, len(body)))
            commit = pb_decode(cm.service("Cloud.ClientCommitFileUpload#1", b"".join([
                pb(1, 1), pb(2, args.appid), pb(3, sha, "b"), pb(4, args.name)])))
            print("  commit: file_committed=%s" % first(commit, 1, 0))
            done = pb_decode(cm.service("Cloud.CompleteAppUploadBatchBlocking#1", b"".join([
                pb(1, args.appid), pb(2, batch_id, "f64"), pb(3, 1)])))
            print("  batch complete: %s" % (done or "ok"))
            return 0

        request = b"".join([pb(1, args.appid), pb(2, 1 if args.details else 0), pb(3, 100), pb(4, 0)])
        payload = cm.service("Cloud.EnumerateUserFiles#1", request)
        files = pb_decode(payload)
        print("total_files=%s" % first(files, 2))
        names = []
        for raw in files.get(1, []):
            f = pb_decode(raw)
            name = text(f, 3)
            names.append(name)
            print("  %-34s size=%-8s ts=%s ugcid=%s sha=%s platforms=%s" % (
                name, first(f, 5), first(f, 4), first(f, 2),
                text(f, 10) or "-",
                [p.decode() for p in f.get(9, [])]))
        if args.command == "download":
            args.out.mkdir(parents=True, exist_ok=True)
            for name in names:
                if args.only and args.only not in name:
                    continue
                info = pb_decode(cm.service("Cloud.ClientFileDownload#1",
                                            pb(1, args.appid) + pb(2, name)))
                host, path = text(info, 7), text(info, 8)
                want = first(info, 4, b"")
                url = "%s://%s%s" % ("https" if first(info, 9, 1) else "http", host, path)
                headers = {}
                for raw_h in info.get(10, []):
                    h = pb_decode(raw_h)
                    headers[text(h, 1)] = text(h, 2)
                req = urllib.request.Request(url, headers=headers)
                with urllib.request.urlopen(req, timeout=120) as r:
                    data = r.read()
                if data[:2] == b"PK":
                    z = zipfile.ZipFile(io.BytesIO(data))
                    data = z.read(z.namelist()[0])
                got = hashlib.sha1(data).hexdigest()
                ok = got == want.hex()
                (args.out / name.replace("/", "_")).write_bytes(data)
                print("  %-34s %8d B  sha1 %s  encrypted=%s" % (
                    name, len(data), "ok" if ok else "MISMATCH", first(info, 11, 0)))
                if not ok:
                    print("     want %s got %s" % (want.hex(), got))
        return 0
    finally:
        cm.close()


if __name__ == "__main__":
    sys.exit(main())
