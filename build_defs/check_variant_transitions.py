#!/usr/bin/env python3
"""Fails on a raw transition() that writes a product variant setting without review.

Native test-variant transitions are declared through `variant_transition` in
build_defs/variant_settings.bzl, which checks their outputs against VARIANT_SETTINGS. A raw
`transition()` that writes one of those settings bypasses that check, so each one must be listed
in REVIEWED_SITES below with the reason it stays outside. A listed site that no longer exists or
no longer writes a variant setting is reported too, so the list stays exact.

A site writes a variant setting when its `outputs` argument names one, directly or through a
top-level constant defined in any scanned .bzl file.

Usage:
  python3 build_defs/check_variant_transitions.py            # scan tracked .bzl files
  python3 build_defs/check_variant_transitions.py FILE...    # scan specific files
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

SETTINGS_FILE = "build_defs/variant_settings.bzl"

# (file, transition name) -> why the transition writes variant settings without variant_transition.
REVIEWED_SITES = {
    ("build_defs/variant_settings.bzl", "variant_neutral_transition"):
        "The reset to the variant-neutral configuration itself.",
    ("build_defs/rules.bzl", "_ci_wasm_transition"):
        "Wasm product profiles; compile sharing applies to native platforms only.",
    ("donner/editor/wasm/editor_wasm_transition.bzl", "_editor_wasm_geode_transition"):
        "Wasm editor package; compile sharing applies to native platforms only.",
    ("donner/svg/renderer/wasm/geode_browser_test_transition.bzl", "_geode_browser_test_transition"):
        "Wasm browser test package; compile sharing applies to native platforms only.",
    ("donner/svg/renderer/wasm/geode_browser_test_transition.bzl", "_enable_text"):
        "Wasm browser test package; compile sharing applies to native platforms only.",
    ("tools/ci/shipped_product.bzl", "_shipped_configuration"):
        "Shipped product configuration, not a test variant. It also changes the compilation mode "
        "and compile options, so its libraries compile separately either way.",
}

_CALL = re.compile(r"(?<![\w.])transition\(")
_ASSIGNED = re.compile(r"(\w+)\s*=\s*$")
_DEFINITION = re.compile(r"^([A-Za-z_]\w*)\s*=", re.MULTILINE)
_IDENTIFIER = re.compile(r"[A-Za-z_]\w*")
_STRING = re.compile(r'"(?:[^"\\]|\\.)*"')


_TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|[()\[\]{},]')


def _end(text: str, start: int, stop_at_comma: bool) -> int:
    """Where the expression starting at `start` ends.

    With `stop_at_comma`, that is the first top-level comma or unmatched closing bracket. Otherwise
    `start` is an opening bracket and the result is just past its matching close.
    """
    depth = 0
    for token in _TOKEN.finditer(text, start):
        char = token.group()
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        if depth < 0 or (stop_at_comma and depth == 0 and char == ","):
            return token.start()
        if depth == 0 and not stop_at_comma and char in ")]}":
            return token.end()
    raise ValueError("unbalanced brackets")


def _balanced(text: str, start: int) -> int:
    """Index just past the bracketed expression whose opening bracket is at `start`."""
    return _end(text, start, stop_at_comma=False)


def _argument(call: str, name: str) -> str:
    """The text of keyword argument `name` in a call, or an empty string."""
    match = re.search(r"\b%s\s*=" % name, call)
    return call[match.end():_end(call, match.end(), stop_at_comma=True)] if match else ""


def variant_settings(text: str) -> list[str]:
    """The setting labels of the VARIANT_SETTINGS dict in variant_settings.bzl."""
    start = text.index("VARIANT_SETTINGS = {")
    body = text[start:_balanced(text, text.index("{", start))]
    return re.findall(r'"(//[^"]+)"\s*:', body)


def definitions(sources: dict[str, str]) -> dict[str, str]:
    """Top-level constant name -> the text of its value, across all scanned files."""
    result = {}
    for text in sources.values():
        for match in _DEFINITION.finditer(text):
            value_start = match.end()
            line_end = text.find("\n", value_start)
            line_end = len(text) if line_end < 0 else line_end
            line = text[value_start:line_end].rstrip()
            if line and line[-1] in "([{":
                result[match.group(1)] = text[value_start:_balanced(text, value_start + len(line) - 1)]
            else:
                result[match.group(1)] = line
    return result


def _expand(text: str, constants: dict[str, str], depth: int = 4) -> str:
    if depth == 0:
        return text
    names = {name for name in _IDENTIFIER.findall(_STRING.sub("", text)) if name in constants}
    return text + "".join(_expand(constants[name], constants, depth - 1) for name in sorted(names))


def transition_sites(path: str, text: str):
    """Yields (name, call text) for each raw transition() call outside comments."""
    for match in _CALL.finditer(text):
        line_start = text.rfind("\n", 0, match.start()) + 1
        prefix = text[line_start:match.start()]
        if "#" in prefix or "`" in prefix or prefix.count('"') % 2:
            continue
        assigned = _ASSIGNED.search(prefix)
        if assigned:
            name = assigned.group(1)
        else:
            function = re.findall(r"^def (\w+)", text[:match.start()], re.MULTILINE)
            name = "<call in %s>" % (function[-1] if function else "module")
        yield name, text[match.start():_balanced(text, match.end() - 1)]


def check(sources: dict[str, str], settings: list[str]) -> list[str]:
    constants = definitions(sources)
    problems = []
    writers = set()
    for path, text in sorted(sources.items()):
        for name, call in transition_sites(path, text):
            outputs = _expand(_argument(call, "outputs"), constants)
            written = sorted(setting for setting in settings if '"%s"' % setting in outputs)
            if not written:
                continue
            writers.add((path, name))
            if (path, name) not in REVIEWED_SITES:
                problems.append(
                    "%s: transition %s writes %s without variant_transition. Declare it with "
                    "variant_transition, or add it to REVIEWED_SITES in %s with the reason it "
                    "stays outside." % (path, name, ", ".join(written), Path(__file__).name))
    for path, name in sorted(set(REVIEWED_SITES) - writers):
        if path in sources:
            problems.append("%s: reviewed transition %s no longer writes a variant setting; "
                            "remove it from REVIEWED_SITES." % (path, name))
    return problems


def main(argv: list[str]) -> int:
    paths = argv or [
        path for path in subprocess.check_output(["git", "ls-files", "*.bzl"], text=True).split()
        if not path.startswith("third_party/")
    ]
    sources = {path: Path(path).read_text(encoding="utf-8") for path in paths}
    settings_text = sources.get(SETTINGS_FILE) or Path(SETTINGS_FILE).read_text(encoding="utf-8")
    problems = check(sources, variant_settings(settings_text))
    for problem in problems:
        print(problem, file=sys.stderr)
    if not problems:
        print("OK: %d .bzl file(s) checked; every raw transition writing a variant setting is "
              "reviewed." % len(sources))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
