"""Verify transport failure handling without contacting a Switch."""
import socket
import threading
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

from switch_botbase import Botbase, ProtocolError, decode_capture


class TransportTests(unittest.TestCase):
    def setUp(self):
        host, peer = socket.socketpair()
        self.peer = peer
        self.bot = Botbase.__new__(Botbase)
        self.bot.sock = host
        self.bot.timeout = 0.1
        self.bot.expected_version = "2.5"
        self.bot.pending = bytearray()

    def tearDown(self):
        self.bot.sock.close()
        self.peer.close()

    def test_fragmented_reply_preserves_following_line(self):
        self.peer.sendall(b"2.")
        def finish():
            self.peer.sendall(b"5\r\nnext\n")
        thread = threading.Thread(target=finish)
        thread.start()
        self.assertEqual(self.bot.line(), b"2.5")
        self.assertEqual(self.bot.line(), b"next")
        thread.join()

    def test_eof_before_newline_is_not_success(self):
        self.peer.sendall(b"2.5")
        self.peer.shutdown(socket.SHUT_WR)
        with self.assertRaises(ProtocolError):
            self.bot.line()

    def test_reply_timeout_is_bounded(self):
        with self.assertRaises(TimeoutError):
            self.bot.line()

    def test_unterminated_large_reply_is_rejected(self):
        self.bot.pending = bytearray(b"x" * (2 * 1024 * 1024))
        self.peer.sendall(b"x")
        with self.assertRaises(ProtocolError):
            self.bot.line()

    def test_input_service_error_cannot_be_acknowledged_as_success(self):
        self.peer.sendall(b"hiddbgSetHdlsState: 123\n2.5\n")
        with self.assertRaisesRegex(ProtocolError, "hiddbgSetHdlsState"):
            self.bot.sync()
        self.assertEqual(self.peer.recv(1024), b"getVersion\r\n")

    def test_command_injection_is_rejected_before_sending(self):
        with self.assertRaises(ValueError):
            self.bot.send("click A\nclick HOME")

    def test_unsupported_button_is_rejected(self):
        with self.assertRaises(ValueError):
            self.bot.click("TYPO")
        with self.assertRaises(ValueError):
            with self.bot.held("TYPO"):
                pass

    def test_held_button_is_released_when_route_fails(self):
        sent = []
        self.bot.send = sent.append
        self.bot.prepare_controller = lambda: None
        self.bot.sync = lambda: "2.5"
        with self.assertRaisesRegex(RuntimeError, "route failed"):
            with self.bot.held("R"):
                raise RuntimeError("route failed")
        self.assertEqual(sent, ["press R", "release R"])


class CaptureTests(unittest.TestCase):
    def test_jpeg_framing(self):
        self.assertEqual(decode_capture(b"FFD8616263FFD9"), b"\xff\xd8abc\xff\xd9")

    def test_failed_and_truncated_capture_is_rejected(self):
        for reply in (b"", b"capssc, 1204: 420046", b"FFD8", b"FFD8GGFFD9",
                      b"FFD861FFD", b"89504E47"):
            with self.subTest(reply=reply), self.assertRaises(ProtocolError):
                decode_capture(reply)


class OwnershipTests(unittest.TestCase):
    def test_second_controller_is_rejected_and_close_releases_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch("switch_botbase.ROOT", Path(directory)), \
                    patch("switch_botbase.socket.create_connection") as connect:
                bot = Botbase("192.0.2.1")
                with self.assertRaises(BlockingIOError):
                    Botbase("192.0.2.1")
                self.assertEqual(connect.call_count, 1)
                bot.close()
                successor = Botbase("192.0.2.1")
                successor.close()
                self.assertEqual(connect.call_count, 2)


if __name__ == "__main__":
    unittest.main()
