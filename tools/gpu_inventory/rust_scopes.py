"""Shared Rust path scopes and build-graph tokens for the GPU inventory tooling.

`generate_gpu_manifests.py` and `check_no_rust_dependencies.py` have to agree
exactly on which files are build graph, which tokens name a Rust build rule, and
which path prefixes carry which permission. They used to hold two copies of that
knowledge with a "keep in sync" comment; a divergence would have shown up as a
verifier finding the manifest never recorded, or the reverse.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

# Files that participate in the Bazel build graph. `BUILD.<name>` covers overlay
# build files applied to external archives (e.g.
# third_party/BUILD.wgpu_native_platform), `WORKSPACE.<name>` covers
# WORKSPACE.bazel/WORKSPACE.bzlmod, and the trailing pattern covers both
# `.bazelrc` and imported rc files such as `ci.bazelrc`.
BUILD_GRAPH_FILE_RE = re.compile(
    r"(^|/)(MODULE\.bazel(\.lock)?|BUILD(\.[^/]+)?|WORKSPACE(\.[^/]+)?"
    r"|[^/]+\.bzl|[^/]*\.bazelrc)$"
)

# CMake inputs. The scan reads git-tracked files only, so this matches the
# tracked hand-written and vendored CMake files. Donner's production
# CMakeLists.txt files are emitted by tools/cmake/gen_cmakelists.py and are
# git-ignored, so they are checked where they exist instead: that generator
# validates its own output against the same token list under `--check`.
CMAKE_FILE_RE = re.compile(r"(^|/)(CMakeLists\.txt|[^/]+\.cmake(\.in)?)$")

# The tracked sources that emit those CMakeLists.txt files. They are read with
# the CMake vocabulary because a Rust command reaching the emitted output has to
# be written here first. Test sources are excluded: they emit nothing, and their
# fixtures legitimately contain the very strings this looks for.
CMAKE_GENERATOR_PREFIX = "tools/cmake/"
CMAKE_GENERATOR_SUFFIXES = (".py", ".json", ".cmake", ".in", ".txt")

# Tokens that name a Rust rule set, toolchain, or crate resolver in Bazel.
RUST_BUILD_TOKENS = (
    "cargo_bazel",
    "crate_universe",
    "rules_rust",
    "rust_binary",
    "rust_library",
    "rust_static_library",
    "rust_toolchains",
)

# Direct invocations of the Rust toolchain, which reach Rust without naming a
# Rust rule at all: a `genrule(cmd = "cargo build ...")` or a `.bzl` action that
# runs `rustc`. Word-boundary matched and case-sensitive, so `cargo_bazel` and
# `Cargo.lock` keep classifying as they do above and only a bare command name
# matches. Scanned outside comments, so prose in a comment cannot trip them.
DIRECT_RUST_TOOL_TOKENS = ("cargo", "rustc", "rustup")
DIRECT_RUST_TOOL_RE = re.compile(r"\b(" + "|".join(DIRECT_RUST_TOOL_TOKENS) + r")\b")

# CMake reaches Rust through an entirely different vocabulary, so the Bazel
# token list finds nothing there. Matched case-insensitively and only in CMake
# inputs and the sources that generate them, because `cargo` and `rustc` are
# ordinary words elsewhere.
CMAKE_RUST_TOKENS = (
    "cargo",
    "corrosion",
    "find_package(rust",
    "findrust",
    "rustc",
)

def is_rust_source_path(path: str) -> bool:
    """True for Rust source and Cargo metadata paths."""
    return path.endswith(".rs") or path.endswith(("Cargo.toml", "Cargo.lock"))


def is_build_graph_path(path: str) -> bool:
    """True for Bazel build-graph files, CMake inputs, and CMake generators."""
    return bool(BUILD_GRAPH_FILE_RE.search(path) or is_cmake_path(path))


def is_cmake_path(path: str) -> bool:
    """True for anything read with the CMake Rust vocabulary.

    That is CMake inputs plus the tracked sources that emit them, minus test
    sources, whose fixtures hold these strings on purpose.
    """
    if CMAKE_FILE_RE.search(path):
        return True
    if not path.startswith(CMAKE_GENERATOR_PREFIX) or path.endswith("_test.py"):
        return False
    return path.endswith(CMAKE_GENERATOR_SUFFIXES)


@dataclass(frozen=True)
class RustScopes:
    """The three path scopes the no-Rust rule grants, from rust_allowlist.json.

    - `inert_reference_prefixes`: reviewed upstream reference source. It may
      exist and may be read by a human; no build rule may compile or link it.
    - `test_only_rust_prefixes`: the tiny-skia cross-validation oracle and the
      vendored workspace module that declares its Rust toolchain. Rust source
      and Rust build rules are permitted here and nowhere else.
    - `test_only_consumer_prefixes`: the only build files allowed to name the
      oracle's targets. This is the containment that keeps Rust out of every
      shipped and non-test dependency closure, which is the actual invariant;
      the oracle's own existence is not.
    """

    inert_reference_prefixes: tuple[str, ...]
    test_only_rust_prefixes: tuple[str, ...]
    test_only_consumer_prefixes: tuple[str, ...]

    def is_inert(self, path: str) -> bool:
        return path.startswith(self.inert_reference_prefixes)

    def is_test_only(self, path: str) -> bool:
        return path.startswith(self.test_only_rust_prefixes)

    def is_test_only_consumer(self, path: str) -> bool:
        return path.startswith(self.test_only_consumer_prefixes)


def load_rust_scopes(allowlist_path: Path) -> RustScopes:
    """Loads the Rust path scopes from tools/gpu_inventory/rust_allowlist.json."""
    data = json.loads(Path(allowlist_path).read_text(encoding="utf-8"))
    return RustScopes(
        inert_reference_prefixes=tuple(data["inertReferencePrefixes"]),
        test_only_rust_prefixes=tuple(data["testOnlyRustPrefixes"]),
        test_only_consumer_prefixes=tuple(data["testOnlyConsumerPrefixes"]),
    )
