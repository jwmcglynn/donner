"""Contracts for paired native delivery diagnostics and lossless unwinding."""

from pathlib import Path
import os
import tempfile
import unittest
from unittest import mock

from tools import native_delivery as delivery


UUID = "12345678-1234-1234-1234-123456789abc"


class NativeDeliveryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.binary = self.root / "source"
        self.binary.write_bytes(b"linked executable")
        self.output = self.root / "delivery"
        self.symbols = self.root / "symbols"
        self.dsym = self.root / "source.dSYM"
        dwarf = self.dsym / "Contents/Resources/DWARF/source"
        dwarf.parent.mkdir(parents=True)
        dwarf.write_bytes(b"paired line tables")
        (self.dsym / "Contents/Info.plist").write_bytes(b"debug metadata")

    def _mach_tool(self, *arguments):
        if arguments[0].endswith("dwarfdump"):
            return f"UUID: {UUID} (arm64) executable\n" if "--uuid" in arguments else "verified\n"
        if arguments[0].endswith("strip"):
            Path(arguments[-1]).write_bytes(b"stripped executable")
        return ""

    def test_macos_strips_fresh_copy_then_signs_with_matching_dsym(self):
        with mock.patch.object(delivery, "_tool", side_effect=self._mach_tool) as tool, \
             mock.patch.object(delivery, "_mach_unwind", return_value={"__eh_frame": "a" * 64}):
            result = delivery.prepare(binary=self.binary, output=self.output,
                                      symbols=self.symbols, platform="darwin-arm64",
                                      dsym=self.dsym, sign=True)
        self.assertEqual(self.binary.read_bytes(), b"linked executable")
        self.assertEqual(self.output.read_bytes(), b"stripped executable")
        self.assertEqual(result["identity"], UUID)
        self.assertEqual(result["binary_sha256"], delivery._digest(self.output))
        self.assertEqual(result["symbol_sha256"], delivery._digest(
            self.symbols / result["symbol_file"]))
        commands = [call.args for call in tool.call_args_list]
        strip_index = next(i for i, command in enumerate(commands) if command[0].endswith("strip"))
        sign_index = next(i for i, command in enumerate(commands) if "--force" in command)
        self.assertLess(strip_index, sign_index)
        self.assertTrue((self.symbols / "delivery.json").is_file())

    def test_changed_unwind_or_uuid_fails_closed(self):
        for mismatch in ("unwind", "identity"):
            with self.subTest(mismatch=mismatch), tempfile.TemporaryDirectory() as temporary:
                destination = Path(temporary)
                with mock.patch.object(delivery, "_tool", side_effect=self._mach_tool), \
                     mock.patch.object(delivery, "_uuid", side_effect=[
                         UUID, UUID, "f" * 36 if mismatch == "identity" else UUID]), \
                     mock.patch.object(delivery, "_mach_unwind", side_effect=[
                         {"__eh_frame": "a"}, {"__eh_frame": "b" if mismatch == "unwind" else "a"}]):
                    with self.assertRaisesRegex(ValueError, "identity or unwind"):
                        delivery.prepare(binary=self.binary, output=destination / "delivery",
                                         symbols=destination / "symbols", platform="darwin-arm64",
                                         dsym=self.dsym)
                self.assertFalse((destination / "symbols/delivery.json").exists())

    def test_missing_mismatched_or_linked_dsym_fails_closed(self):
        with mock.patch.object(delivery, "_tool", side_effect=self._mach_tool), \
             mock.patch.object(delivery, "_mach_unwind", return_value={"__eh_frame": "a"}):
            with self.assertRaisesRegex(ValueError, "dSYM"):
                delivery.prepare(binary=self.binary, output=self.output, symbols=self.symbols,
                                 platform="darwin-arm64")
        self.assertFalse((self.symbols / "delivery.json").exists())

    def test_linux_retains_debug_file_and_debuglink_with_same_build_id(self):
        def tool(*arguments):
            if "--only-keep-debug" in arguments:
                Path(arguments[-1]).write_bytes(b"paired ELF symbols")
            if "--strip-unneeded" in arguments:
                Path(arguments[-1]).write_bytes(b"stripped ELF")
            return ""
        with mock.patch.object(delivery, "_tool", side_effect=tool) as run, \
             mock.patch.object(delivery, "_build_id", return_value="a" * 40), \
             mock.patch.object(delivery, "_elf_unwind", return_value={".eh_frame": "b" * 64}):
            result = delivery.prepare(binary=self.binary, output=self.output,
                                      symbols=self.symbols, platform="linux-x86-64")
        self.assertEqual(self.binary.read_bytes(), b"linked executable")
        self.assertEqual(result["identity"], "a" * 40)
        self.assertTrue(any(call.args[1].startswith("--add-gnu-debuglink=")
                            for call in run.call_args_list))

    def test_public_or_absent_symbol_evidence_directory_fails_before_copy(self):
        evidence = self.root / "public"
        evidence.mkdir(mode=0o755)
        for parent in (evidence, self.root / "missing"):
            with self.subTest(parent=parent), self.assertRaisesRegex(ValueError, "private evidence"):
                delivery.prepare(binary=self.binary, output=self.output, symbols=parent / "symbols",
                                 platform="linux-x86-64")
            self.assertFalse(self.output.exists())

    def test_dangling_output_symlink_is_rejected_before_mutation(self):
        victim = self.root / "missing-victim"
        self.output.symlink_to(victim)
        with self.assertRaisesRegex(ValueError, "fresh owned output"):
            delivery.prepare(binary=self.binary, output=self.output, symbols=self.symbols,
                             platform="linux-x86-64")
        self.assertFalse(victim.exists())
        self.assertFalse(self.symbols.exists())
        self.assertTrue(self.output.is_symlink())

    def test_exclusive_output_copy_refuses_links_inserted_after_preflight(self):
        victim = self.root / "preserved-victim"
        victim.write_bytes(b"preserve existing bytes")
        self.output.symlink_to(victim)
        with mock.patch.object(delivery.os, "open", wraps=os.open) as opened:
            with self.assertRaises(FileExistsError):
                delivery._copy_fresh_executable(self.binary, self.output)
        flags = opened.call_args.args[1]
        self.assertTrue(flags & os.O_EXCL)
        self.assertTrue(flags & os.O_NOFOLLOW)
        self.assertEqual(victim.read_bytes(), b"preserve existing bytes")
        self.assertEqual(self.binary.read_bytes(), b"linked executable")

    def test_mach_section_data_uses_file_offsets_not_virtual_addresses(self):
        self.binary.write_bytes(b"header__unwind__tail")
        load_commands = "Section\n  sectname __eh_frame\n  size 0x8\n  offset 6\n"
        with mock.patch.object(delivery, "_tool", return_value=load_commands):
            self.assertEqual(delivery._mach_unwind(self.binary),
                             {"__eh_frame": delivery.sha256(b"__unwind").hexdigest()})
        with mock.patch.object(delivery, "_tool", return_value=""):
            with self.assertRaisesRegex(ValueError, "no unwind"):
                delivery._mach_unwind(self.binary)

    def test_elf_inspection_always_writes_a_separate_output_image(self):
        def tool(*arguments):
            if "-SW" in arguments:
                return ".eh_frame PROGBITS\n"
            name = arguments[2].split("=", 1)[1]
            Path(name).write_bytes(b"unwind")
            return ""
        with mock.patch.object(delivery, "_tool", side_effect=tool) as run:
            self.assertEqual(delivery._elf_unwind(self.binary),
                             {".eh_frame": delivery.sha256(b"unwind").hexdigest()})
        self.assertEqual(run.call_args_list[-1].args[-2], str(self.binary))
        self.assertNotEqual(run.call_args_list[-1].args[-1], str(self.binary))


if __name__ == "__main__":
    unittest.main()
