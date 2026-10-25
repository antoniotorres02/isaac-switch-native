#!/usr/bin/env python3
"""Send an existing probe through nxlink and require a fresh hardware receipt.

hbmenu must already be in Application mode with NetLoader active. Timeout
terminates the local nxlink client; it cannot stop or reboot a hung Switch.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import selectors
import shutil
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
PREFIX = "SWITCH_TEST "


class Receipt:
    def __init__(self, run_id: str, current_pe: bool, return_to_sphaira: bool = False,
                 graphics: bool = False, audio: bool = False):
        self.run_id = run_id
        self.current_pe = current_pe
        self.return_to_sphaira = return_to_sphaira
        self.graphics = graphics
        self.audio = audio
        self.started = False
        self.result = None

    def consume(self, line: str):
        if not line.startswith(PREFIX):
            return
        message = json.loads(line[len(PREFIX):])
        if not isinstance(message, dict):
            raise ValueError("Receipt is not an object")
        if message.get("run_id") != self.run_id:
            raise ValueError("Receipt belongs to another execution")
        event = message.get("event")
        if event == "begin" and not self.started and self.result is None:
            if message.get("result") != "RUNNING":
                raise ValueError("Invalid begin receipt")
            self.started = True
        elif event == "end" and self.started and self.result is None:
            status = message.get("result")
            if status not in {"PASS", "FAIL", "WRONG_MODE"}:
                raise ValueError("Unknown test result")
            if status == "PASS":
                # PASS_EXPECTED_IMPORT_STOP is the older import-boundary token;
                # PASS_EXPECTED_STOP covers an expected stop at any measured
                # frontier (import or untranslated guest address).
                expected_pe = ({"PASS_EXPECTED_IMPORT_STOP",
                                "PASS_EXPECTED_STOP"} if self.current_pe else
                               {"NOT_BUILT"})
                if (message.get("synthetic") != "PASS" or
                        message.get("current_pe") not in expected_pe or
                        message.get("applet_type") != 0):  # libnx AppletType_Application
                    raise ValueError("PASS disagrees with required checks or Application mode")
                if self.return_to_sphaira and message.get("return_to") != "SPHAIRA":
                    raise ValueError("PASS did not schedule the requested return to Sphaira")
                if self.graphics and message.get("graphics") != "PASS":
                    raise ValueError("PASS did not verify the requested graphics probe")
                if self.audio and message.get("audio") != "PASS":
                    raise ValueError("PASS did not verify the requested native audio probe")
            self.result = message
        else:
            raise ValueError("Duplicate or out-of-order receipt")


def run(nro: Path, address: str, nxlink: str, timeout: float,
        output: Path, current_pe: bool = False, return_to_sphaira: bool = False,
        graphics: bool = False, audio: bool = False, nro_args: str = "") -> dict:
    raw = nro.read_bytes()
    if raw[0x10:0x14] != b"NRO0":
        raise ValueError("Input is not an NRO")
    run_id = uuid.uuid4().hex
    directory = output / run_id
    directory.mkdir(parents=True, exist_ok=False)
    command = [nxlink, "-s", "-a", address, "-r", "3", str(nro.resolve()),
               "--args", f"--autotest --run-id {run_id}"]
    if return_to_sphaira:
        command[-1] += " --return-to-sphaira"
    if graphics:
        command[-1] += " --graphics"
    if nro_args:
        command[-1] += " " + nro_args
    summary = {"run_id": run_id, "address": address,
               "nro": str(nro.resolve()), "sha256": hashlib.sha256(raw).hexdigest(),
               "current_pe_required": current_pe, "command": command,
               "return_to_sphaira": return_to_sphaira,
               "graphics_required": graphics,
               "audio_required": audio,
               "process_exit_verified": False,
               "status": "TRANSPORT_ERROR"}
    receipt = Receipt(run_id, current_pe, return_to_sphaira, graphics, audio)
    started = time.monotonic()
    process = None
    pending = b""
    print(f"Sending to {address}; evidence: {directory}", flush=True)
    try:
        with (directory / "nxlink.log").open("wb") as log, selectors.DefaultSelector() as selector:
            process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, bufsize=0)
            selector.register(process.stdout, selectors.EVENT_READ)
            while True:
                remaining = timeout - (time.monotonic() - started)
                if remaining <= 0:
                    summary["status"] = "TIMEOUT"
                    break
                events = selector.select(min(remaining, 0.2))
                if not events:
                    continue
                data = process.stdout.read(65536)
                if not data:
                    if pending:
                        receipt.consume(pending.decode("utf-8", errors="replace"))
                    summary["status"] = (receipt.result["result"] if receipt.result
                                         else "INCOMPLETE" if receipt.started else "TRANSPORT_ERROR")
                    break
                log.write(data)
                log.flush()
                pending += data
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    decoded = line.decode("utf-8", errors="replace").rstrip("\r")
                    print(decoded, flush=True)
                    receipt.consume(decoded)
                if len(pending) > 1024 * 1024:
                    raise ValueError("Unterminated output exceeds 1 MiB")
    except (ValueError, OSError) as exc:
        summary["status"] = "PROTOCOL_ERROR" if isinstance(exc, ValueError) else "TRANSPORT_ERROR"
        summary["error"] = str(exc)
    finally:
        if process is not None:
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            process.stdout.close()
            summary["nxlink_exit_code"] = process.returncode
            if summary["status"] == "PASS" and process.returncode != 0:
                summary["status"] = "TRANSPORT_ERROR"
        summary["elapsed_seconds"] = round(time.monotonic() - started, 3)
        summary["receipt"] = receipt.result
        (directory / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{summary['status']}: {directory / 'result.json'}", flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True)
    parser.add_argument("--nro", type=Path, required=True)
    parser.add_argument("--nxlink", default="nxlink")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--current-pe", action="store_true", help="Require the PE loader/startup check too")
    parser.add_argument("--graphics", action="store_true", help="Require two EGL/OpenGL render/readback cycles")
    parser.add_argument("--audio", action="store_true", help="Require the compiled native OpenAL probe in the receipt")
    parser.add_argument("--return-to-sphaira", action="store_true", help="Return to the installed Sphaira NRO, without replacing hbmenu")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/switch-tests")
    parser.add_argument("--nro-args", default="", help="Extra arguments for the NRO (e.g. --mods)")
    args = parser.parse_args()
    if not 0 < args.timeout < 3600:
        parser.error("--timeout must be between 0 and 3600 seconds")
    nxlink = shutil.which(args.nxlink)
    if not nxlink:
        parser.error("nxlink not found in PATH")
    if not args.nro.is_file():
        parser.error("NRO does not exist")
    summary = run(args.nro, args.address, nxlink, args.timeout, args.output_dir, args.current_pe,
                  args.return_to_sphaira, args.graphics, args.audio, args.nro_args)
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
