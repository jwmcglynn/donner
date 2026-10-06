#!/usr/bin/env python3
"""Size budgets for shipped native products, measured from the linked executables.

A product's size is the program its executable maps from the file, split into read-only bytes
(code, constants and unwind tables) and writable initialized data. On ELF that is the allocated
program sections (`SHT_PROGBITS`, the init and fini arrays, and the processor unwind table); on
Mach-O it is the sections of the `__TEXT` segment and of the data segments. Dynamic-linking
metadata (ELF symbol, string, hash, version and relocation tables, which Mach-O keeps in
`__LINKEDIT`), notes, debug information and zero-filled sections are excluded, so the measurement
follows the program the linker kept rather than how the file was stripped or linked.

Usage:
  native_linked_size.py --product NAME=PATH ... --budget NAME=BYTES ...
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

ELF_MAGIC = b"\x7fELF"
MACHO_MAGIC_64 = 0xFEEDFACF
MACHO_FAT_MAGICS = (0xCAFEBABE, 0xCAFEBABF)

# ELF section header fields used here.
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
# Section types that hold the program itself: code, constants and initialized data, the init and
# fini arrays, and the processor-specific unwind table (`SHT_X86_64_UNWIND` for `.eh_frame`).
ELF_PROGRAM_TYPES = (1, 14, 15, 16, 0x70000001)

# Mach-O load command and section types used here.
LC_SEGMENT_64 = 0x19
MACHO_CODE_SEGMENTS = ("__TEXT",)
MACHO_DATA_SEGMENTS = ("__DATA", "__DATA_CONST", "__DATA_DIRTY", "__AUTH", "__AUTH_CONST")
MACHO_ZEROFILL_TYPES = (0x1, 0xC, 0x12)  # S_ZEROFILL, S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL


@dataclass(frozen=True)
class LinkedSize:
    """Program bytes a linked executable maps from its file."""

    read_only: int
    writable: int

    @property
    def total(self) -> int:
        return self.read_only + self.writable


def _unpack(fmt: str, data: bytes, offset: int) -> tuple:
    end = offset + struct.calcsize(fmt)
    if offset < 0 or end > len(data):
        raise ValueError(f"truncated executable header at byte {offset}")
    return struct.unpack_from(fmt, data, offset)


def measure_elf(data: bytes) -> LinkedSize:
    """Measures a 64-bit little-endian ELF executable."""
    if len(data) < 64:
        raise ValueError("truncated ELF header")
    if data[4] != 2 or data[5] != 1:
        raise ValueError("only 64-bit little-endian ELF executables are supported")
    (section_offset,) = _unpack("<Q", data, 0x28)
    entry_size, section_count = _unpack("<HH", data, 0x3A)
    if section_offset == 0:
        raise ValueError("ELF executable has no section headers")
    if entry_size != 64:
        raise ValueError(f"unexpected ELF section header size {entry_size}")
    if section_count == 0:
        # Extended numbering: the count lives in the first section header's size field.
        (section_count,) = _unpack("<Q", data, section_offset + 0x20)
    read_only = 0
    writable = 0
    for index in range(section_count):
        header = section_offset + index * entry_size
        _, section_type, flags, _, _, size = _unpack("<IIQQQQ", data, header)
        if not flags & SHF_ALLOC or section_type not in ELF_PROGRAM_TYPES:
            continue
        if flags & SHF_WRITE:
            writable += size
        else:
            read_only += size
    return LinkedSize(read_only, writable)


def _segment_name(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace")


def _macho_segment_size(data: bytes, offset: int) -> LinkedSize:
    """Read-only and writable bytes of the LC_SEGMENT_64 command at `offset`."""
    (raw_segment,) = _unpack("16s", data, offset + 8)
    (section_count,) = _unpack("<I", data, offset + 64)
    segment = _segment_name(raw_segment)
    if segment not in MACHO_CODE_SEGMENTS and segment not in MACHO_DATA_SEGMENTS:
        return LinkedSize(0, 0)
    mapped = 0
    for index in range(section_count):
        section = offset + 72 + index * 80
        (size,) = _unpack("<Q", data, section + 40)
        (flags,) = _unpack("<I", data, section + 64)
        if flags & 0xFF not in MACHO_ZEROFILL_TYPES:
            mapped += size
    if segment in MACHO_CODE_SEGMENTS:
        return LinkedSize(mapped, 0)
    return LinkedSize(0, mapped)


def measure_macho(data: bytes) -> LinkedSize:
    """Measures a thin 64-bit little-endian Mach-O executable."""
    _, _, _, _, command_count, _, _, _ = _unpack("<IiiIIIII", data, 0)
    offset = 32
    read_only = 0
    writable = 0
    for _ in range(command_count):
        command, command_size = _unpack("<II", data, offset)
        if command_size < 8:
            raise ValueError(f"malformed Mach-O load command at byte {offset}")
        if command == LC_SEGMENT_64:
            segment = _macho_segment_size(data, offset)
            read_only += segment.read_only
            writable += segment.writable
        offset += command_size
    return LinkedSize(read_only, writable)


def measure(path: Path) -> LinkedSize:
    """Measures a linked native executable, refusing a format this check does not read."""
    data = path.read_bytes()
    if data[:4] == ELF_MAGIC:
        return measure_elf(data)
    if len(data) >= 4:
        (magic,) = struct.unpack_from("<I", data, 0)
        if magic == MACHO_MAGIC_64:
            return measure_macho(data)
        (big_endian_magic,) = struct.unpack_from(">I", data, 0)
        if big_endian_magic in MACHO_FAT_MAGICS:
            raise ValueError(f"{path}: universal Mach-O files are not measured; build one arch")
    raise ValueError(f"{path}: not a 64-bit ELF or Mach-O executable")


def _pairs(values: list[str], label: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for value in values:
        name, separator, rest = value.partition("=")
        if not separator or not name or not rest:
            raise ValueError(f"--{label} expects NAME=VALUE, got {value!r}")
        if name in result:
            raise ValueError(f"--{label} names {name!r} more than once")
        result[name] = rest
    return result


def check(products: dict[str, Path], budgets: dict[str, int]) -> list[str]:
    """Measures every product and returns one failure message per product over its budget."""
    if set(products) != set(budgets):
        raise ValueError(
            "every product needs exactly one budget: products "
            f"{sorted(products)}, budgets {sorted(budgets)}"
        )
    failures = []
    for name in sorted(products):
        size = measure(products[name])
        limit = budgets[name]
        print(
            f"native-linked-size product={name} read_only={size.read_only} "
            f"writable={size.writable} total={size.total} budget={limit} "
            f"headroom={limit - size.total}"
        )
        if size.total > limit:
            failures.append(
                f"{name}: {size.total} bytes exceeds its {limit}-byte budget by "
                f"{size.total - limit} bytes (budgets: //tools/ci:native_linked_size_budget_test "
                "and docs/design_docs/0064-gpu_release_matrix.md)"
            )
    return failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--product", action="append", default=[], metavar="NAME=PATH")
    parser.add_argument("--budget", action="append", default=[], metavar="NAME=BYTES")
    args = parser.parse_args(argv)
    try:
        products = {name: Path(path) for name, path in _pairs(args.product, "product").items()}
        budgets = {name: int(value) for name, value in _pairs(args.budget, "budget").items()}
        if not products:
            raise ValueError("no --product to measure")
        failures = check(products, budgets)
    except (OSError, ValueError) as error:
        print(f"native linked-size check failed: {error}", file=sys.stderr)
        return 2
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
