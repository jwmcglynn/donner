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
  - No imgui / GLFW / Tracy headers outside `donner/editor/**` (path-scoped)

Usage:
  python3 tools/check_banned_patterns.py            # Check all files
  python3 tools/check_banned_patterns.py FILE...    # Check specific files
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import List, NamedTuple, Tuple


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
            "Use std::int64_t / std::uint64_t (width-portable) — long long is long on macOS but "
            "long-long on Linux, causing template specialization collisions (see PR #415)."
        ),
    ),
    _Rule(
        pattern=re.compile(r"\bstd::aligned_storage\b"),
        description="std::aligned_storage",
        remediation=(
            "Use `alignas(T) std::byte buffer[N * sizeof(T)]` instead — aligned_storage is "
            "deprecated in C++23."
        ),
    ),
    _Rule(
        pattern=re.compile(r"\bstd::aligned_union\b"),
        description="std::aligned_union",
        remediation="Use `alignas` on a byte buffer instead — aligned_union is deprecated in C++23.",
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
        # embedding reference app — Phase 6 — whose GLFW window demonstrates
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
        pattern=re.compile(r"\.createRenderPipeline\b|\.createComputePipeline\b"),
        description="wgpu pipeline construction outside GeodeDevice",
        remediation=(
            "wgpu-native retains every `wgpu::RenderPipeline` / `wgpu::ComputePipeline` it "
            "ever constructs internally — `wgpuDevicePoll(wait=true)` does NOT drain the "
            "pending-destroy queue for pipelines. Constructing pipelines per-frame or "
            "per-renderer leaks ~100 KB each until the driver's `maxMemoryAllocationCount` "
            "trips (Mesa lavapipe) or the process hangs (Mesa llvmpipe). See issue #575.\n"
            "    Allowed sites: GeodePipeline.cc, GeodeImagePipeline.cc, GeodeFilterEngine.cc. "
            "All pipelines must be owned by `GeodeDevice` so every renderer that shares the "
            "device reuses them. If you need a new pipeline class, add ownership to "
            "`GeodeDevice::Impl` and expose it via a `GeodeDevice` accessor — do not call "
            "`createRenderPipeline` / `createComputePipeline` from anywhere else."
        ),
        # These are the only files that legitimately call the wgpu pipeline
        # constructors — they are the pipeline *classes* themselves, owned
        # end-to-end by GeodeDevice. GeoEncoder_tests / GeodeShaders_tests
        # construct test fixtures that compile pipelines for shader-only
        # validation; they're exempt because the test binary's wgpu::Device
        # goes away at process exit, not during the test run.
        exempt_path_prefixes=(
            "donner/svg/renderer/geode/GeodePipeline.cc",
            "donner/svg/renderer/geode/GeodeImagePipeline.cc",
            "donner/svg/renderer/geode/GeodeFilterEngine.cc",
            "donner/svg/renderer/geode/tests/GeoEncoder_tests.cc",
            "donner/svg/renderer/geode/tests/GeodeShaders_tests.cc",
        ),
    ),
    _Rule(
        pattern=re.compile(r"\bSVGParser::ParseSVG\b"),
        description="SVGParser::ParseSVG call outside allowed hosts",
        remediation=(
            "Desktop editor host code must route every parse through EditorBackendClient. "
            "Only the backend library, parser/engine code, the EditorBackendClient_InProcess "
            "path (WASM), and tests may call SVGParser::ParseSVG directly."
        ),
        exempt_path_prefixes=(
            "donner/svg/",
            "donner/benchmarks/",
            "donner/editor/sandbox/parser_child_main.cc",
            "donner/editor/sandbox/editor_backend_main.cc",
            "donner/editor/sandbox/EditorBackendCore.",
            "donner/editor/backend_lib/",
            "donner/editor/EditorBackendClient_InProcess.cc",
            "donner/editor/sandbox/tests/",
            "donner/editor/tests/",
            "donner/editor/backend_lib/tests/",
            "donner/editor/repro/tests/",
            "examples/",
            "tools/",
        ),
    ),
]


_INCLUDE_LINE_RE = re.compile(r"^\s*#\s*include\b")
_STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"')


def _strip_comments_and_strings(text: str) -> str:
    """Remove comments and string literals but preserve line counts.

    `#include "..."` directives are preserved verbatim so include-path rules
    can match against the filename — otherwise the quoted include path would
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
    posix_path = path.as_posix()

    errors: List[Tuple[int, str, str]] = []
    for rule in _RULES:
        if any(prefix in posix_path for prefix in rule.exempt_path_prefixes):
            continue
        for m in rule.pattern.finditer(stripped):
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
            errors.append((line, rule.description, rule.remediation))

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
