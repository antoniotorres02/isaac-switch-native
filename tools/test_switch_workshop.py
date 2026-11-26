#!/usr/bin/env python3
"""Deterministic Workshop protocol, filesystem and controller tests."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "host/native"
FIXTURES = ROOT / "tools/fixtures"


class WorkshopTests(unittest.TestCase):
    def test_protocol_download_registry_and_controls(self):
        with tempfile.TemporaryDirectory(prefix="isaac-workshop-test-") as tmp:
            out = Path(tmp)
            shutil.copyfile(FIXTURES / "workshop_switch.h", out / "switch.h")
            flags = subprocess.check_output(
                ["pkg-config", "--cflags", "--libs", "libcrypto", "liblzma", "libzstd",
                 "libpng", "libjpeg", "zlib"], text=True
            ).split()
            modules = ("steamproto", "sha1", "zip", "steamworkshop", "steamcdn",
                       "modmanager", "modstate", "modsync", "workshop_browser", "workshop_media")
            subprocess.run(
                [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                 "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                 "-I", str(out), "-I", str(NATIVE), str(FIXTURES / "workshop_test.c"),
                 *(str(NATIVE / f"native_{name}.c") for name in modules), "-pthread",
                 *flags, "-o", str(out / "test")], check=True
            )
            subprocess.run([str(out / "test"), tmp], check=True, timeout=30)

    def test_workshop_entry_points(self):
        # Exercise the browser worker and controller above, then verify which
        # application lifecycle paths can call those entry points on Switch.
        switch = (NATIVE / "native_switch.c").read_text()
        launcher = switch[switch.index("static int launcher_run"):switch.index("/* ---- heap")]
        self.assertEqual(switch.count("workshop_browser_open("), 1)
        self.assertEqual(switch.count("workshop_browser_input("), 1)
        self.assertIn("else if ((down & HidNpadButton_R) && idle)", launcher)
        for name in ("native_steamauth.c", "native_steamsync.c", "native_main.c"):
            source = (NATIVE / name).read_text()
            for call in ("workshop_browser_open(", "mod_plan_apply(", "workshop_install(",
                         "workshop_set_subscription("):
                self.assertNotIn(call, source)
        browser = (NATIVE / "native_workshop_browser.c").read_text()
        self.assertIn('post(JOB_LOCAL, "Reading installed mods…")', browser)
        self.assertIn("b.confirming = JOB_APPLY", browser)
        self.assertNotIn("post(JOB_APPLY", browser)


if __name__ == "__main__":
    unittest.main()
