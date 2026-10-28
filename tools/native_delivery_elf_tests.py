"""Inspect real ELF source-line companions without executing the fixture."""

from pathlib import Path
import os
import shutil
import stat
import subprocess
import tempfile
import unittest

from tools import native_delivery as delivery


class NativeDeliveryElfTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        supplied = os.environ.get("NATIVE_DELIVERY_ELF_FIXTURE")
        if supplied:
            self.fixture = Path(supplied)
        else:
            from python.runfiles import runfiles
            self.fixture = Path(runfiles.Create().Rlocation(
                "donner/tools/native_delivery_elf_fixture"))
        self.binary = self.root / "input"
        shutil.copyfile(self.fixture, self.binary)
        self.original = self.binary.read_bytes()
        self.output = self.root / "delivery"
        self.symbols = self.root / "symbols"

    def test_debug_bearing_pair_preserves_source_and_usable_lines(self):
        result = delivery.prepare(binary=self.binary, output=self.output,
                                  symbols=self.symbols, platform="linux-x86-64")
        debug = self.symbols / result["symbol_file"]
        lines = subprocess.check_output(["readelf", "--debug-dump=decodedline", str(debug)])
        self.assertIn(b"native_delivery_elf_fixture.cc", lines)
        self.assertEqual(delivery._build_id(debug), delivery._build_id(self.output))
        self.assertEqual(self.binary.read_bytes(), self.original)
        self.assertEqual(stat.S_IMODE(self.output.stat().st_mode), 0o755)
        self.assertEqual(stat.S_IMODE(self.symbols.stat().st_mode), 0o700)
        self.assertTrue((self.symbols / "delivery.json").is_file())

    def _check_stripped_rejected(self, flag):
        subprocess.run(["strip", flag, str(self.binary)], check=True, timeout=30)
        stripped = self.binary.read_bytes()
        with self.assertRaisesRegex(ValueError, "usable DWARF source lines"):
            delivery.prepare(binary=self.binary, output=self.output,
                             symbols=self.symbols, platform="linux-x86-64")
        self.assertEqual(self.binary.read_bytes(), stripped)
        self.assertEqual(self.output.read_bytes(), stripped)
        self.assertFalse((self.symbols / "delivery.json").exists())

    def test_strip_debug_input_rejected(self):
        self._check_stripped_rejected("--strip-debug")

    def test_strip_all_input_rejected(self):
        self._check_stripped_rejected("--strip-all")


if __name__ == "__main__":
    unittest.main()
