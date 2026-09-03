"""Shared Rust path scopes and build-graph tokens for the design 0053 tooling.

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
# third_party/BUILD.wgpu_native_platform) and `WORKSPACE.<name>` covers
# WORKSPACE.bazel/WORKSPACE.bzlmod.
BUILD_GRAPH_FILE_RE = re.compile(
    r"(^|/)(MODULE\.bazel(\.lock)?|BUILD(\.[^/]+)?|WORKSPACE(\.[^/]+)?"
    r"|[^/]+\.bzl|[^/]*\.bazelrc)$"
)

# CMake inputs. The scan reads git-tracked files only, so this matches the
# tracked hand-written and vendored CMake files; Donner's root CMakeLists.txt is
# generated and not tracked, and is therefore out of scope.
CMAKE_FILE_RE = re.compile(r"(^|/)(CMakeLists\.txt|[^/]+\.cmake(\.in)?)$")

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

# CMake reaches Rust through an entirely different vocabulary, so the Bazel
# token list finds nothing there. Matched case-insensitively and only in CMake
# inputs, because `cargo` and `rustc` are ordinary words elsewhere.
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
    """True for Bazel build-graph files and CMake inputs."""
    return bool(BUILD_GRAPH_FILE_RE.search(path) or is_cmake_path(path))


def is_cmake_path(path: str) -> bool:
    """True for CMake inputs, which get their own Rust token vocabulary."""
    return bool(CMAKE_FILE_RE.search(path))


@dataclass(frozen=True)
class RustScopes:
    """The three path scopes design 0053 grants, loaded from rust_allowlist.json.

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
