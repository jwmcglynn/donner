#!/usr/bin/env python3
"""Verifier for Donner's no-Rust-dependency invariant.

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
- rust-built-archive: a Rust-built GPU archive or reference outside the sole
  checksum-pinned Linux test-only resvg comparison boundary.

`--blocking` takes the categories that must fail the run. `--blocking default`
enforces every category, including unexpected Rust-built archives.

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
    DIRECT_RUST_TOOL_RE,
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

# Every category is blocking. The one retained test-only GPU oracle has an
# exact, checksum-bound allowlist below; no archive category is waived.
DEFAULT_BLOCKING = CATEGORIES

# Rust-built prebuilt archive signatures. wgpu-native releases are compiled from
# Rust; any build rule that downloads one is a Rust-built dependency edge.
RUST_BUILT_ARCHIVE_TOKENS = ("gfx-rs/wgpu-native", "wgpu_native_", "libwgpu_native")

ARCHIVE_FETCH_RULE = "third_party/bazel/non_bcr_deps.bzl"
ARCHIVE_MODULE = "MODULE.bazel"
ARCHIVE_OVERLAY = "third_party/BUILD.wgpu_native_platform"
ARCHIVE_RUNTIME = "third_party/webgpu-cpp/BUILD.bazel"
ARCHIVE_ORACLE_CONSUMER = "donner/svg/renderer/tests/BUILD.bazel"
REQUIRED_ARCHIVE_SITES = tuple(sorted((
    ARCHIVE_FETCH_RULE, ARCHIVE_MODULE, ARCHIVE_OVERLAY, ARCHIVE_RUNTIME,
    ARCHIVE_ORACLE_CONSUMER,
)))
ARCHIVE_NAME_RE = re.compile(r"\bwgpu_native_[a-z0-9_]+\b")
ARCHIVE_STRUCT_RE = re.compile(
    r"struct\(\s*name\s*=\s*\"(?P<name>[^\"]+)\"\s*,\s*"
    r"asset\s*=\s*\"(?P<asset>[^\"]+)\"\s*,\s*"
    r"sha256\s*=\s*\"(?P<sha256>[^\"]*)\"\s*,\s*\)",
    re.DOTALL,
)
RULE_BLOCK_RE = re.compile(r"(?ms)^([A-Za-z_][A-Za-z0-9_]*)\(\s*\n(.*?)^\)\s*$")
RULE_NAME_RE = re.compile(r"\bname\s*=\s*[\"']([^\"']+)[\"']")

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
VISIBILITY_ENTRY_RE = re.compile(r"\"([^\"]*)\"|'([^']*)'")
ALIAS_RE = re.compile(r"\balias\s*\(")


@dataclass(frozen=True)
class Finding:
    """One verifier finding: a category, the offending path, and detail text."""

    category: str
    path: str
    detail: str


def _scan_lines(text: str, keep_strings: bool) -> list[str]:
    """Splits `text` into lines with comments, and optionally strings, removed.

    One quote-state machine serves both readers. Bracket depth has to be counted
    over code alone, because a single unbalanced `(` in a comment held the depth
    open for the rest of a file and shifted every later attribute. Token scans
    want the opposite half: a `genrule(cmd = "cargo build")` is a real Rust
    invocation, while the word `cargo` in a comment is prose.
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
                if keep_strings:
                    kept.append(line[index])
                index += 1
        out.append("".join(kept))
        if quote in ('"', "'"):
            # A single-quoted string cannot span lines; a triple-quoted one can.
            quote = None
    return out


def code_lines(text: str) -> list[str]:
    """Each line with string contents and `#` comments removed."""
    return _scan_lines(text, keep_strings=False)


def text_without_comments(text: str) -> str:
    """The file with `#` comments removed and string contents intact."""
    return "\n".join(_scan_lines(text, keep_strings=True))


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


# What may legitimately follow a visibility list: the attribute separator, the
# end of the enclosing call, a trailing comment, or the end of the line.
# Anything else means the list is one operand of a larger expression, and the
# expression is not readable here.
VISIBILITY_LIST_TERMINATORS = (",", ")", "#", "\n")

# The only visibility targets that name a fixed set of packages. A package_group
# label such as `//tests/policy:everyone` also lives under the test tree but its
# contents are declared elsewhere and can include `//...`.
VISIBILITY_TARGETS = ("__pkg__", "__subpackages__")


def is_test_tree_visibility(entry: str) -> bool:
    """True only for a fixed package under the vendored workspace's `//tests`.

    `@donner//donner/base/tests:__pkg__` leaves the workspace entirely,
    `//src/tiny_skia/tests:__pkg__` is a source package that merely has a
    `tests` segment, and `//tests/policy:everyone` is a package_group whose
    membership this file does not state. None of the three is containment.
    """
    if entry.startswith("@") or ":" not in entry:
        return False
    package, _, target = entry.partition(":")
    package = package.lstrip("/")
    if target not in VISIBILITY_TARGETS:
        return False
    return package == "tests" or package.startswith("tests/")


def visibility_entries(listed: str) -> list[str] | None:
    """Decodes a visibility list body, or None if any element is not a literal.

    Both quote forms appear in the wild, and an element that is neither is a
    name or a call whose value is not readable here.
    """
    entries = []
    for element in listed.split(","):
        element = element.strip()
        if not element:
            continue
        match = VISIBILITY_ENTRY_RE.fullmatch(element)
        if match is None:
            return None
        entries.append(match.group(1) or match.group(2))
    return entries


def visibility_findings(path: str, text: str) -> list[Finding]:
    """Flags visibility in the oracle's packages that leaves the test tree."""

    def unreadable() -> Finding:
        return Finding(
            category="rust-fixture-containment",
            path=path,
            detail=(
                "visibility is not a literal list, so its scope cannot be read here; "
                "spell the packages out."
            ),
        )

    findings = []
    for match in VISIBILITY_ASSIGN_RE.finditer(text):
        rest = text[match.end() :].lstrip()
        closing = rest.find("]") if rest.startswith("[") else -1
        if closing != -1:
            after = rest[closing + 1 :].lstrip(" \t")
            if after and not after.startswith(VISIBILITY_LIST_TERMINATORS):
                # `["//tests:__subpackages__"] + ["//visibility:public"]` reads
                # as contained if the scan stops at the first `]`.
                closing = -1
        entries = visibility_entries(rest[1:closing]) if closing != -1 else None
        if entries is None:
            findings.append(unreadable())
            continue
        for entry in entries:
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


def tokens_in(text: str, tokens: tuple[str, ...]) -> list[str]:
    """The members of `tokens` present in `text`, sorted for stable messages."""
    return sorted(token for token in tokens if token in text)


def matched_rust_build_tokens(path: str, text: str) -> list[str]:
    """Rust toolchain references in `text`, read in that file's vocabulary.

    Bazel says `rules_rust`; CMake says `corrosion_import_crate` or
    `cargo build`, case however the author felt. One list finds nothing in the
    other's files.

    A Bazel file can also reach Rust without naming a Rust rule, by running the
    toolchain directly from a `genrule` command or a `.bzl` action, so bare
    command names count there too. Those are matched outside comments, where a
    command can actually run.
    """
    if is_cmake_path(path):
        return tokens_in(text.lower(), CMAKE_RUST_TOKENS)
    direct = set(DIRECT_RUST_TOOL_RE.findall(text_without_comments(text)))
    return sorted(set(tokens_in(text, RUST_BUILD_TOKENS)) | direct)


def rust_source_scope_findings(path: str, scopes: RustScopes) -> list[Finding]:
    """Flags Rust source that is neither inert reference nor the test oracle."""
    if not is_rust_source_path(path) or scopes.is_inert(path) or scopes.is_test_only(path):
        return []
    return [
        Finding(
            category="rust-source-outside-allowlist",
            path=path,
            detail=(
                "Rust source or Cargo metadata outside the inert reference allowlist and "
                "the test-only cross-validation oracle."
            ),
        )
    ]


def rust_build_edge_findings(path: str, text: str, scopes: RustScopes) -> list[Finding]:
    """Flags a Rust toolchain reference outside the test-only oracle."""
    rust_tokens = matched_rust_build_tokens(path, text)
    if not rust_tokens or scopes.is_test_only(path):
        return []
    return [
        Finding(
            category="rust-build-edge",
            path=path,
            detail=(
                "References a Rust toolchain outside the test-only oracle: "
                + ", ".join(rust_tokens)
            ),
        )
    ]


def fixture_containment_findings(path: str, text: str, scopes: RustScopes) -> list[Finding]:
    """Flags the oracle escaping the vendored workspace's own test tree.

    Inside that tree, containment is about how the oracle is exposed: how wide
    its visibility reaches and whether anything re-exports it. Outside it,
    naming the oracle at all is the finding.
    """
    if scopes.is_test_only(path) or scopes.is_test_only_consumer(path):
        return visibility_findings(path, text) + reexport_findings(path, text)
    fixture_tokens = tokens_in(text, RUST_FIXTURE_TOKENS)
    if not fixture_tokens:
        return []
    return [
        Finding(
            category="rust-fixture-containment",
            path=path,
            detail=(
                "Build file outside the vendored workspace's test tree names the "
                "cross-validation oracle: " + ", ".join(fixture_tokens)
            ),
        )
    ]


def _archive_finding(path: str, detail: str) -> list[Finding]:
    return [Finding("rust-built-archive", path, detail)]


def _archive_rules(text: str) -> dict[str, tuple[str, str, str]]:
    """Read only literal wgpu-native release structs, never infer a pin from a URL."""
    entries = {}
    for match in ARCHIVE_STRUCT_RE.finditer(text):
        name, asset, sha = match.group("name", "asset", "sha256")
        if name.startswith("wgpu_native_") or asset.startswith("wgpu-"):
            entries[name] = (name, asset, sha)
    return entries


def _active_archive_text(text: str) -> str:
    """Exclude a BUILD file's opening docstring and comments, retaining rule strings."""
    stripped = text.lstrip()
    for quote in ('"""', "'''"):
        if stripped.startswith(quote):
            end = stripped.find(quote, len(quote))
            if end >= 0:
                stripped = stripped[end + len(quote):]
            break
    return text_without_comments(stripped)


def _archive_allowlist(scopes: RustScopes) -> dict[str, tuple[str, str, str]]:
    return {name: (name, asset, sha) for name, asset, sha in scopes.test_only_gpu_oracle_archives}


def _rule_blocks(text: str) -> list[tuple[str, str, str]]:
    """Top-level BUILD calls in buildifier's one-rule-per-block form."""
    result = []
    for kind, body in RULE_BLOCK_RE.findall(text):
        name = RULE_NAME_RE.search(body)
        if name:
            result.append((kind, name.group(1), text_without_comments(body)))
    return result


def _rule_has_edge(body: str, token: str, attribute: str) -> bool:
    return any(token in line and owner == attribute
               for line, owner in zip(body.splitlines(), attribute_of_each_line(body)))


def _archive_fetch_findings(path: str, text: str, scopes: RustScopes) -> list[Finding]:
    expected = _archive_allowlist(scopes)
    required_names = {"wgpu_native_linux_aarch64", "wgpu_native_linux_x86_64"}
    if len(scopes.test_only_gpu_oracle_archives) != 2 or set(expected) != required_names or any(
        not re.fullmatch(r"[0-9a-f]{64}", item[2]) or item[2] == "0" * 64 or
        item[1] != item[0].replace("wgpu_native_", "wgpu-").replace("_", "-", 1) + "-release.zip"
        for item in expected.values()
    ):
        return _archive_finding(path, "Linux oracle archive allowlist is incomplete or unpinned")
    actual = _archive_rules(text)
    active_names = set(ARCHIVE_NAME_RE.findall(_active_archive_text(text))) - {
        "wgpu_native_platform",
    }
    if actual != expected or len([
        match for match in ARCHIVE_STRUCT_RE.finditer(text)
        if match.group("name").startswith("wgpu_native_") or match.group("asset").startswith("wgpu-")
    ]) != len(expected) or active_names != required_names:
        return _archive_finding(
            path, "Rust archive fetch rules differ from the two reviewed Linux SHA-256 pins: "
            + ", ".join(sorted(actual)),
        )
    active = text_without_comments(text)
    if not all(token in active for token in (
        "gfx-rs/wgpu-native/releases/download", "sha256 = p.sha256",
        "build_file = //third_party:BUILD.wgpu_native_platform",
    )):
        return _archive_finding(path, "Linux oracle fetch lacks its pinned URL, SHA-256, or overlay")
    return []


def _archive_module_findings(path: str, text: str, scopes: RustScopes) -> list[Finding]:
    active = _active_archive_text(text)
    match = re.search(r"use_repo\(\s*non_bcr_deps\s*,(.*?)\)", active, re.DOTALL)
    names = ARCHIVE_NAME_RE.findall(match.group(1)) if match else []
    if len(names) != 2 or set(names) != set(_archive_allowlist(scopes)) or \
       ARCHIVE_NAME_RE.findall(active) != names:
        return _archive_finding(path, "root module must expose only the two Linux oracle archives")
    return []


def _archive_overlay_findings(path: str, text: str) -> list[Finding]:
    rules = [(kind, name, body) for kind, name, body in _rule_blocks(text)
             if "libwgpu_native" in body or name == "wgpu_native"]
    if len(rules) != 1 or rules[0][0] != "cc_library" or rules[0][1] != "wgpu_native":
        return _archive_finding(path, "archive overlay must define exactly one wgpu_native library")
    body = rules[0][2]
    if not re.search(r"\btestonly\s*=\s*(?:True|1)\b", body) or \
       "lib/libwgpu_native.so" not in body or \
       "lib/libwgpu_native.dylib" in body or "lib/libwgpu_native.a" in body:
        return _archive_finding(path, "archive overlay must be test-only and Linux .so only")
    return []


def _archive_runtime_findings(path: str, text: str, scopes: RustScopes) -> list[Finding]:
    blocks = {name: (kind, body) for kind, name, body in _rule_blocks(text)}
    expected_names = set(_archive_allowlist(scopes))
    active = _active_archive_text(text)
    repo_names = set(re.findall(r"@(?P<name>wgpu_native_[A-Za-z0-9_]+)//:wgpu_native", active))
    if repo_names != expected_names or "wgpu_native_macos" in active:
        return _archive_finding(path, "reference runtime must name only Linux archive repositories")
    for name in ("webgpu_cpp", "wgpu_native_reference_runtime", "wgpu_native_platform",
                 "wgpu_native_linux"):
        rule = blocks.get(name)
        if rule is None or not re.search(r"\btestonly\s*=\s*(?:True|1)\b", rule[1]):
            return _archive_finding(path, f"{name} must be a test-only reference target")
    reference = blocks["wgpu_native_reference_runtime"][1]
    if "@platforms//os:linux" not in reference or ":webgpu_cpp" not in reference or \
       "//visibility:public" in reference or \
       "//donner/svg/renderer/tests:__pkg__" not in reference:
        return _archive_finding(path, "reference runtime lacks Linux-only compatibility or narrow test visibility")
    if not _rule_has_edge(blocks["webgpu_cpp"][1], ":wgpu_native_platform", "deps") or \
       not _rule_has_edge(blocks["wgpu_native_platform"][1], ":wgpu_native_linux", "actual") or \
       any(not _rule_has_edge(blocks["wgpu_native_linux"][1],
                              "@" + name + "//:wgpu_native", "actual")
           for name in expected_names):
        return _archive_finding(path, "test-only wrapper-to-Linux-archive alias chain is incomplete")
    return []


def _archive_consumer_findings(path: str, text: str) -> list[Finding]:
    consumers = [(kind, name, body) for kind, name, body in _rule_blocks(text)
                 if "wgpu_native_reference_runtime" in body]
    if len(consumers) != 1 or consumers[0][0] != "donner_cc_test" or \
       consumers[0][1] != "resvg_test_suite_wgpu_reference_linux_impl" or \
       "@platforms//os:linux" not in consumers[0][2]:
        return _archive_finding(path, "only the Linux resvg test may consume the reference runtime")
    return []


def rust_built_archive_findings(path: str, text: str, scopes: RustScopes) -> list[Finding]:
    """Allow only the checksum-pinned Linux test oracle's complete build boundary."""
    # These five files are the complete allowed boundary. Inspect them even if
    # their last Rust token was deleted: absence of the required oracle edge is
    # a failure, not a clean scan.
    if path == ARCHIVE_FETCH_RULE:
        return _archive_fetch_findings(path, text, scopes)
    if path == ARCHIVE_MODULE:
        return _archive_module_findings(path, text, scopes)
    if path == ARCHIVE_OVERLAY:
        return _archive_overlay_findings(path, text)
    if path == ARCHIVE_RUNTIME:
        return _archive_runtime_findings(path, text, scopes)
    if path == ARCHIVE_ORACLE_CONSUMER:
        return _archive_consumer_findings(path, text)
    active = text_without_comments(text)
    if not tokens_in(active, RUST_BUILT_ARCHIVE_TOKENS):
        return []
    attributes = attribute_of_each_line(text)
    for line, attribute in zip(active.splitlines(), attributes):
        if tokens_in(line, RUST_BUILT_ARCHIVE_TOKENS) and \
           attribute not in {"forbidden", "required", "tags"}:
            return _archive_finding(path, "unexpected Rust-built archive or reference edge")
    return []


def inert_reference_findings(path: str, text: str, scopes: RustScopes) -> list[Finding]:
    """Flags a build file that compiles or links the inert Rust snapshot.

    A build file may legitimately live next to the snapshot without building
    it, and may stage its golden images as test data. Only a reference that
    compiles or links is a finding.
    """
    if scopes.is_inert(path):
        return []
    reference_tokens = tuple(t for t in ALLOWLIST_REFERENCE_TOKENS if t in text)
    if not reference_tokens or not is_compiled_reference(path, text, reference_tokens):
        return []
    return [
        Finding(
            category="reference-into-allowlist",
            path=path,
            detail=(
                "Build-graph file compiles or links the inert Rust reference snapshot: "
                + ", ".join(sorted(reference_tokens))
            ),
        )
    ]


# Every rule a build-graph file is held to, applied in this order. Adding a
# category means adding a function here and a name to CATEGORIES, not another
# branch in check().
BUILD_GRAPH_RULES = (
    rust_build_edge_findings,
    fixture_containment_findings,
    rust_built_archive_findings,
    inert_reference_findings,
)


def check(files: dict[str, str], scopes: RustScopes) -> list[Finding]:
    """Returns all no-Rust-dependency findings for a path -> content mapping.

    Pure function so unit tests can exercise it with synthetic trees.
    """
    findings: list[Finding] = []
    for path in sorted(files):
        findings.extend(rust_source_scope_findings(path, scopes))
    for path in sorted(files):
        if not is_build_graph_path(path):
            continue
        for rule in BUILD_GRAPH_RULES:
            findings.extend(rule(path, files[path], scopes))
    return findings


def check_tracked_tree(files: dict[str, str], scopes: RustScopes) -> list[Finding]:
    """Apply the per-file rules and require every test-oracle boundary file to exist."""
    findings = check(files, scopes)
    for path in REQUIRED_ARCHIVE_SITES:
        if path not in files:
            findings.extend(_archive_finding(path, "required Linux test-oracle boundary file is missing"))
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
    findings = check_tracked_tree(collect_scannable_files(args.root), scopes)
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
