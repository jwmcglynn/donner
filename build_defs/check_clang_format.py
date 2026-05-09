#!/usr/bin/env python3
"""Verify each given C/C++ source matches `clang-format --dry-run -Werror`.

One py_test is emitted per `donner_cc_library`/`_test`/`_binary` (see
`build_defs/rules.bzl`). The test fails if any of the parent target's
srcs/hdrs deviates from the project's `.clang-format` style, so format
escapes get caught by `bazel test //...` instead of waiting for the
`Lint` GitHub workflow.

clang-format is resolved via `$CLANG_FORMAT` (override) or the first
`clang-format-19` / `clang-format-18` / `clang-format` found on `$PATH`.
If none is available the test fails with an actionable install hint
rather than silently passing.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys


_PREFERRED_BINARIES = (
    "clang-format-19",
    "clang-format-18",
    "clang-format",
)


def find_clang_format() -> str | None:
    override = os.environ.get("CLANG_FORMAT")
    if override:
        return override if shutil.which(override) else None
    for name in _PREFERRED_BINARIES:
        path = shutil.which(name)
        if path:
            return path
    return None


def main(argv: list[str]) -> int:
    files = argv[1:]
    if not files:
        return 0

    clang_format = find_clang_format()
    if clang_format is None:
        sys.stderr.write(
            "clang_format_lint: clang-format not found on PATH.\n"
            "  Install clang-format (Ubuntu/Debian: `apt-get install clang-format`,\n"
            "  macOS: `brew install llvm` and use `clang-format` from the brew prefix),\n"
            "  or set CLANG_FORMAT to a usable binary.\n"
        )
        return 1

    result = subprocess.run(
        [clang_format, "--dry-run", "--Werror", *files],
        check=False,
    )
    return result.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv))
