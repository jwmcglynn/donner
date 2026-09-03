#!/usr/bin/env python3
"""Verifier for the design doc 0053 no-Rust-dependency invariant.

The invariant is a closure property, not a file census: Donner's shipped
artifacts and every non-test dependency closure must contain no Rust compiler
invocation, Cargo execution, or Rust-built library. Two kinds of Rust are
therefore allowed to sit in the tree, each behind its own boundary:

- inert upstream reference source (resvg-test-suite, the tiny-skia snapshot),
  which no build rule may compile or link;
- the tiny-skia cross-validation oracle under
  `third_party/tiny-skia-cpp/tests/rust_ffi/`, a test-only differential fixture
  inside a vendored workspace that Donner's root module never evaluates. It is
  hidden from Bazel by .bazelignore and reachable only from that workspace's own
  test targets, so no Donner target and no non-test target can pull Rust in
  through it.

Categories:

- rust-source-outside-allowlist: Rust source or Cargo metadata outside both
  boundaries (tools/gpu_inventory/rust_allowlist.json).
- rust-build-edge: rules_rust / crate_universe / Rust toolchain references in a
  build-graph or CMake file outside the test-only prefixes.
- rust-fixture-containment: the oracle escaping its boundary, either by taking
  a visibility wider than the vendored workspace's tests or by being named from
  a build file outside that workspace's test tree.
- reference-into-allowlist: a build-graph file compiling or linking the inert
  snapshot. Naming its golden images as test data is not a finding.
- rust-built-archive: build rules that download Rust-built prebuilt archives
  (the wgpu-native release tarballs). These are the Rust that actually ships;
  they leave with the Metal and Linux cutovers.

`--blocking` takes the categories that must fail the run. `--blocking default`
is the set CI enforces: everything except rust-built-archive, which is still
true of the tree and stays report-only until those cutovers land.

This is a static check over build-file text, and it is not the only defense.
From the Donner root, `bazel query 'deps(@tiny-skia-cpp//tests/rust_ffi:tiny_skia_ffi)'`
fails to load, because `@rules_rust` is not visible from a repository Donner
pulls in with a repo rule rather than as a module. A Donner target that reaches
the oracle therefore fails at load time rather than silently linking Rust, and
the edit that would make it load, adding rules_rust to the root module graph, is
itself a rust-build-edge finding here. This verifier catches the attempt early
and names it; Bazel catches it regardless.

The scan covers git-tracked files only. A generated `MODULE.bazel.lock` is
deliberately out of scope: it records the whole transitive Bzlmod graph, and
Donner's reaches rules_rust through protobuf, which declares Rust toolchain
repositories that nothing ever fetches because no Donner target uses a Rust
rule. Reporting that would be reporting the graph instead of the closure, and it
is also invisible to a fresh CI checkout, which has no lockfile at all.

Usage:
  python3 tools/gpu_inventory/check_no_rust_dependencies.py                     # report
  python3 tools/gpu_inventory/check_no_rust_dependencies.py --blocking default  # as CI
  python3 tools/gpu_inventory/check_no_rust_dependencies.py --blocking          # all
  python3 tools/gpu_inventory/check_no_rust_dependencies.py --blocking a,b      # some
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rust_scopes import (  # noqa: E402  (sibling module, path set just above)
    CMAKE_RUST_TOKENS,
    RUST_BUILD_TOKENS,
    RustScopes,
    is_build_graph_path,
    is_cmake_path,
    is_rust_source_path,
    load_rust_scopes,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
ALLOWLIST_RELPATH = "tools/gpu_inventory/rust_allowlist.json"

CATEGORIES = (
    "rust-source-outside-allowlist",
    "rust-build-edge",
    "rust-fixture-containment",
    "reference-into-allowlist",
    "rust-built-archive",
)

# Categories that hold on the tree today and therefore block in CI. The
# wgpu-native archives are still real, so their category is reported and not
# enforced until phases 3 and 4 delete them.
DEFAULT_BLOCKING = tuple(c for c in CATEGORIES if c != "rust-built-archive")

# Rust-built prebuilt archive signatures. wgpu-native releases are compiled from
# Rust; any build rule that downloads one is a Rust-built dependency edge.
RUST_BUILT_ARCHIVE_TOKENS = ("gfx-rs/wgpu-native", "wgpu_native_")

# Labels and paths that name the cross-validation oracle, directly or through
# the two test_utils libraries built on it. A build file outside the vendored
# workspace's test tree that mentions any of these has pulled the oracle into a
# closure the invariant does not allow.
RUST_FIXTURE_TOKENS = (
    "tests/rust_ffi",
    "tiny_skia_ffi",
    "test_utils:rust_reference",
    "test_utils:cross_validator",
)

# Path fragments that name something inside the inert Rust code snapshot. Both
# the directory form and the bazel package-label form (`//...:target`) are
# matched; the trailing `/` and `:` keep `third_party/tiny-skia-cpp` itself from
# matching. The resvg-test-suite allowlist entry is deliberately absent: it
# contains no Rust code, and its SVG/PNG corpus is legitimately test data.
ALLOWLIST_REFERENCE_TOKENS = ("third_party/tiny-skia/", "third_party/tiny-skia:")

# Build attributes that compile or link their contents, versus ones that only
# stage files at runtime. A golden PNG in `data` is test input; the same label
# in `deps` is a build edge.
COMPILE_ATTRIBUTES = frozenset(
    {
        "srcs",
        "hdrs",
        "textual_hdrs",
        "deps",
        "implementation_deps",
        "exports",
        "additional_linker_inputs",
    }
)
DATA_ATTRIBUTES = frozenset({"data", "testdata", "resources", "args", "tags"})

ATTRIBUTE_ASSIGN_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=")
VISIBILITY_ASSIGN_RE = re.compile(r"(?:default_)?visibility\s*=\s*")
VISIBILITY_ENTRY_RE = re.compile(r"\"([^\"]+)\"")
ALIAS_RE = re.compile(r"\balias\s*\(")


@dataclass(frozen=True)
class Finding:
    """One verifier finding: a category, the offending path, and detail text."""

    category: str
    path: str
    detail: str


def code_lines(text: str) -> list[str]:
    """Returns each line with string contents and `#` comments removed.

    Bracket depth has to be counted over code, not over prose. A single
    unbalanced `(` in a comment held the depth open for the rest of the file,
    which shifted every later attribute onto whatever variable happened to be
    open, and a `[` inside a string did the same. Both are one-character edits
    that turned a compiled reference into a staged-data one.
    """
    out: list[str] = []
    quote: str | None = None
    for line in text.splitlines():
        kept: list[str] = []
        index = 0
        while index < len(line):
            if quote is None:
                if line[index] == "#":
                    break
                opener = next(
                    (q for q in ('"""', "'''", '"', "'") if line.startswith(q, index)), None
                )
                if opener is not None:
                    quote = opener
                    index += len(opener)
                else:
                    kept.append(line[index])
                    index += 1
            elif line[index] == "\\":
                index += 2
            elif line.startswith(quote, index):
                index += len(quote)
                quote = None
            else:
                index += 1
        out.append("".join(kept))
        if quote in ('"', "'"):
            # A single-quoted string cannot span lines; a triple-quoted one can.
            quote = None
    return out


def attribute_of_each_line(text: str) -> list[str | None]:
    """Maps every line of a build file to the attribute or variable it sits in.

    Buildifier-formatted build files put one `name = value` per line and indent
    list elements underneath, so tracking the most recent assignment at the
    lowest open bracket depth attributes each element line correctly. A line
    that opens no assignment and sits at depth zero belongs to nothing.
    """
    attributes: list[str | None] = []
    current: str | None = None
    depth = 0
    for line in code_lines(text):
        match = ATTRIBUTE_ASSIGN_RE.match(line)
        if match and depth <= 1:
            current = match.group(1)
        attributes.append(current)
        depth += line.count("[") + line.count("(") - line.count("]") - line.count(")")
        if depth <= 0:
            depth = 0
            if not match:
                current = None
    return attributes


def compile_attributes_referencing(text: str, tokens: tuple[str, ...]) -> set[str]:
    """Returns the attribute names under which `tokens` appear, resolving vars.

    A top-level `_GOLDEN_IMAGES = [...]` list is not itself an attribute, so a
    reference inside it is attributed to every attribute the variable is later
    assigned to. Without that step every extracted constant would read as an
    unattributed, and therefore fail-closed, reference.
    """
    attributes = attribute_of_each_line(text)
    lines = text.splitlines()
    found: set[str] = set()
    for line, attribute in zip(lines, attributes):
        if not any(token in line for token in tokens):
            continue
        if attribute is None:
            found.add("")
            continue
        if attribute in COMPILE_ATTRIBUTES or attribute in DATA_ATTRIBUTES:
            found.add(attribute)
            continue
        # A variable: attribute it to wherever the variable is consumed.
        uses = re.findall(
            r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=[^=\n]*\b" + re.escape(attribute) + r"\b",
            text,
            re.MULTILINE,
        )
        consumers = {use for use in uses if use != attribute}
        found |= consumers if consumers else {""}
    return found


def is_compiled_reference(path: str, text: str, tokens: tuple[str, ...]) -> bool:
    """True when a snapshot reference is compiled or linked rather than staged.

    Fails closed: a reference the attribute scan cannot place, and any reference
    that names Rust source or Cargo metadata directly, counts as compiled.
    """
    for line in text.splitlines():
        if any(token in line for token in tokens) and (
            ".rs" in line or "Cargo.toml" in line or "Cargo.lock" in line
        ):
            return True
    attributes = compile_attributes_referencing(text, tokens)
    return any(attribute not in DATA_ATTRIBUTES for attribute in attributes)


def is_test_tree_visibility(entry: str) -> bool:
    """True only for a package inside the vendored workspace's own `//tests`.

    `@donner//donner/base/tests:__pkg__` leaves the workspace entirely and
    `//src/tiny_skia/tests:__pkg__` is a source package that merely has a
    `tests` segment. An earlier "tests appears somewhere in the path" rule let
    both through.
    """
    if entry.startswith("@"):
        return False
    package = entry.split(":", 1)[0].lstrip("/")
    return package == "tests" or package.startswith("tests/")


def visibility_findings(path: str, text: str) -> list[Finding]:
    """Flags visibility in the oracle's packages that leaves the test tree."""
    findings = []
    for match in VISIBILITY_ASSIGN_RE.finditer(text):
        rest = text[match.end() :].lstrip()
        if not rest.startswith("["):
            findings.append(
                Finding(
                    category="rust-fixture-containment",
                    path=path,
                    detail=(
                        "visibility is not a literal list, so its scope cannot be read here; "
                        "spell the packages out."
                    ),
                )
            )
            continue
        closing = rest.find("]")
        listed = rest[: closing] if closing != -1 else rest
        for entry in VISIBILITY_ENTRY_RE.findall(listed):
            if entry == "//visibility:private" or is_test_tree_visibility(entry):
                continue
            findings.append(
                Finding(
                    category="rust-fixture-containment",
                    path=path,
                    detail=(
                        f"cross-validation oracle's package exposed to {entry}; visibility must "
                        "stay inside the vendored workspace's own //tests tree."
                    ),
                )
            )
    return findings


def reexport_findings(path: str, text: str) -> list[Finding]:
    """Flags indirection that hands the oracle out under a different name.

    An `alias` and a `.bzl` constant both re-export a label without repeating
    it in the consuming file, which is how a Donner target reaches the oracle
    while every Donner BUILD file still reads clean.
    """
    fixture_tokens = sorted(token for token in RUST_FIXTURE_TOKENS if token in text)
    if not fixture_tokens:
        return []
    if path.endswith(".bzl"):
        reason = "a .bzl file hands out"
    elif ALIAS_RE.search(text):
        reason = "an alias re-exports"
    else:
        return []
    return [
        Finding(
            category="rust-fixture-containment",
            path=path,
            detail=(
                f"{reason} the cross-validation oracle ({', '.join(fixture_tokens)}); it must be "
                "named directly by the test targets that use it."
            ),
        )
    ]


def check(files: dict[str, str], scopes: RustScopes) -> list[Finding]:
    """Returns all no-Rust-dependency findings for a path -> content mapping.

    Pure function so unit tests can exercise it with synthetic trees.
    """
    findings: list[Finding] = []

    for path in sorted(files):
        if not is_rust_source_path(path):
            continue
        if scopes.is_inert(path) or scopes.is_test_only(path):
            continue
        findings.append(
            Finding(
                category="rust-source-outside-allowlist",
                path=path,
                detail=(
                    "Rust source or Cargo metadata outside the inert reference allowlist and "
                    "the test-only cross-validation oracle."
                ),
            )
        )

    for path in sorted(files):
        if not is_build_graph_path(path):
            continue
        text = files[path]

        tokens = CMAKE_RUST_TOKENS if is_cmake_path(path) else RUST_BUILD_TOKENS
        haystack = text.lower() if is_cmake_path(path) else text
        rust_tokens = sorted(token for token in tokens if token in haystack)
        if rust_tokens and not scopes.is_test_only(path):
            findings.append(
                Finding(
                    category="rust-build-edge",
                    path=path,
                    detail=(
                        "References a Rust toolchain outside the test-only oracle: "
                        + ", ".join(rust_tokens)
                    ),
                )
            )

        # The oracle's own package and every package allowed to consume it are
        # both places a wider visibility or a re-export would leak it, so both
        # get the containment checks. Everywhere else, naming it at all is the
        # finding.
        if scopes.is_test_only(path) or scopes.is_test_only_consumer(path):
            findings.extend(visibility_findings(path, text))
            findings.extend(reexport_findings(path, text))
        else:
            fixture_tokens = sorted(t for t in RUST_FIXTURE_TOKENS if t in text)
            if fixture_tokens:
                findings.append(
                    Finding(
                        category="rust-fixture-containment",
                        path=path,
                        detail=(
                            "Build file outside the vendored workspace's test tree names the "
                            "cross-validation oracle: " + ", ".join(fixture_tokens)
                        ),
                    )
                )

        archive_tokens = sorted(t for t in RUST_BUILT_ARCHIVE_TOKENS if t in text)
        if archive_tokens:
            findings.append(
                Finding(
                    category="rust-built-archive",
                    path=path,
                    detail="Declares Rust-built prebuilt archives: " + ", ".join(archive_tokens),
                )
            )

        if scopes.is_inert(path):
            continue
        reference_tokens = tuple(t for t in ALLOWLIST_REFERENCE_TOKENS if t in text)
        # A build file may legitimately live next to the snapshot without
        # building it, and may stage its golden images as test data. Only a
        # reference that compiles or links the snapshot is a finding.
        if reference_tokens and is_compiled_reference(path, text, reference_tokens):
            findings.append(
                Finding(
                    category="reference-into-allowlist",
                    path=path,
                    detail=(
                        "Build-graph file compiles or links the inert Rust reference snapshot: "
                        + ", ".join(sorted(reference_tokens))
                    ),
                )
            )

    return findings


def git_tracked_files(repo_root: Path) -> list[str]:
    """Returns all git-tracked paths (repo-relative, sorted)."""
    output = subprocess.check_output(
        ["git", "ls-files", "-z"], cwd=repo_root, text=True, encoding="utf-8"
    )
    return sorted(p for p in output.split("\0") if p)


def collect_scannable_files(repo_root: Path) -> dict[str, str]:
    """Reads tracked files the verifier inspects (Rust sources + build graph)."""
    files: dict[str, str] = {}
    for path in git_tracked_files(repo_root):
        if not (is_rust_source_path(path) or is_build_graph_path(path)):
            continue
        try:
            files[path] = (repo_root / path).read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            files[path] = ""
    return files


def format_report(findings: list[Finding], blocking: tuple[str, ...]) -> str:
    """Renders findings grouped by category, marking which categories block."""
    if not findings:
        return "No Rust dependency edges found.\n"
    lines = [f"{len(findings)} Rust dependency finding(s):", ""]
    by_category: dict[str, list[Finding]] = {}
    for finding in findings:
        by_category.setdefault(finding.category, []).append(finding)
    for category in sorted(by_category):
        group = by_category[category]
        mode = "blocking" if category in blocking else "report-only"
        lines.append(f"[{category}] ({len(group)}, {mode})")
        for finding in group:
            lines.append(f"  {finding.path}: {finding.detail}")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def parse_blocking(value: str | None) -> tuple[str, ...]:
    """Parses --blocking: absent none, bare all, `default` the CI set, else a list."""
    if value is None:
        return ()
    if value == "all":
        return CATEGORIES
    if value == "default":
        return DEFAULT_BLOCKING
    requested = tuple(part.strip() for part in value.split(",") if part.strip())
    unknown = sorted(set(requested) - set(CATEGORIES))
    if unknown:
        raise SystemExit(
            "unknown --blocking category: "
            + ", ".join(unknown)
            + "\nknown categories: "
            + ", ".join(CATEGORIES)
        )
    return requested


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--blocking",
        nargs="?",
        const="all",
        default=None,
        metavar="CATEGORIES",
        help=(
            "Comma-separated categories that fail the run, bare for all, or "
            "`default` for the set CI enforces (" + ", ".join(DEFAULT_BLOCKING) + "). "
            "Known categories: " + ", ".join(CATEGORIES)
        ),
    )
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="Repository root to scan.")
    args = parser.parse_args()

    blocking = parse_blocking(args.blocking)
    scopes = load_rust_scopes(args.root / ALLOWLIST_RELPATH)
    findings = check(collect_scannable_files(args.root), scopes)
    print(format_report(findings, blocking), end="")

    blocked = sorted({f.category for f in findings} & set(blocking))
    if blocked:
        print("FAILED: blocking category with findings: " + ", ".join(blocked))
        return 1
    if findings:
        print("(no blocking category has findings; report-only categories are listed above)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
