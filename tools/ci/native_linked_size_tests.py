#!/usr/bin/env python3
"""Unit tests for the native linked-size measurement, over synthetic executables."""

from __future__ import annotations

import ast
import contextlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import native_linked_size as size  # noqa: E402  (sibling module, path set just above)
from tools import bcr_preflight

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_DYNAMIC = 6
SHT_NOTE = 7
SHT_NOBITS = 8
SHT_DYNSYM = 11
SHT_INIT_ARRAY = 14
SHT_FINI_ARRAY = 15
SHT_PREINIT_ARRAY = 16
SHT_RELR = 19
SHT_GNU_HASH = 0x6FFFFFF6
SHT_GNU_VERSYM = 0x6FFFFFFF
SHT_UNWIND = 0x70000001
LC_UUID = 0x1B


def elf(sections: list[tuple[int, int, int]], extended_count: bool = False) -> bytes:
    """A little-endian ELF64 file whose section headers carry (type, flags, size) triples."""
    header = bytearray(64)
    header[:4] = b"\x7fELF"
    header[4] = 2  # 64-bit
    header[5] = 1  # little-endian
    headers = [bytes(64)]  # The null section.
    for section_type, flags, section_size in sections:
        headers.append(struct.pack("<IIQQQQIIQQ", 0, section_type, flags, 0, 0, section_size, 0,
                                   0, 1, 0))
    count = len(headers)
    struct.pack_into("<Q", header, 0x28, len(header))
    struct.pack_into("<HH", header, 0x3A, 64, 0 if extended_count else count)
    if extended_count:
        headers[0] = struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, count, 0, 0, 0, 0)
    return bytes(header) + b"".join(headers)


def macho(segments: list[tuple[str, list[tuple[int, int]]] | None]) -> bytes:
    """A thin Mach-O 64 file whose segments carry (section flags, size) pairs.

    A None entry is an LC_UUID command, which is not a segment.
    """
    commands = b""
    for segment in segments:
        if segment is None:
            commands += struct.pack("<II16s", LC_UUID, 24, bytes(16))
            continue
        name, sections = segment
        body = b"".join(
            struct.pack("<16s16sQQIIIIIIII", b"__s", name.encode(), 0, section_size, 0, 0, 0, 0,
                        flags, 0, 0, 0)
            for flags, section_size in sections
        )
        commands += struct.pack("<II16sQQQQiiII", 0x19, 72 + len(body), name.encode(), 0, 0, 0, 0,
                                0, 0, len(sections), 0) + body
    header = struct.pack("<IiiIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, len(segments), len(commands),
                         0, 0)
    return header + commands


class ShippedConfigurationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = Path(__file__).with_name("shipped_product.bzl").read_text(encoding="utf-8")
        layout = bcr_preflight.Layout("/temp")
        cls.plans = {
            platform: [command.argv for command in bcr_preflight.cli_plan(
                layout, platform, "0" * 40, "1", "1") if command.argv[0] == "bazelisk"]
            for platform in ("linux-x86-64", "darwin-arm64")
        }

    @staticmethod
    def _read_transition(source):
        # The transition body uses Python-compatible Starlark; evaluate only that pure function.
        tree = ast.parse(source)
        function = next(statement for statement in tree.body
                        if isinstance(statement, ast.FunctionDef)
                        and statement.name == "_shipped_configuration_impl")
        namespace = {}
        exec(compile(ast.Module(body=[function], type_ignores=[]), "shipped_product.bzl", "exec"),
             namespace)
        declaration = next(statement.value for statement in tree.body
                           if isinstance(statement, ast.Assign)
                           and any(isinstance(target, ast.Name)
                                   and target.id == "_shipped_configuration"
                                   for target in statement.targets))
        attributes = {item.arg: item.value for item in declaration.keywords}
        return namespace[function.name], attributes

    def _assert_release_optimization(self, source, plans):
        transition, attributes = self._read_transition(source)
        self.assertEqual(attributes["implementation"].id, transition.__name__)
        copt = "//command_line_option:copt"
        mode = "//command_line_option:compilation_mode"
        self.assertIn(copt, ast.literal_eval(attributes["inputs"]))
        self.assertIn(copt, ast.literal_eval(attributes["outputs"]))
        self.assertIn(mode, ast.literal_eval(attributes["outputs"]))
        inherited = ["-DKEEP_CALLER_SETTING=1", "-gline-tables-only", "-O2"]
        result = transition({copt: inherited}, None)
        self.assertEqual(result[copt], [*inherited, "-Os"])
        self.assertEqual(inherited, ["-DKEEP_CALLER_SETTING=1", "-gline-tables-only", "-O2"])
        self.assertEqual(result[mode], "opt")
        for platform, commands in plans.items():
            self.assertEqual(len(commands), 6 if platform == "darwin-arm64" else 3)
            for index, command in enumerate(commands):
                phase = f"{platform} phase {index}"
                self.assertEqual(command[command.index("-c") + 1], result[mode], phase)
                copts = [argument.removeprefix("--copt=") for argument in command
                         if argument.startswith("--copt=")]
                self.assertEqual(copts, [result[copt][-1]], phase)

    def test_measurement_matches_every_native_build_and_notice_configuration(self):
        self._assert_release_optimization(self.source, self.plans)

    def test_contract_rejects_lost_copts_and_ineffective_size_optimization(self):
        original = 'settings["//command_line_option:copt"] + ["-Os"]'
        self.assertIn(original, self.source)
        mutations = {
            "lost caller flags": self.source.replace(original, '["-Os"]', 1),
            "caller optimization wins": self.source.replace(
                original, '["-Os"] + settings["//command_line_option:copt"]', 1),
            "missing optimization": self.source.replace(
                original, 'settings["//command_line_option:copt"]', 1),
            "different implementation": self.source.replace(
                'implementation = _shipped_configuration_impl', 'implementation = other', 1),
            "undeclared input": self.source.replace(
                'inputs = ["//command_line_option:copt"]', 'inputs = []', 1),
            "undeclared output": self.source.replace(
                '        "//command_line_option:copt",\n', '', 1),
        }
        for name, source in mutations.items():
            with self.subTest(mutation=name), self.assertRaises(AssertionError):
                self._assert_release_optimization(source, self.plans)

    def test_contract_rejects_each_producer_phase_losing_size_optimization(self):
        for platform, commands in self.plans.items():
            for index in range(len(commands)):
                for replacement in ((), ("--copt=-Os", "--copt=-O2")):
                    changed = list(commands)
                    changed[index] = tuple(argument for argument in changed[index]
                                           if argument != "--copt=-Os") + replacement
                    plans = {**self.plans, platform: changed}
                    with self.subTest(platform=platform, phase=index, replacement=replacement):
                        with self.assertRaises(AssertionError):
                            self._assert_release_optimization(self.source, plans)


class MeasureElfTest(unittest.TestCase):
    def test_counts_allocated_file_backed_sections_by_writability(self):
        measured = size.measure_elf(elf([
            (SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 1000),  # .text
            (SHT_PROGBITS, SHF_ALLOC, 200),                    # .rodata
            (SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 30),         # .data
            (SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 4000),         # .bss, zero-filled
            (SHT_SYMTAB, 0, 5000),                             # not loaded
            (SHT_PROGBITS, 0, 7000),                           # debug information
        ]))
        self.assertEqual(measured, size.LinkedSize(read_only=1200, writable=30))
        self.assertEqual(measured.total, 1230)

    def test_excludes_dynamic_linking_metadata_and_notes(self):
        # Mach-O keeps the equivalent of each excluded section in __LINKEDIT, which is not
        # counted either, so the two formats measure the same program.
        measured = size.measure_elf(elf([
            (SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 1000),  # .text
            (SHT_UNWIND, SHF_ALLOC, 70),                       # .eh_frame on x86-64
            (SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE, 8),        # .init_array
            (SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE, 16),       # .fini_array
            (SHT_PREINIT_ARRAY, SHF_ALLOC | SHF_WRITE, 32),    # .preinit_array
            (SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, 800),         # .dynamic
            (SHT_DYNSYM, SHF_ALLOC, 100),                      # .dynsym
            (SHT_STRTAB, SHF_ALLOC, 200),                      # .dynstr
            (SHT_GNU_HASH, SHF_ALLOC, 300),                    # .gnu.hash
            (SHT_GNU_VERSYM, SHF_ALLOC, 400),                  # .gnu.version
            (SHT_RELA, SHF_ALLOC, 500),                        # .rela.dyn
            (SHT_RELR, SHF_ALLOC, 600),                        # .relr.dyn
            (SHT_NOTE, SHF_ALLOC, 700),                        # .note.gnu.build-id
        ]))
        self.assertEqual(measured, size.LinkedSize(read_only=1070, writable=56))

    def test_reads_an_extended_section_count(self):
        sections = [(SHT_PROGBITS, SHF_ALLOC, 10)] * 3
        self.assertEqual(size.measure_elf(elf(sections, extended_count=True)),
                         size.LinkedSize(read_only=30, writable=0))

    def test_refuses_a_32_bit_or_truncated_file(self):
        thirty_two_bit = bytearray(elf([]))
        thirty_two_bit[4] = 1
        with self.assertRaisesRegex(ValueError, "64-bit little-endian"):
            size.measure_elf(bytes(thirty_two_bit))
        with self.assertRaisesRegex(ValueError, "truncated"):
            size.measure_elf(elf([(SHT_PROGBITS, SHF_ALLOC, 10)])[:100])

    def test_refuses_missing_or_unexpected_section_headers(self):
        no_headers = bytearray(elf([]))
        struct.pack_into("<Q", no_headers, 0x28, 0)
        with self.assertRaisesRegex(ValueError, "no section headers"):
            size.measure_elf(bytes(no_headers))
        wrong_size = bytearray(elf([]))
        struct.pack_into("<H", wrong_size, 0x3A, 40)
        with self.assertRaisesRegex(ValueError, "section header size 40"):
            size.measure_elf(bytes(wrong_size))


class MeasureMachOTest(unittest.TestCase):
    def test_counts_text_and_data_segment_sections_except_zero_fill(self):
        measured = size.measure_macho(macho([
            ("__PAGEZERO", []),
            ("__TEXT", [(0x80000400, 900), (0x0, 100)]),        # __text, __const
            ("__DATA_CONST", [(0x0, 40)]),
            ("__DATA", [(0x0, 20), (0x1, 3000), (0xC, 500)]),  # __data, __bss, __common
            ("__LINKEDIT", []),
            ("__DWARF", [(0x0, 9000)]),
        ]))
        self.assertEqual(measured, size.LinkedSize(read_only=1000, writable=60))

    def test_skips_other_load_commands_and_every_zero_fill_type(self):
        measured = size.measure_macho(macho([
            ("__TEXT", [(0x0, 100)]),
            None,  # LC_UUID between segments
            ("__AUTH_CONST", [(0x0, 7)]),
            ("__AUTH", [(0x0, 100)]),
            ("__DATA_DIRTY", [(0x0, 200)]),
            # __bss with attribute bits set, and __thread_bss.
            ("__DATA", [(0x0, 20), (0x80000001, 3000), (0x12, 64)]),
        ]))
        self.assertEqual(measured, size.LinkedSize(read_only=100, writable=327))

    def test_refuses_a_truncated_load_command(self):
        with self.assertRaisesRegex(ValueError, "truncated"):
            size.measure_macho(macho([("__TEXT", [(0x0, 10)])])[:60])

    def test_refuses_a_malformed_load_command_size(self):
        malformed = bytearray(macho([("__TEXT", [(0x0, 10)])]))
        struct.pack_into("<I", malformed, 36, 4)
        with self.assertRaisesRegex(ValueError, "malformed Mach-O load command"):
            size.measure_macho(bytes(malformed))


class MeasureTest(unittest.TestCase):
    def write(self, directory: str, name: str, payload: bytes) -> Path:
        path = Path(directory) / name
        path.write_bytes(payload)
        return path

    def test_dispatches_on_the_file_format(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(
                size.measure(self.write(directory, "a.elf", elf([(SHT_PROGBITS, SHF_ALLOC, 8)]))),
                size.LinkedSize(read_only=8, writable=0))
            self.assertEqual(
                size.measure(self.write(directory, "a.macho", macho([("__TEXT", [(0x0, 9)])]))),
                size.LinkedSize(read_only=9, writable=0))

    def test_refuses_universal_and_unknown_files(self):
        with tempfile.TemporaryDirectory() as directory:
            fat = self.write(directory, "fat", struct.pack(">II", 0xCAFEBABE, 2) + bytes(64))
            with self.assertRaisesRegex(ValueError, "universal"):
                size.measure(fat)
            with self.assertRaisesRegex(ValueError, "not a 64-bit"):
                size.measure(self.write(directory, "script", b"#!/bin/sh\n"))


class MainTest(unittest.TestCase):
    def run_main(self, args: list[str]) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = size.main(args)
        return status, out.getvalue(), err.getvalue()

    def test_passes_within_budget_and_reports_every_product(self):
        with tempfile.TemporaryDirectory() as directory:
            product = Path(directory) / "tool"
            product.write_bytes(elf([(SHT_PROGBITS, SHF_ALLOC, 100),
                                     (SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 20)]))
            status, out, _ = self.run_main([f"--product=tool={product}", "--budget=tool=120"])
        self.assertEqual(status, 0)
        self.assertIn("native-linked-size product=tool read_only=100 writable=20 total=120 "
                      "budget=120 headroom=0", out)

    def test_fails_over_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            product = Path(directory) / "tool"
            product.write_bytes(elf([(SHT_PROGBITS, SHF_ALLOC, 121)]))
            status, _, err = self.run_main([f"--product=tool={product}", "--budget=tool=120"])
        self.assertEqual(status, 1)
        self.assertIn("tool: 121 bytes exceeds its 120-byte budget by 1 bytes (budgets: "
                      "//tools/ci:native_linked_size_budget_test", err)

    def test_requires_exactly_one_budget_per_product(self):
        with tempfile.TemporaryDirectory() as directory:
            product = Path(directory) / "tool"
            product.write_bytes(elf([(SHT_PROGBITS, SHF_ALLOC, 1)]))
            cases = (
                [f"--product=tool={product}"],
                [f"--product=tool={product}", "--budget=tool=5", "--budget=other=5"],
                [f"--product=tool={product}", "--budget=tool=5", "--budget=tool=6"],
                [f"--product=tool={product}", "--budget=tool=many"],
                [f"--product=tool={product}", "--budget=tool"],
                [],
            )
            for args in cases:
                with self.subTest(args=args):
                    status, _, err = self.run_main(args)
                    self.assertEqual(status, 2)
                    self.assertIn("native linked-size check failed", err)


if __name__ == "__main__":
    unittest.main()
