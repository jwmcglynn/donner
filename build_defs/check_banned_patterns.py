#!/usr/bin/env python3
"""
Check C++ source files for banned language patterns documented in
docs/coding_style.md "Language and Library Features".

Catches escapes like PR #415 where `long long` template specialization
collided with `std::int64_t` on Linux (where int64_t IS long long) but
not on macOS (where int64_t is long).

Rules enforced:
  - No `long long` type: use std::int64_t / std::uint64_t / std::size_t
  - No `std::aligned_storage`: use alignas(T) on a byte buffer
  - No `std::aligned_union`: same reason
  - No user-defined literal operators (operator"" _foo): use named helpers

Usage:
  python3 tools/check_banned_patterns.py            # Check all files
  python3 tools/check_banned_patterns.py FILE...    # Check specific files
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import List, Tuple


# Each rule: (pattern, description, remediation)
_RULES: List[Tuple[re.Pattern, str, str]] = [
    (
        re.compile(r"\blong\s+long\b"),
        "`long long` type",
        "Use std::int64_t / std::uint64_t (width-portable) — long long is long on macOS but "
        "long-long on Linux, causing template specialization collisions (see PR #415).",
    ),
    (
        re.compile(r"\bstd::aligned_storage\b"),
        "std::aligned_storage",
        "Use `alignas(T) std::byte buffer[N * sizeof(T)]` instead — aligned_storage is "
        "deprecated in C++23.",
    ),
    (
        re.compile(r"\bstd::aligned_union\b"),
        "std::aligned_union",
        "Use `alignas` on a byte buffer instead — aligned_union is deprecated in C++23.",
    ),
    (
        re.compile(r"\boperator\s*\"\"\s*_[A-Za-z_][A-Za-z0-9_]*"),
        "user-defined literal operator",
        "Use a named helper function (e.g. `RgbHex(0xFF0000)` instead of `0xFF0000_rgb`).",
    ),
]


def _strip_comments_and_strings(text: str) -> str:
    """Remove comments and string literals but preserve line counts."""
    # Line comments: replace text after // with spaces, keep newlines
    text = re.sub(r"//[^\n]*", "", text)
    # Block comments: replace with equivalent number of newlines
    def _replace_block(m):
        return "\n" * m.group(0).count("\n")
    text = re.sub(r"/\*.*?\*/", _replace_block, text, flags=re.DOTALL)
    text = re.sub(r'R"\([^)]*\)"', '""', text)
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    return text


_NOLINT_RE = re.compile(r"//\s*NOLINT\(banned_patterns(?::[^)]*)?\)")


def check_file(path: Path) -> List[Tuple[int, str, str]]:
    """Check a single file; return list of (line_number, description, remediation).

    Lines marked `// NOLINT(banned_patterns)` or `// NOLINT(banned_patterns: reason)`
    are exempted.
    """
    try:
        raw = path.read_text()
    except (UnicodeDecodeError, IOError):
        return []

    raw_lines = raw.splitlines()
    stripped = _strip_comments_and_strings(raw)

    errors: List[Tuple[int, str, str]] = []
    for pattern, desc, remediation in _RULES:
        for m in pattern.finditer(stripped):
            line = stripped.count("\n", 0, m.start()) + 1
            # Check the match line and up to 2 lines after for a NOLINT marker
            # (clang-format may wrap the signature onto multiple lines).
            suppressed = False
            for offset in (0, 1, 2):
                idx = line - 1 + offset
                if 0 <= idx < len(raw_lines) and _NOLINT_RE.search(raw_lines[idx]):
                    suppressed = True
                    break
            if suppressed:
                continue
            errors.append((line, desc, remediation))

    return sorted(errors)


def _iter_source_files(paths: List[Path]) -> List[Path]:
    exts = {".cc", ".h", ".hpp", ".cpp"}
    exclude_prefixes = ("third_party/", "bazel-", ".git/")
    result: List[Path] = []
    for p in paths:
        if p.is_file():
            if p.suffix in exts:
                result.append(p)
        elif p.is_dir():
            for sub in p.rglob("*"):
                if sub.suffix not in exts:
                    continue
                rel = sub.as_posix()
                if any(rel.startswith(ex) or f"/{ex}" in rel for ex in exclude_prefixes):
                    continue
                result.append(sub)
    return sorted(result)


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "paths",
        nargs="*",
        help="Files or directories to check (default: donner/ and examples/)",
    )
    args = parser.parse_args(argv[1:])

    inputs = [Path(p) for p in args.paths] if args.paths else [Path("donner"), Path("examples")]
    files = _iter_source_files(inputs)
    if not files:
        print("No source files found to check.", file=sys.stderr)
        return 0

    total_errors = 0
    for f in files:
        errors = check_file(f)
        for line, desc, remediation in errors:
            total_errors += 1
            print(f"{f}:{line}: {desc}")
            print(f"    {remediation}")

    if total_errors:
        print(
            f"\n{total_errors} banned pattern(s) found. See docs/coding_style.md "
            f"'Language and Library Features' for details.",
            file=sys.stderr,
        )
        return 1

    print(f"OK: {len(files)} file(s) checked, no banned patterns found.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
