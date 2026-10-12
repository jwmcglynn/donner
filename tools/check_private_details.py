#!/usr/bin/env python3
"""Fails when tracked files contain private environment details.

This is a public repository. The check scans every file reported by
`git ls-files` (or an explicit file list) and reports each finding as
`path:line: kind`, without echoing the matched text. A sensitive filename is
reported by its one-based input position instead of repeating the filename.

Generic checks, always on:
  home-path        /Users/<name>, /home/<name> or C:\\Users\\<name> for a
                   non-placeholder account name.
  private-address  IPv4 private (10/8, 172.16/12, 192.168/16), carrier-grade
                   NAT (100.64/10) and link-local (169.254/16) addresses, and
                   IPv6 unique-local (fc00/7) addresses.
  private-suffix   Host names under .lan, .local, .internal or .home.arpa.

Optional name checks, on only when a private name list is configured:
  private-name     Any configured name, case-insensitively, with or without
                   `-`, `_`, `.` or a space between its parts.
  private-account  A configured account name used as a home directory,
                   `User=<name>` or `~<name>`.

The name list is never tracked. It is read from, in order:
  1. the DONNER_PRIVATE_NAMES environment variable,
  2. the file named by DONNER_PRIVATE_NAMES_FILE,
  3. `.private-names` at the repository root (git-ignored).
Entries are separated by newlines, commas or whitespace; `#` starts a comment.
A plain entry is a private name (machine, service, repository or domain).
An entry written `exact:<name>` is a private name matched only as written
(ignoring case), for short names whose separator-free form is a common
phrase. An entry written `account:<name>` is a personal account name. When no list is
configured, the name checks are skipped and the generic checks still run.
An explicit configuration or existing default file must be readable and contain
usable entries; a configuration failure exits nonzero without printing its path.

Fixtures that must contain one of these shapes (for example a test proving a
redactor removes private addresses) build the value at runtime, so no file
needs a waiver.

Usage:
  python3 tools/check_private_details.py                # scan tracked files
  python3 tools/check_private_details.py FILE [FILE...] # scan specific files
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from ipaddress import AddressValueError, IPv6Address
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Iterable, Mapping, Optional


KIND_HOME_PATH = "home-path"
KIND_PRIVATE_ADDRESS = "private-address"
KIND_PRIVATE_SUFFIX = "private-suffix"
KIND_PRIVATE_NAME = "private-name"
KIND_PRIVATE_ACCOUNT = "private-account"

DESCRIPTIONS = {
    KIND_HOME_PATH: "personal home directory path",
    KIND_PRIVATE_ADDRESS: "private network address",
    KIND_PRIVATE_SUFFIX: "private network host name",
    KIND_PRIVATE_NAME: "configured private name",
    KIND_PRIVATE_ACCOUNT: "configured personal account",
}

NAMES_ENV = "DONNER_PRIVATE_NAMES"
NAMES_FILE_ENV = "DONNER_PRIVATE_NAMES_FILE"
NAMES_DEFAULT_FILE = ".private-names"
ACCOUNT_PREFIX = "account:"
EXACT_PREFIX = "exact:"

# Placeholder or system account names that are fine in examples, fixtures,
# containers, CI runners and package managers.
GENERIC_HOME_NAMES = frozenset(
    {
        "$user",
        "${user}",
        "<name>",
        "<user>",
        "<username>",
        "all users",
        "default",
        "example",
        "fuzzer",
        "linuxbrew",
        "me",
        "name",
        "operator",
        "public",
        "runner",
        "shared",
        "someone",
        "ubuntu",
        "user",
        "username",
        "you",
    }
)

_OCTET = r"(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)"
_HOME_NAME = r"([^/\\\s\"'`<>()\[\],;:]+|<[^>/\\\s]+>)"
HOME_PATH_RE = re.compile(
    r"(?:(?<![\w.~-])/(?:Users|home)/|(?<![\w])[A-Za-z]:[\\/]+Users[\\/]+)" + _HOME_NAME,
    re.IGNORECASE,
)
IPV4_PRIVATE_RE = re.compile(
    r"(?<![\w.])(?:"
    r"10(?:\." + _OCTET + r"){3}"
    r"|172\.(?:1[6-9]|2\d|3[01])(?:\." + _OCTET + r"){2}"
    r"|192\.168(?:\." + _OCTET + r"){2}"
    r"|100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7])(?:\." + _OCTET + r"){2}"
    r"|169\.254(?:\." + _OCTET + r"){2}"
    r")(?![\w-]|\.\d)"
)
IPV6_ULA_RE = re.compile(
    r"(?<![\w:])f[cd][0-9a-f]{2}:[0-9a-f:]*(?:\.[0-9]+){0,3}(?![\w:]|\.\d)",
    re.IGNORECASE,
)
# A dotted number right after a version keyword is a version, not an address.
VERSION_CONTEXT_RE = re.compile(r"(?<![\w.-])(?:version|ver|sdk|build)\s*[:=]?\s*[\"']?$", re.IGNORECASE)
_LABEL = r"[a-z0-9](?:[a-z0-9-]*[a-z0-9])?"
PRIVATE_SUFFIX_RE = re.compile(
    r"(?<![\w.-])" + _LABEL + r"(?:\." + _LABEL + r")*\.(?:lan|local|internal|home\.arpa)"
    r"(?![\w-]|\.[\w-])",
    re.IGNORECASE,
)
WORD_RE = re.compile(r"[A-Za-z0-9]+")
# Characters that may separate the parts of one name: `bazel-x`, `bazel_x`,
# `bazel.x` and `bazel x` all match the configured name `bazel-x`.
NAME_SEPARATOR_RE = re.compile(r"^(?:[-_.]| +)$")


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    kind: str

    def __str__(self) -> str:
        return "%s:%d: %s: %s" % (self.path, self.line, self.kind, DESCRIPTIONS[self.kind])


def _compact(value: str) -> str:
    return "".join(WORD_RE.findall(value.lower()))


def _prefixed(entries: Iterable[str], prefix: str) -> list[str]:
    return [entry[len(prefix):] for entry in entries if entry.lower().startswith(prefix)]


@dataclass(frozen=True)
class PrivateNames:
    """Compact (separator-free, lowercase) names and account names."""

    names: frozenset[str] = field(default_factory=frozenset)
    accounts: frozenset[str] = field(default_factory=frozenset)
    exact: Optional[re.Pattern] = None

    @classmethod
    def parse(cls, text: str) -> "PrivateNames":
        entries = [
            entry
            for raw_line in text.splitlines()
            for entry in re.split(r"[,\s]+", raw_line.split("#", 1)[0])
            if entry
        ]
        exact = sorted(
            re.escape(value) for value in _prefixed(entries, EXACT_PREFIX) if value
        )
        accounts = {_compact(value) for value in _prefixed(entries, ACCOUNT_PREFIX)}
        names = {
            _compact(entry)
            for entry in entries
            if not entry.lower().startswith((EXACT_PREFIX, ACCOUNT_PREFIX))
        }
        pattern = None
        if exact:
            pattern = re.compile(
                r"(?<![A-Za-z0-9])(?:%s)(?![A-Za-z0-9])" % "|".join(exact), re.IGNORECASE
            )
        return cls(frozenset(names - {""}), frozenset(accounts - {""}), pattern)

    def matches_name(self, line: str) -> bool:
        if self.exact is not None and self.exact.search(line):
            return True
        return bool(self.names) and any(
            run in self.names for run in _word_runs(line, max(map(len, self.names)))
        )

    def __bool__(self) -> bool:
        return bool(self.names or self.accounts or self.exact)


def load_private_names(root: Path, environ: Mapping[str, str] = os.environ) -> PrivateNames:
    if environ.get(NAMES_ENV):
        names = PrivateNames.parse(environ[NAMES_ENV])
    else:
        if NAMES_FILE_ENV in environ:
            if not environ[NAMES_FILE_ENV]:
                raise ValueError("private-name configuration file path is empty")
            candidate = Path(environ[NAMES_FILE_ENV])
        else:
            candidate = root / NAMES_DEFAULT_FILE
            try:
                candidate.lstat()
            except FileNotFoundError:
                return PrivateNames()
            except OSError:
                raise ValueError("private-name configuration file cannot be inspected") from None
        try:
            names = PrivateNames.parse(candidate.read_text(encoding="utf-8"))
        except (OSError, UnicodeError):
            raise ValueError("private-name configuration file cannot be read") from None
    if not names:
        raise ValueError("private-name configuration contains no usable entries")
    return names


def _is_generic_home(name: str) -> bool:
    return name.lower() in GENERIC_HOME_NAMES


def _address_kinds(line: str) -> Iterable[str]:
    for match in IPV4_PRIVATE_RE.finditer(line):
        if not VERSION_CONTEXT_RE.search(line, max(0, match.start() - 16), match.start()):
            yield KIND_PRIVATE_ADDRESS
    for match in IPV6_ULA_RE.finditer(line):
        try:
            IPv6Address(match.group())
        except AddressValueError:
            continue
        yield KIND_PRIVATE_ADDRESS


def _word_runs(line: str, max_length: int) -> Iterable[str]:
    """Yields compact adjacent words up to the longest configured name length.

    Words are adjacent when only a name separator lies between them, so a run
    never spans other punctuation such as `/` or `:`. Each word contributes at
    least one character, bounding the work without limiting the number of parts.
    """
    words = list(WORD_RE.finditer(line))
    for start, first in enumerate(words):
        compact = first.group(0).lower()
        if len(compact) > max_length:
            continue
        yield compact
        for index in range(start + 1, len(words)):
            previous, word = words[index - 1], words[index]
            if not NAME_SEPARATOR_RE.match(line[previous.end():word.start()]):
                break
            compact += word.group(0).lower()
            if len(compact) > max_length:
                break
            yield compact


def _account_contexts(line: str) -> Iterable[str]:
    for match in HOME_PATH_RE.finditer(line):
        yield match.group(1)
    for match in re.finditer(r"(?:\bUser\s*=\s*|(?<![\w/])~)([A-Za-z0-9_.-]+)", line):
        yield match.group(1)


def _line_kinds(line: str, private_names: PrivateNames) -> list[str]:
    """Returns the finding kinds on one line, one entry per match."""
    kinds = [
        KIND_HOME_PATH
        for match in HOME_PATH_RE.finditer(line)
        if not _is_generic_home(match.group(1))
    ]
    kinds.extend(_address_kinds(line))
    kinds.extend(KIND_PRIVATE_SUFFIX for _ in PRIVATE_SUFFIX_RE.finditer(line))
    if private_names.matches_name(line):
        kinds.append(KIND_PRIVATE_NAME)
    if private_names.accounts and any(
        _compact(name) in private_names.accounts for name in _account_contexts(line)
    ):
        kinds.append(KIND_PRIVATE_ACCOUNT)
    return kinds


def scan_text(path: str, text: str, private_names: PrivateNames = PrivateNames()) -> list[Finding]:
    return [
        Finding(path, line_number, kind)
        for line_number, line in enumerate(text.splitlines(), start=1)
        for kind in _line_kinds(line, private_names)
    ]


def _is_binary(data: bytes) -> bool:
    return b"\0" in data[:8192]


def scan_files(
    root: Path, relative_paths: Iterable[str], private_names: PrivateNames = PrivateNames()
) -> list[Finding]:
    findings: list[Finding] = []
    for position, relative in enumerate(relative_paths, start=1):
        path_findings = scan_text("<tracked path %d>" % position, relative, private_names)
        findings.extend(path_findings)
        display_path = path_findings[0].path if path_findings else relative
        path = root / relative
        if path.is_symlink():
            # The link target is the tracked payload. Never follow it into an
            # untracked or external file, including when the target is missing.
            data = os.fsencode(os.readlink(path))
        elif path.is_file():
            data = path.read_bytes()
        else:
            continue
        if _is_binary(data):
            continue
        findings.extend(scan_text(display_path, data.decode("utf-8", errors="replace"), private_names))
    return findings


def tracked_files(root: Path) -> list[str]:
    output = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        stdout=subprocess.PIPE,
    ).stdout
    return [entry.decode("utf-8") for entry in output.split(b"\0") if entry]


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("files", nargs="*", help="Files to scan (default: all tracked files)")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    try:
        private_names = load_private_names(root)
    except ValueError as error:
        print("check_private_details: %s" % error, file=sys.stderr)
        return 2
    if not private_names:
        print("check_private_details: no private name list configured; name checks skipped.")
    files = args.files or tracked_files(root)
    findings = scan_files(root, files, private_names)
    for finding in findings:
        print(finding, file=sys.stderr)
    if findings:
        print(
            "check_private_details: %d finding(s). Replace private details with generic "
            "placeholders; build unavoidable test fixtures at runtime." % len(findings),
            file=sys.stderr,
        )
        return 1
    print("check_private_details: scanned %d files, no private details found." % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
