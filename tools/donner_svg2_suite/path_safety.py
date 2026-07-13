#!/usr/bin/env python3
"""Path-safety primitives for the Donner SVG2 test suite.

Every input, oracle, and resource named by a corpus manifest is untrusted. A
malicious or buggy manifest must never be able to make the validator or the
reference runner touch a file outside the corpus root. This module performs the
checks the design's Security section requires *before any file access*:

- reject absolute paths;
- reject paths containing a URL scheme (``http:``, ``file:``, ``data:``, ...);
- reject Windows-style backslash separators and drive letters;
- reject ``..`` traversal and ``.`` current-directory components;
- reject empty paths and empty components; and
- reject any component that is a symlink (a symlink is an escape vector even
  when it happens to resolve inside the root today).

The lexical checks in :func:`ensure_relative` run without touching the
filesystem, so a hostile path is rejected before it is ever opened. The
filesystem check in :func:`resolve_within_root` additionally confirms the
fully resolved path stays inside the resolved root, as defence in depth.
"""

from __future__ import annotations

import os
import re
from pathlib import PurePosixPath


# Matches a leading ``scheme:`` per RFC 3986. Windows drive letters like ``C:``
# also match, which is intentional: both are rejected as non-relative.
_URL_SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")

# Default cap for reading manifests and other suite-controlled text files. A
# decompression or expansion bomb should hit this ceiling instead of exhausting
# memory. 16 MiB is far larger than any legitimate manifest.
DEFAULT_MAX_BYTES = 16 * 1024 * 1024


class UnsafePathError(ValueError):
    """Raised when a manifest-declared path fails a safety check."""


def ensure_relative(raw: str) -> PurePosixPath:
    """Validate ``raw`` as a safe corpus-relative path and return it normalized.

    Performs lexical checks only; it does not touch the filesystem. Raises
    :class:`UnsafePathError` for anything that could escape the corpus root.
    """

    if not isinstance(raw, str):
        raise UnsafePathError(f"path must be a string, got {type(raw).__name__}")
    if raw == "":
        raise UnsafePathError("empty path")
    if "\x00" in raw:
        raise UnsafePathError("path contains a NUL byte")
    if "\\" in raw:
        raise UnsafePathError(f"backslash separator is not allowed: {raw!r}")
    if _URL_SCHEME_RE.match(raw):
        raise UnsafePathError(f"path looks like a URL or drive-qualified path: {raw!r}")
    if raw.startswith("/"):
        raise UnsafePathError(f"absolute path is not allowed: {raw!r}")

    # Check the raw segments rather than PurePosixPath.parts: PurePosixPath
    # silently collapses "." and empty ("//") segments, which would let an
    # un-normalized path slip past. We require manifests to carry clean paths.
    for segment in raw.split("/"):
        if segment == "..":
            raise UnsafePathError(f"parent-directory traversal is not allowed: {raw!r}")
        if segment == ".":
            raise UnsafePathError(f"current-directory component is not allowed: {raw!r}")
        if segment == "":
            raise UnsafePathError(f"empty path component is not allowed: {raw!r}")

    pure = PurePosixPath(raw)
    if pure.is_absolute():
        raise UnsafePathError(f"absolute path is not allowed: {raw!r}")

    return pure


def resolve_within_root(root: os.PathLike[str] | str, raw: str) -> str:
    """Return the absolute path of ``raw`` under ``root`` after safety checks.

    Combines the lexical checks in :func:`ensure_relative` with filesystem
    checks: every component that exists must be a real directory or file, never
    a symlink, and the resolved target must stay inside the resolved root.
    Raises :class:`UnsafePathError` on any violation. The file is not opened.
    """

    relative = ensure_relative(raw)
    root_real = os.path.realpath(root)

    # Reject a symlink at any existing prefix of the path. os.path.realpath on
    # the final target would silently follow these, so check them explicitly to
    # honour the "reject symlinks" rule rather than only "reject escapes".
    current = root_real
    for part in relative.parts:
        current = os.path.join(current, part)
        if os.path.islink(current):
            raise UnsafePathError(f"symlink component is not allowed: {raw!r}")

    resolved = os.path.realpath(os.path.join(root_real, relative))
    if resolved != root_real and not resolved.startswith(root_real + os.sep):
        raise UnsafePathError(f"path escapes the corpus root: {raw!r}")
    return resolved


def read_text_capped(path: os.PathLike[str] | str, max_bytes: int = DEFAULT_MAX_BYTES) -> str:
    """Read a UTF-8 text file, refusing files larger than ``max_bytes``.

    The size ceiling is enforced during the read (not via a prior ``stat``) so
    a file that grows between the check and the read, or a pipe/special file,
    cannot smuggle in unbounded data. Raises :class:`UnsafePathError` when the
    cap is exceeded.
    """

    with open(path, "rb") as handle:
        data = handle.read(max_bytes + 1)
    if len(data) > max_bytes:
        raise UnsafePathError(f"file exceeds the {max_bytes}-byte read cap: {os.fspath(path)!r}")
    return data.decode("utf-8")
