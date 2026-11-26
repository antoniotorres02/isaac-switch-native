#!/usr/bin/env python3
"""Verify the Switch options editor against a synthetic save directory."""
import ctypes
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OPTIONS = Path("documents/my games/binding of isaac repentance+/options.ini")
EFFECTS = Path("resources/config.ini")


class Menu(ctypes.Structure):
    _fields_ = [("values", ctypes.c_int * 9), ("changed", ctypes.c_uint),
                ("selected", ctypes.c_int), ("status", ctypes.c_char * 96)]


class SwitchOptionsTests(unittest.TestCase):
    def test_defaults_existing_options_and_graphics_override(self):
        with tempfile.TemporaryDirectory(prefix="isaac-switch-options-") as tmp:
            root = Path(tmp)
            library = root / "options.so"
            subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200112L",
                            "-Wall", "-Wextra",
                            "-Werror", "-D__SWITCH__", "-shared", "-fPIC",
                            ROOT / "host/native/native_options.c", "-o", library], check=True)
            native = ctypes.CDLL(str(library))
            for method in (native.isaac_options_read, native.isaac_options_write):
                method.argtypes = [ctypes.POINTER(Menu), ctypes.c_char_p, ctypes.c_char_p]
                method.restype = ctypes.c_int
            saves, guest = root / "saves", root / "guest"
            menu = Menu()
            args = (ctypes.byref(menu), os.fsencode(saves), os.fsencode(guest))
            self.assertEqual(native.isaac_options_read(*args), 0)
            self.assertEqual(list(menu.values), [2, 1, 1, 1, 1, 1, 1, 1, 1])
            settings = root / "settings.ini"
            settings.write_bytes(b"# controller\nframe_skip=1\na_accepts=1")
            native.isaac_port_setting.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
            self.assertEqual(native.isaac_port_setting(os.fsencode(root), b"frame_skip", 2), 0)
            self.assertEqual(settings.read_bytes(), b"# controller\nframe_skip=2\na_accepts=1")
            self.assertFalse(saves.exists())
            menu.values[1] = 0
            menu.changed = 1 << 1
            self.assertEqual(native.isaac_options_write(*args), 0)
            options = saves / OPTIONS
            self.assertIn(b"MaxRenderScale=2\r\n", options.read_bytes())
            self.assertIn(b"VSync=0\r\n", options.read_bytes())
            self.assertIn(b"Fullscreen=0\r\n", options.read_bytes())

            menu.values[3] = 1
            menu.changed = 1 << 3
            self.assertEqual(native.isaac_options_write(*args), 0)
            new_graphics = saves / EFFECTS
            self.assertIn(b"EnableCaustics=1\r\n", new_graphics.read_bytes())
            self.assertIn(b"EnableBloom=1\r\n", new_graphics.read_bytes())
            self.assertIn(b"EnableWaterSurface=1\r\n", new_graphics.read_bytes())
            new_graphics.unlink()

            long_comment = b"; " + b"long comment " * 512 + b"\r\n"
            options.write_bytes(b"[Options]\r\n; comment\r\nMaxRenderScale = 5\r\nMusicVolume=0.4\r\n"
                                + long_comment + b"[Other]\r\nMaxRenderScale=37")
            graphics = guest / EFFECTS
            graphics.parent.mkdir(parents=True)
            graphics.write_text("[Options]\nEnableBloom=1\nExistingEffect=12\n")
            self.assertEqual(native.isaac_options_read(*args), 0)
            self.assertEqual(menu.values[0], 5)
            self.assertEqual(menu.values[4], 1)
            self.assertEqual(menu.changed, 0)
            native.isaac_options_defaults(ctypes.byref(menu))
            self.assertEqual(native.isaac_options_write(*args), 0)
            self.assertFalse(menu.changed)
            self.assertIn(b"MaxRenderScale=2\r\n", options.read_bytes())
            self.assertIn(b"; comment\r\n", options.read_bytes())
            self.assertIn(long_comment, options.read_bytes())
            self.assertIn(b"MusicVolume=0.4\r\n", options.read_bytes())
            self.assertTrue(options.read_bytes().endswith(b"[Other]\r\nMaxRenderScale=37"))
            self.assertIn("EnableBloom=1", (saves / EFFECTS).read_text())
            self.assertIn("ExistingEffect=12", (saves / EFFECTS).read_text())
            self.assertIn("EnableBloom=1", graphics.read_text())
            self.assertEqual(native.isaac_options_read(*args), 0)
            self.assertEqual(list(menu.values), [2, 1, 1, 1, 1, 1, 1, 1, 1])
            backup = options.with_name(options.name + ".tmp-backup")
            options.rename(backup)
            native.isaac_options_recover.argtypes = [ctypes.c_char_p]
            native.isaac_options_recover.restype = None
            native.isaac_options_recover(os.fsencode(saves))
            self.assertTrue(options.is_file())
            self.assertFalse(list(saves.rglob("*.tmp*")))


if __name__ == "__main__":
    unittest.main()
