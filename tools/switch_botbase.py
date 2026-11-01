#!/usr/bin/env python3
"""Control the original sys-botbase over TCP; keep evidence under build/.

A successful input command acknowledges the
service operation, not the resulting UI. Inspect the following screenshot.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager, suppress
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import socket
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
BUTTONS = ("A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LSTICK", "RSTICK",
           "DUP", "DDOWN", "DLEFT", "DRIGHT", "HOME")


class ProtocolError(RuntimeError):
    pass


class JsonInput:
    """Bounded JSON lines; partial input does not renew the idle deadline."""
    def __init__(self, stream):
        self.stream = stream
        self.pending = bytearray()

    def read(self, timeout: float) -> dict | None:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not select.select([self.stream], [], [], remaining)[0]:
                raise TimeoutError("Session idle deadline exceeded")
            chunk = os.read(self.stream.fileno(), 4096)
            if not chunk:
                if self.pending:
                    raise ValueError("Incomplete JSON request at EOF")
                return None
            self.pending.extend(chunk)
            if len(self.pending) > 4096:
                raise ValueError("Session request exceeds 4096 bytes")
        raw, _, rest = self.pending.partition(b"\n")
        self.pending = bytearray(rest)
        request = json.loads(raw)
        if not isinstance(request, dict):
            raise ValueError("Expected a JSON object")
        return request


def decode_capture(reply: bytes) -> bytes:
    if not reply or len(reply) % 2 or re.fullmatch(rb"[0-9A-Fa-f]+", reply) is None:
        raise ProtocolError("Capture failed: " + reply[:200].decode("ascii", errors="replace"))
    data = bytes.fromhex(reply.decode("ascii"))
    if not data.startswith(b"\xff\xd8") or not data.endswith(b"\xff\xd9"):
        raise ProtocolError("Capture is not a complete JPEG")
    return data


class Botbase:
    def __init__(self, address: str, port: int = 6000, timeout: float = 15,
                 expected_version: str = "2.5"):
        if not 0 < timeout <= 60:
            raise ValueError("timeout must be in (0, 60] seconds")
        self.timeout = timeout
        self.expected_version = expected_version
        lock_directory = ROOT / "build/switch-control-locks"
        lock_directory.mkdir(parents=True, exist_ok=True)
        key = hashlib.sha256(f"{address}:{port}".encode()).hexdigest()
        self.lock = (lock_directory / f"{key}.lock").open("a")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.sock = socket.create_connection((address, port), timeout=min(timeout, 5))
        except BaseException:
            self.lock.close()
            raise
        self.pending = bytearray()

    def __enter__(self):
        try:
            self.sync()
            self.send("configure echoCommands 0")
            self.send("configure printDebugResultCodes 1")
            self.sync()
            return self
        except BaseException:
            self.close()
            raise

    def __exit__(self, *args):
        self.close()

    def close(self):
        with suppress(OSError):
            self.send("configure printDebugResultCodes 0")
        self.sock.close()
        self.lock.close()

    def send(self, command: str):
        if not command or "\n" in command or "\r" in command:
            raise ValueError("Expected one nonempty command")
        self.sock.settimeout(self.timeout)
        self.sock.sendall(command.encode("ascii") + b"\r\n")

    def line(self, deadline: float | None = None) -> bytes:
        deadline = deadline if deadline is not None else time.monotonic() + self.timeout
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Reply deadline exceeded")
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ProtocolError("Connection closed before a complete reply")
            self.pending.extend(chunk)
            if len(self.pending) > 2 * 1024 * 1024:
                raise ProtocolError("Reply exceeds 2 MiB")
        line, _, rest = self.pending.partition(b"\n")
        self.pending = bytearray(rest)
        return bytes(line).rstrip(b"\r")

    def query(self, command: str) -> bytes:
        self.send(command)
        return self.line()

    def sync(self) -> str:
        """Use getVersion as a barrier and reject preceding service errors."""
        self.send("getVersion")
        deadline = time.monotonic() + self.timeout
        messages = []
        for _ in range(32):
            reply = self.line(deadline).decode("ascii", errors="replace")
            if reply == self.expected_version:
                if messages:
                    raise ProtocolError("Unexpected service response: " + "; ".join(messages))
                return reply
            messages.append(reply)
        raise ProtocolError("Version barrier did not complete")

    def prepare_controller(self):
        # Initialize with neutral sticks before sending the first real button.
        self.send("setStick LEFT 0 0")
        self.send("setStick RIGHT 0 0")
        self.sync()
        time.sleep(0.5)

    def click(self, button: str, hold_ms: int = 250):
        if button not in BUTTONS or not 100 <= hold_ms <= 1000:
            raise ValueError("Invalid button or click duration")
        self.prepare_controller()
        self.send(f"configure buttonClickSleepTime {hold_ms}")
        self.send(f"click {button}")
        self.sync()

    @contextmanager
    def held(self, button: str | None):
        """Keep a button down until the observed route completes."""
        if button is not None and button not in BUTTONS:
            raise ValueError("Invalid held button")
        if button is None:
            yield
            return
        self.prepare_controller()
        try:
            self.send(f"press {button}")
            self.sync()
            yield
        finally:
            self.send(f"release {button}")
            self.sync()

    def neutral(self):
        self.send("clickCancel")
        for button in BUTTONS:
            self.send(f"release {button}")
        self.send("setStick LEFT 0 0")
        self.send("setStick RIGHT 0 0")
        self.sync()

    def capture(self) -> bytes:
        return decode_capture(self.query("pixelPeek"))


def interactive_session(bot: Botbase, directory: Path, result: dict,
                        held_button: str | None, idle_timeout: float):
    """One TCP owner; JSON lines allow observation between input steps."""
    result["steps"] = []
    with bot.held(held_button):
        print(json.dumps({"event": "ready", "run_id": result["run_id"],
                          "held_button": held_button, "idle_timeout": idle_timeout,
                          "receipt_path": result["receipt_path"]}), flush=True)
        try:
            requests = JsonInput(sys.stdin)
            while True:
                request = requests.read(idle_timeout)
                if request is None:
                    break
                action = request.get("action")
                if action == "stop":
                    break
                step = {"action": action, "ui_transition_verified": False}
                if action == "capture":
                    data = bot.capture()
                    path = directory / f"screen-{len(result['steps']) + 1:03d}.jpg"
                    path.write_bytes(data)
                    step.update(image_path=str(path), bytes=len(data),
                                sha256=hashlib.sha256(data).hexdigest())
                elif action == "click":
                    button = request.get("button")
                    if button == held_button:
                        raise ValueError("Cannot click the button held by this session")
                    bot.click(button, request.get("hold_ms", 250))
                    step.update(button=button, input_sent=True)
                else:
                    raise ValueError("Session actions are capture, click and stop")
                step["ok"] = True
                result["steps"].append(step)
                (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
                print(json.dumps(step), flush=True)
        finally:
            # Cancels old queued sequences and clears the whole virtual pad.
            bot.neutral()
            result["inputs_released"] = True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True)
    parser.add_argument("--port", type=int, default=6000)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--expected-version", default="2.5")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/switch-control")
    commands = parser.add_subparsers(dest="action", required=True)
    commands.add_parser("status")
    commands.add_parser("capture")
    commands.add_parser("neutral")
    click = commands.add_parser("click")
    click.add_argument("button", choices=BUTTONS)
    click.add_argument("--hold-ms", type=int, default=250)
    session = commands.add_parser("session")
    session.add_argument("--hold", choices=BUTTONS)
    session.add_argument("--idle-timeout", type=float, default=45)
    args = parser.parse_args()
    if not 0 < args.timeout <= 60 or not 1 <= args.port <= 65535:
        parser.error("Invalid timeout or TCP port")
    if args.action == "click" and not 100 <= args.hold_ms <= 1000:
        parser.error("--hold-ms must be 100..1000")
    if args.action == "session" and not 1 <= args.idle_timeout <= 60:
        parser.error("--idle-timeout must be 1..60 seconds")
    output = args.output_dir.resolve()
    if not output.is_relative_to(ROOT / "build"):
        parser.error("Evidence must remain under the ignored build/ directory")
    run_id = uuid.uuid4().hex
    directory = output / run_id
    directory.mkdir(parents=True, exist_ok=False)
    result = {"run_id": run_id, "started_at_utc": datetime.now(timezone.utc).isoformat(),
              "address": args.address, "port": args.port, "transport": "TCP",
              "action": args.action, "receipt_path": str(directory / "result.json"),
              "ui_transition_verified": False}
    started = time.monotonic()
    try:
        with Botbase(args.address, args.port, args.timeout, args.expected_version) as bot:
            result["version"] = bot.expected_version
            if args.action == "status":
                result["title_id"] = bot.query("getTitleID").decode("ascii")
            elif args.action == "click":
                bot.click(args.button, args.hold_ms)
                result.update(button=args.button, hold_ms=args.hold_ms, input_sent=True)
            elif args.action == "session":
                interactive_session(bot, directory, result, args.hold, args.idle_timeout)
            elif args.action == "neutral":
                bot.neutral()
            elif args.action == "capture":
                data = bot.capture()
                image = directory / "screen.jpg"
                image.write_bytes(data)
                result.update(image_path=str(image), bytes=len(data),
                              sha256=hashlib.sha256(data).hexdigest())
        result["ok"] = True
    except (OSError, ValueError, TypeError, RuntimeError, KeyboardInterrupt) as error:
        result.update(ok=False, error=type(error).__name__ + ": " + str(error))
    result["elapsed_seconds"] = round(time.monotonic() - started, 3)
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
