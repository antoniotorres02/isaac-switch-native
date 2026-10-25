#!/usr/bin/env python3
"""Exercise false-PASS rejection and real subprocess timeout/receipt handling."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from run_switch_tests import PREFIX, Receipt, run


class ReceiptTests(unittest.TestCase):
    def message(self, event, **changes):
        data = dict(event=event, run_id="a" * 32, result="RUNNING" if event == "begin" else "PASS",
                    synthetic="PASS", current_pe="NOT_BUILT", applet_type=0)
        data.update(changes)
        return PREFIX + json.dumps(data)

    def test_fresh_pass_requires_begin(self):
        receipt = Receipt("a" * 32, False)
        with self.assertRaises(ValueError):
            receipt.consume(self.message("end"))
        receipt.consume(self.message("begin"))
        receipt.consume(self.message("end"))
        self.assertEqual(receipt.result["result"], "PASS")
        with self.assertRaises(ValueError):
            receipt.consume(self.message("end"))

    def test_reject_false_pass(self):
        for changes in ({"run_id": "b" * 32}, {"synthetic": "FAIL"},
                        {"applet_type": 1}, {"current_pe": "NOT_RUN"},
                        {"result": "UNKNOWN"}):
            with self.subTest(changes=changes):
                receipt = Receipt("a" * 32, False)
                receipt.consume(self.message("begin"))
                with self.assertRaises(ValueError):
                    receipt.consume(self.message("end", **changes))

    def test_pe_cannot_be_silently_skipped(self):
        receipt = Receipt("a" * 32, True)
        receipt.consume(self.message("begin"))
        with self.assertRaises(ValueError):
            receipt.consume(self.message("end"))
        receipt.consume(self.message("end", current_pe="PASS_EXPECTED_IMPORT_STOP"))
        self.assertEqual(receipt.result["result"], "PASS")

    def test_expected_stop_token_covers_non_import_frontiers(self):
        receipt = Receipt("a" * 32, True)
        receipt.consume(self.message("begin"))
        receipt.consume(self.message("end", current_pe="PASS_EXPECTED_STOP"))
        self.assertEqual(receipt.result["result"], "PASS")
        # Without --current-pe only NOT_BUILT is acceptable, so a PE token
        # cannot satisfy a synthetic-only requirement.
        receipt = Receipt("a" * 32, False)
        receipt.consume(self.message("begin"))
        with self.assertRaises(ValueError):
            receipt.consume(self.message("end", current_pe="PASS_EXPECTED_STOP"))
        receipt = Receipt("a" * 32, False)
        receipt.consume(self.message("begin"))
        receipt.consume(self.message("end", current_pe="NOT_BUILT"))
        self.assertEqual(receipt.result["result"], "PASS")

    def test_failure_and_wrong_mode_are_results(self):
        for status in ("FAIL", "WRONG_MODE"):
            receipt = Receipt("a" * 32, False)
            receipt.consume(self.message("begin"))
            receipt.consume(self.message("end", result=status, synthetic="NOT_RUN"))
            self.assertEqual(receipt.result["result"], status)

    def test_return_to_sphaira_must_be_acknowledged(self):
        receipt = Receipt("a" * 32, False, True)
        receipt.consume(self.message("begin"))
        with self.assertRaises(ValueError):
            receipt.consume(self.message("end"))
        receipt.consume(self.message("end", return_to="SPHAIRA"))
        self.assertEqual(receipt.result["result"], "PASS")

    def test_graphics_cannot_be_silently_skipped(self):
        for value in (None, "NOT_BUILT", "NOT_RUN", "FAIL"):
            with self.subTest(graphics=value):
                receipt = Receipt("a" * 32, False, graphics=True)
                receipt.consume(self.message("begin"))
                with self.assertRaises(ValueError):
                    receipt.consume(self.message("end", graphics=value))
        receipt = Receipt("a" * 32, False, graphics=True)
        receipt.consume(self.message("begin"))
        receipt.consume(self.message("end", graphics="PASS"))
        self.assertEqual(receipt.result["graphics"], "PASS")

    def test_audio_cannot_be_silently_skipped(self):
        for value in (None, "NOT_BUILT", "NOT_RUN", "FAIL"):
            with self.subTest(audio=value):
                receipt = Receipt("a" * 32, False, audio=True)
                receipt.consume(self.message("begin"))
                with self.assertRaises(ValueError):
                    receipt.consume(self.message("end", audio=value))
        receipt = Receipt("a" * 32, False, audio=True)
        receipt.consume(self.message("begin"))
        receipt.consume(self.message("end", audio="PASS"))
        self.assertEqual(receipt.result["audio"], "PASS")

    def test_older_receipt_without_audio_remains_supported(self):
        receipt = Receipt("a" * 32, False, audio=False)
        receipt.consume(self.message("begin"))
        receipt.consume(self.message("end"))
        self.assertEqual(receipt.result["result"], "PASS")

    def test_subprocesses(self):
        for mode, expected in (("pass", "PASS"), ("incomplete", "INCOMPLETE"),
                               ("silent", "TRANSPORT_ERROR"), ("timeout", "TIMEOUT"),
                               ("stale", "PROTOCOL_ERROR"), ("bad_exit", "TRANSPORT_ERROR")):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                nro = root / "probe.nro"
                nro.write_bytes(b"\0" * 16 + b"NRO0")
                fake = root / "nxlink"
                fake.write_text('''#!/usr/bin/env python3
import json, sys, time
mode = MODE
run_id = sys.argv[-1].split()[-1]
def emit(event, result):
    print('SWITCH_TEST ' + json.dumps(dict(event=event, result=result,
          run_id=run_id, synthetic='PASS', current_pe='NOT_BUILT', applet_type=0)), flush=True)
if mode == 'silent': sys.exit(0)
if mode == 'stale': run_id = 'old'
emit('begin', 'RUNNING')
if mode == 'timeout': time.sleep(20)
if mode == 'incomplete': sys.exit(0)
emit('end', 'PASS')
sys.exit(1 if mode == 'bad_exit' else 0)
'''.replace("MODE", repr(mode)))
                fake.chmod(0o755)
                with contextlib.redirect_stdout(io.StringIO()):
                    summary = run(nro, "127.0.0.1", str(fake), 0.5, root / "results")
                self.assertEqual(summary["status"], expected)
                saved = json.loads(next((root / "results").glob("*/result.json")).read_text())
                self.assertEqual(saved, summary)
                self.assertLess(summary["elapsed_seconds"], 5)


if __name__ == "__main__":
    unittest.main()
