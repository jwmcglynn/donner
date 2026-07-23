#!/usr/bin/env python3
"""
Check source files for banned language patterns documented in
docs/coding_style.md "Language and Library Features".

Catches escapes like PR #415 where a `long long` template specialization
collided with a `std::int64_t` one on macOS (where int64_t IS long long)
but not on 64-bit Linux (where int64_t is long).

Rules enforced:
  - No `long long` type: use std::int64_t / std::uint64_t / std::size_t
  - No `std::aligned_storage`: use alignas(T) on a byte buffer
  - No `std::aligned_union`: same reason
  - No user-defined literal operators (operator"" _foo): use named helpers
  - No `XMLQualifiedNameRef` / `RcStringOrRef` return types: return owning values instead
    These C++-specific rules apply to C++/ObjC++ source files.
  - No typographic hyphens/dashes, smart quotes, or hidden Unicode whitespace
  - No imgui / GLFW / Tracy headers outside `donner/editor/**` (path-scoped)
  - No ImGui `AddImageQuad`: present document textures through direct framebuffer composition
  - No direct TreeComponent structural mutation outside approved low-level code

Usage:
  python3 build_defs/check_banned_patterns.py            # Check all files
  python3 build_defs/check_banned_patterns.py FILE...    # Check specific files
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import unicodedata
from pathlib import Path
from typing import Dict, List, NamedTuple, Tuple


class _Rule(NamedTuple):
    pattern: re.Pattern
    description: str
    remediation: str
    # Path prefixes (forward-slash) where this rule does NOT apply.
    # Empty tuple means the rule applies everywhere.
    exempt_path_prefixes: Tuple[str, ...] = ()


_RULES: List[_Rule] = [
    _Rule(
        pattern=re.compile(r"\blong\s+long\b"),
        description="`long long` type",
        remediation=(
            "Use std::int64_t / std::uint64_t (width-portable) - int64_t is long long on macOS "
            "but long on Linux, causing template specialization collisions (see PR #415)."
        ),
    ),
    _Rule(
        pattern=re.compile(r"\bstd::aligned_storage\b"),
        description="std::aligned_storage",
        remediation=(
            "Use `alignas(T) std::byte buffer[N * sizeof(T)]` instead - aligned_storage is "
            "deprecated in C++23."
        ),
    ),
    _Rule(
        pattern=re.compile(r"\bstd::aligned_union\b"),
        description="std::aligned_union",
        remediation="Use `alignas` on a byte buffer instead - aligned_union is deprecated in C++23.",
    ),
    _Rule(
        pattern=re.compile(r"\boperator\s*\"\"\s*_[A-Za-z_][A-Za-z0-9_]*"),
        description="user-defined literal operator",
        remediation="Use a named helper function (e.g. `RgbHex(0xFF0000)` instead of `0xFF0000_rgb`).",
    ),
    _Rule(
        pattern=re.compile(
            r'#\s*include\s*[<"](?:imgui(?:[_/][^>"]*)?\.h|GLFW/[^>"]+|Tracy[A-Za-z]*\.h)[>"]'
        ),
        description="editor-only third-party header outside donner/editor/**",
        remediation=(
            "imgui, GLFW, and Tracy headers are only allowed under donner/editor/** "
            "(plus the //examples:svg_viewer demo binary). If you need this functionality "
            "elsewhere, expose a Donner-internal abstraction in donner/editor/ and depend "
            "on that."
        ),
        # `examples/svg_viewer` covers the canonical //examples:svg_viewer
        # demo binary that wires the editor TextEditor + AsyncSVGDocument
        # together for live demos. `examples/geode_embed` is the Geode
        # embedding reference app - Phase 6 - whose GLFW window demonstrates
        # how a host integrates wgpu-native + Geode. New non-editor consumers
        # of these headers must add an explicit exemption (and a justification).
        exempt_path_prefixes=(
            "donner/editor/",
            "examples/svg_viewer",
            "examples/geode_embed",
        ),
    ),
    _Rule(
        pattern=re.compile(
            r'#\s*include\s*[<"](?:donner/svg/components/|donner/base/xml/components/)'
        ),
        description="ECS-internal component header from outside donner/svg or donner/base",
        remediation=(
            "donner/svg/components/** and donner/base/xml/components/** are ECS-internal "
            "implementation details of the SVG pipeline. External consumers (including "
            "//donner/editor) must go through the public SVGElement / SVGGraphicsElement / "
            "SVGGeometryElement / XMLNode APIs instead. If your callsite legitimately "
            "needs a new piece of state from the ECS, add a public accessor to the "
            "SVGElement subclass rather than reaching into the component."
        ),
        # Only donner/svg and donner/base are allowed to include these
        # component headers. Notably the editor, examples, and any future
        # non-core consumer must go through the SVG public API surface.
        exempt_path_prefixes=("donner/svg/", "donner/base/"),
    ),
    _Rule(
        pattern=re.compile(r"\bAddImageQuad\s*\("),
        description="ImGui AddImageQuad texture presentation",
        remediation=(
            "Do not present document textures through ImGui quadrilateral draws. Compose document "
            "pixels through the editor's direct framebuffer path or into an explicit intermediate "
            "texture so document pixels and direct overlays share the same presentation space."
        ),
    ),
    _Rule(
        pattern=re.compile(r"\.createRenderPipeline\b|\.createComputePipeline\b"),
        description="wgpu pipeline construction outside GeodeDevice",
        remediation=(
            "wgpu-native retains every `wgpu::RenderPipeline` / `wgpu::ComputePipeline` it "
            "ever constructs internally - `wgpuDevicePoll(wait=true)` does NOT drain the "
            "pending-destroy queue for pipelines. Constructing pipelines per-frame or "
            "per-renderer leaks ~100 KB each until the driver's `maxMemoryAllocationCount` "
            "trips (Mesa lavapipe) or the process hangs (Mesa llvmpipe). See issue #575.\n"
            "    Allowed sites: GeodePipeline.cc, GeodeImagePipeline.cc, GeodeFilterEngine.cc. "
            "All pipelines must be owned by `GeodeDevice` so every renderer that shares the "
            "device reuses them. If you need a new pipeline class, add ownership to "
            "`GeodeDevice::Impl` and expose it via a `GeodeDevice` accessor - do not call "
            "`createRenderPipeline` / `createComputePipeline` from anywhere else."
        ),
        # These are the only files that legitimately call the wgpu pipeline
        # constructors - they are the pipeline *classes* themselves, owned
        # end-to-end by GeodeDevice. GeoEncoder_tests / GeodeShaders_tests
        # construct test fixtures that compile pipelines for shader-only
        # validation; they're exempt because the test binary's wgpu::Device
        # goes away at process exit, not during the test run.
        # donner/gpu/** is the Donner-owned GPU runtime (design 0053):
        # `donner::gpu::Device::createRenderPipeline` is that runtime's own validated API, not a
        # wgpu-native pipeline constructor, and its recording backend never touches a driver.
        # Pipeline ownership rules for the new runtime are enforced by its Device API and model
        # tests.
        # GeodeWgpuAdapterDevice.cc is the design-0053 transition adapter: its
        # `onCreateRenderPipeline` hook is the wgpu translation of the runtime's validated
        # `donner::gpu::Device::createRenderPipeline`, and the pipelines it constructs are the
        # same GeodeDevice-owned singletons as before (the pipeline classes cache the handles;
        # the adapter slot-owns the wgpu objects for the device's lifetime).
        exempt_path_prefixes=(
            "donner/svg/renderer/geode/GeodePipeline.cc",
            "donner/svg/renderer/geode/GeodeImagePipeline.cc",
            "donner/svg/renderer/geode/GeodeFilterEngine.cc",
            "donner/svg/renderer/geode/GeodeCheckerboardPipeline.cc",
            "donner/svg/renderer/geode/GeodeWgpuAdapterDevice.cc",
            "donner/svg/renderer/geode/tests/GeoEncoder_tests.cc",
            "donner/svg/renderer/geode/tests/GeodeShaders_tests.cc",
            "donner/gpu/",
        ),
    ),
    _Rule(
        pattern=re.compile(
            r"\.(?:insertBefore|appendChild|replaceChild|removeChild|remove)\s*\(\s*"
            r"[^;\n)]*registry"
        ),
        description="direct TreeComponent structural mutation outside TreeMutation",
        remediation=(
            "Use donner::svg::components::TreeMutation for SVG DOM tree edits so dirty flags, "
            "detached-node lifetime, collection, and mutation revisions stay coherent. "
            "TreeComponent remains only the low-level link container for XML internals, "
            "TreeMutation, and audited shadow-tree internals."
        ),
        exempt_path_prefixes=(
            "donner/base/xml/",
            "donner/svg/components/TreeMutation.cc",
            "donner/svg/components/shadow/ShadowTreeSystem.cc",
            "_tests.",
            "tests/",
        ),
    ),
]


class _BannedCharacter(NamedTuple):
    character: str
    description: str
    remediation: str


_BANNED_CHARACTERS: List[_BannedCharacter] = [
    _BannedCharacter(
        "\u00ad",
        "soft hyphen",
        "Use ASCII '-' in source text; spell intentional Unicode as an escaped code point.",
    ),
    _BannedCharacter(
        "\u2010",
        "typographic hyphen",
        "Use ASCII '-' in source text; spell intentional Unicode as an escaped code point.",
    ),
    _BannedCharacter(
        "\u2011",
        "non-breaking hyphen",
        "Use ASCII '-' in source text; spell intentional Unicode as an escaped code point.",
    ),
    _BannedCharacter(
        "\u2012",
        "figure dash",
        "Use ASCII '-' in source text; spell intentional Unicode as an escaped code point.",
    ),
    _BannedCharacter(
        "\u2013",
        "en dash",
        "Use ASCII '-' in source text; spell intentional Unicode as an escaped code point.",
    ),
    _BannedCharacter(
        "\u2014",
        "em dash",
        "Use ASCII '-' in source text; spell intentional Unicode as an escaped code point.",
    ),
]

_BANNED_CHARACTERS += [
    _BannedCharacter(
        chr(codepoint),
        "smart quote",
        "Use ASCII quotes in source text; spell intentional Unicode as an escaped code point.",
    )
    for codepoint in range(0x2018, 0x2020)
]

_HIDDEN_WHITESPACE_CODEPOINTS = (
    0x00A0,  # NO-BREAK SPACE
    0x1680,  # OGHAM SPACE MARK
    0x180E,  # MONGOLIAN VOWEL SEPARATOR
    *range(0x2000, 0x2010),
    0x2028,  # LINE SEPARATOR
    0x2029,  # PARAGRAPH SEPARATOR
    0x202F,  # NARROW NO-BREAK SPACE
    0x205F,  # MEDIUM MATHEMATICAL SPACE
    0x2060,  # WORD JOINER
    0x3000,  # IDEOGRAPHIC SPACE
    0xFEFF,  # ZERO WIDTH NO-BREAK SPACE
)

_BANNED_CHARACTERS += [
    _BannedCharacter(
        chr(codepoint),
        "hidden Unicode whitespace",
        "Use ASCII space/tab/newline in source text; spell intentional Unicode as an escaped "
        "code point.",
    )
    for codepoint in _HIDDEN_WHITESPACE_CODEPOINTS
]

_BANNED_CHARACTER_BY_CHAR: Dict[str, _BannedCharacter] = {
    character.character: character for character in _BANNED_CHARACTERS
}
_BANNED_CHARACTER_RE = re.compile(
    "[" + "".join(re.escape(character) for character in _BANNED_CHARACTER_BY_CHAR) + "]"
)


_INCLUDE_LINE_RE = re.compile(r"^\s*#\s*include\b")
_STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"')


def _strip_comments_and_strings(text: str) -> str:
    """Remove comments and string literals but preserve line counts.

    `#include "..."` directives are preserved verbatim so include-path rules
    can match against the filename - otherwise the quoted include path would
    be stripped to `""` along with every other string literal.
    """
    # Line comments: replace text after // with spaces, keep newlines
    text = re.sub(r"//[^\n]*", "", text)
    # Block comments: replace with equivalent number of newlines
    def _replace_block(m):
        return "\n" * m.group(0).count("\n")
    text = re.sub(r"/\*.*?\*/", _replace_block, text, flags=re.DOTALL)
    text = re.sub(r'R"\([^)]*\)"', '""', text)
    # Strings: strip them, but leave `#include "..."` lines alone.
    lines = text.split("\n")
    for i, line in enumerate(lines):
        if not _INCLUDE_LINE_RE.match(line):
            lines[i] = _STRING_LITERAL_RE.sub('""', line)
    return "\n".join(lines)


_NOLINT_RE = re.compile(r"//\s*NOLINT\(banned_patterns(?::[^)]*)?\)")
_CPP_EXTS = {".cc", ".cpp", ".h", ".hpp", ".mm"}
_SOURCE_EXTS = _CPP_EXTS | {".bzl", ".css", ".html", ".js", ".py", ".sh", ".ts", ".tsx"}
_EXCLUDED_DIRECTORY_NAMES = {".git", "node_modules", "third_party"}
_EXCLUDED_DIRECTORY_PREFIXES = ("bazel-",)


def _is_suppressed(raw_lines: List[str], line: int) -> bool:
    # Check the match line and up to 2 lines after for a NOLINT marker
    # (clang-format may wrap the signature onto multiple lines).
    for offset in (0, 1, 2):
        idx = line - 1 + offset
        if 0 <= idx < len(raw_lines) and _NOLINT_RE.search(raw_lines[idx]):
            return True
    return False


def _format_codepoint(character: str) -> str:
    return f"U+{ord(character):04X} {unicodedata.name(character, 'UNKNOWN')}"


def _check_banned_characters(raw: str, raw_lines: List[str]) -> List[Tuple[int, str, str]]:
    """Check raw source text for banned typographic or hidden Unicode characters."""
    errors: List[Tuple[int, str, str]] = []
    for match in _BANNED_CHARACTER_RE.finditer(raw):
        line = raw.count("\n", 0, match.start()) + 1
        if _is_suppressed(raw_lines, line):
            continue
        banned_character = _BANNED_CHARACTER_BY_CHAR[match.group(0)]
        errors.append(
            (
                line,
                f"{banned_character.description} ({_format_codepoint(banned_character.character)})",
                banned_character.remediation,
            )
        )

    return errors

_REF_RETURN_RE = re.compile(
    r"""
    ^\s*
    (?:\[\[[^\]]+\]\]\s*)*
    (?:(?:static|inline|constexpr|friend|virtual)\s+)*
    (?P<type>
      (?:const\s+)?
      (?:
        std::optional\s*<\s*
        (?:(?:[A-Za-z_][A-Za-z0-9_]*)::)*(?:XMLQualifiedNameRef|RcStringOrRef)
        \s*>
        |
        (?:(?:[A-Za-z_][A-Za-z0-9_]*)::)*(?:XMLQualifiedNameRef|RcStringOrRef)
      )
      (?:\s*[&*])?
    )
    \s+
    (?P<name>(?:(?:[A-Za-z_][A-Za-z0-9_]*)::)*[A-Za-z_][A-Za-z0-9_]*)
    \s*\((?P<params>[^;{}]*)\)
    (?P<suffix>\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?(?:final\s*)?(?:;|\{))
    """,
    re.MULTILINE | re.VERBOSE,
)

_REF_RETURN_EXEMPT_PATHS = (
    # RcStringOrRef is the view/owning bridge type itself. Its substring helper intentionally
    # preserves the input lifetime model.
    "donner/base/RcStringOrRef.h",
)


def _looks_like_parameter_list(params: str) -> bool:
    """Return true if `params` looks like C++ parameter declarations, not ctor arguments."""
    params = params.strip()
    if not params:
        return True
    if any(token in params for token in ('"', "'", "(", ")")):
        return False

    # Function signatures contain parameter types. Local direct-initialization false positives
    # usually contain literals, variable names, or function calls instead.
    return bool(
        re.search(
            r"\b(?:const|std::|donner::|xml::|svg::|RcString|XMLQualifiedName|"
            r"XMLQualifiedNameRef|RcStringOrRef|string_view|size_t|int|bool|char|auto)\b|[&*]",
            params,
        )
    )


def _check_ref_return_types(
    stripped: str, raw_lines: List[str], posix_path: str
) -> List[Tuple[int, str, str]]:
    if any(prefix in posix_path for prefix in _REF_RETURN_EXEMPT_PATHS):
        return []

    errors: List[Tuple[int, str, str]] = []
    for m in _REF_RETURN_RE.finditer(stripped):
        if not _looks_like_parameter_list(m.group("params")):
            continue

        line = stripped.count("\n", 0, m.start()) + 1
        if _is_suppressed(raw_lines, line):
            continue

        errors.append(
            (
                line,
                f"`{m.group('type').strip()}` return type",
                (
                    "Return an owning value such as XMLQualifiedName or RcString instead. "
                    "`*Ref` types are for input parameters; returning them makes it easy to "
                    "bind a view to storage owned by a temporary."
                ),
            )
        )

    return errors


def check_file(path: Path) -> List[Tuple[int, str, str]]:
    """Check a single file; return list of (line_number, description, remediation).

    Lines marked `// NOLINT(banned_patterns)` or `// NOLINT(banned_patterns: reason)`
    are exempted.
    """
    try:
        raw = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, IOError):
        return []

    raw_lines = raw.splitlines()
    errors = _check_banned_characters(raw, raw_lines)
    if path.suffix not in _CPP_EXTS:
        return sorted(errors)

    stripped = _strip_comments_and_strings(raw)
    posix_path = path.as_posix()

    for rule in _RULES:
        if any(prefix in posix_path for prefix in rule.exempt_path_prefixes):
            continue
        for m in rule.pattern.finditer(stripped):
            line = stripped.count("\n", 0, m.start()) + 1
            if _is_suppressed(raw_lines, line):
                continue
            errors.append((line, rule.description, rule.remediation))

    errors.extend(_check_ref_return_types(stripped, raw_lines, posix_path))

    return sorted(errors)


def _iter_source_files(paths: List[Path]) -> List[Path]:
    result: List[Path] = []
    for p in paths:
        if p.is_file():
            if p.suffix in _SOURCE_EXTS:
                result.append(p)
        elif p.is_dir():
            if p.name in _EXCLUDED_DIRECTORY_NAMES or p.name.startswith(
                _EXCLUDED_DIRECTORY_PREFIXES
            ):
                continue

            for root, directory_names, file_names in os.walk(p):
                directory_names[:] = [
                    name
                    for name in directory_names
                    if name not in _EXCLUDED_DIRECTORY_NAMES
                    and not name.startswith(_EXCLUDED_DIRECTORY_PREFIXES)
                ]
                root_path = Path(root)
                result.extend(
                    root_path / name
                    for name in file_names
                    if Path(name).suffix in _SOURCE_EXTS
                )
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
