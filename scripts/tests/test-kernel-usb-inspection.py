"""Regression of the read-only kernel inspector against captured H2 evidence.

Pass the evidence directory as the only argument. Negative cases alter bytes
in memory through mocks; they never edit snapshots or access the device.
"""

import contextlib
import gzip
import importlib.util
import io
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


if len(sys.argv) != 2:
    raise SystemExit("usage: test-kernel-usb-inspection.py EVIDENCE_DIRECTORY")
EVIDENCE = Path(sys.argv.pop()).resolve()
SPEC = importlib.util.spec_from_file_location(
    "kernel_usb_inspector", Path(__file__).resolve().parents[1] / "probes" / "inspect-kernel-usb.py"
)
INSPECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSPECTOR)
READ_BYTES = Path.read_bytes


class InspectorTests(unittest.TestCase):
    def inspect(self, replacements=None, base="0xffffff8008080000"):
        arguments = [
            "inspect-kernel-usb.py",
            str(EVIDENCE / "brickpro-boot.img"),
            str(EVIDENCE / "brickpro-kallsyms.txt"),
            str(EVIDENCE / "brickpro-config.gz"),
            "--base", base,
            "--functions", "usb_gadget_ep_match_desc",
            "--live-code", str(EVIDENCE / "live-epmatch-code.bin"),
            "--live-endpoints", str(EVIDENCE / "live-endpoints.bin"),
            "--gadget-base", "0xffffff8008bab630",
            "--live-gadget-ops", str(EVIDENCE / "live-gadget-ops.bin"),
            "--live-match-ep", str(EVIDENCE / "live-match-ep.bin"),
        ]
        replacements = replacements or {}

        def read_bytes(path):
            return replacements[path.name] if path.name in replacements else READ_BYTES(path)

        output = io.StringIO()
        with patch.object(sys, "argv", arguments), patch.object(Path, "read_bytes", read_bytes), contextlib.redirect_stdout(output):
            INSPECTOR.main()
        return output.getvalue()

    def changed(self, name, offset):
        data = bytearray(READ_BYTES(EVIDENCE / name))
        data[offset] ^= 1
        return {name: bytes(data)}

    def test_verified_snapshots(self):
        output = self.inspect()
        self.assertIn("live_ep_list_allocation_count=6", output)
        self.assertIn("live_gadget_ops_and_match_ep_match_boot=true", output)
        self.assertIn("ep4-int: caps=0x38 types=interrupt dir=IN|OUT", output)
        self.assertIn("ep1in-bulk: caps=0x14 types=bulk dir=IN", output)

    def test_boot_magic_rejected(self):
        with self.assertRaisesRegex(SystemExit, "Android boot"):
            self.inspect(self.changed("brickpro-boot.img", 0))

    def test_truncated_boot_rejected(self):
        with self.assertRaisesRegex(SystemExit, "bounds"):
            self.inspect({"brickpro-boot.img": READ_BYTES(EVIDENCE / "brickpro-boot.img")[:2048]})

    def test_config_mismatch_rejected(self):
        with self.assertRaisesRegex(SystemExit, "does not match"):
            self.inspect({"brickpro-config.gz": gzip.compress(b"# another firmware\n")})

    def test_guessed_base_rejected(self):
        with self.assertRaisesRegex(SystemExit, "_stext mapping"):
            self.inspect(base="0xffffff8008081000")

    def test_live_code_mismatch_rejected(self):
        with self.assertRaisesRegex(SystemExit, "matching code"):
            self.inspect(self.changed("live-epmatch-code.bin", 0))

    def test_live_endpoint_caps_mismatch_rejected(self):
        with self.assertRaisesRegex(SystemExit, "capabilities changed"):
            self.inspect(self.changed("live-endpoints.bin", 0x28))

    def test_live_endpoint_list_change_rejected(self):
        with self.assertRaisesRegex(SystemExit, "list is incomplete"):
            self.inspect(self.changed("live-endpoints.bin", 0x88 + 0x18))

    def test_match_callback_mismatch_rejected(self):
        with self.assertRaisesRegex(SystemExit, "operations/callback"):
            self.inspect(self.changed("live-match-ep.bin", 0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
