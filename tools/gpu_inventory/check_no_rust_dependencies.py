#!/usr/bin/env python3
"""Report-only verifier for the design doc 0053 no-Rust-dependency invariant.

Scans the git-tracked tree for Rust dependency edges that must be removed before
the design 0053 cutover completes:

- rust-source-outside-allowlist: Rust source or Cargo metadata outside the inert
  third-party reference allowlist (tools/gpu_inventory/rust_allowlist.json).
- rust-build-edge: rules_rust / crate_universe / Rust toolchain references in
  build-graph files (MODULE.bazel, MODULE.bazel.lock, BUILD files, .bzl files).
- rust-built-archive: build rules that download Rust-built prebuilt archives
  (the wgpu-native release tarballs).
- active-rust-fixture: the tiny-skia Rust FFI cross-validation crate, which is an
  active Rust build target rather than inert reference material.
- reference-into-allowlist: build-graph files outside the allowlist that
  reference paths inside the inert Rust reference snapshot.

The verifier currently runs in REPORT-ONLY mode (design 0053 phase 0): it prints
findings and exits 0. Phase 6 switches it to --blocking, where any finding fails.

Usage:
  python3 tools/gpu_inventory/check_no_rust_dependencies.py             # report
  python3 tools/gpu_inventory/check_no_rust_dependencies.py --blocking  # gate
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ALLOWLIST_PATH = REPO_ROOT / "tools/gpu_inventory/rust_allowlist.json"

# Files that participate in the build graph. Kept in sync with
# generate_gpu_manifests.py (same definition, same rationale).
BUILD_GRAPH_FILE_RE = re.compile(
    r"(^|/)(MODULE\.bazel|MODULE\.bazel\.lock|BUILD\.bazel|BUILD|WORKSPACE(\.bazel)?|[^/]+\.bzl|\.bazelrc)$"
)

RUST_BUILD_TOKENS = (
    "cargo_bazel",
    "crate_universe",
    "rules_rust",
    "rust_binary",
    "rust_library",
    "rust_static_library",
    "rust_toolchains",
)

# Rust-built prebuilt archive signatures. wgpu-native releases are compiled from
# Rust; any build rule that downloads one is a Rust-built dependency edge.
RUST_BUILT_ARCHIVE_TOKENS = ("gfx-rs/wgpu-native", "wgpu_native_")

# The active Rust FFI cross-validation fixture (design 0053 phase 6 removes it).
RUST_FFI_PREFIX = "third_party/tiny-skia-cpp/tests/rust_ffi/"

# Path fragments that indicate a build-graph reference into the inert snapshot.
ALLOWLIST_REFERENCE_TOKENS = ("third_party/tiny-skia/",)

# Generated build state that is not git-tracked but must still be free of Rust
# edges (design 0053: the verifier scans "generated build state" too). Scanned
# from the working tree when present.
GENERATED_BUILD_STATE_PATHS = (
    "MODULE.bazel.lock",
    "third_party/tiny-skia-cpp/MODULE.bazel.lock",
)


@dataclass(frozen=True)
class Finding:
    """One verifier finding: a category, the offending path, and detail text."""

    category: str
    path: str
    detail: str


def is_rust_source_path(path: str) -> bool:
    """True for Rust source and Cargo metadata paths."""
    return path.endswith(".rs") or path.endswith(("Cargo.toml", "Cargo.lock"))


def check(files: dict[str, str], allowlist_prefixes: list[str]) -> list[Finding]:
    """Returns all no-Rust-dependency findings for a path -> content mapping.

    Pure function so unit tests can exercise it with synthetic trees.
    """
    findings: list[Finding] = []

    def allowlisted(path: str) -> bool:
        return any(path.startswith(prefix) for prefix in allowlist_prefixes)

    for path in sorted(files):
        if is_rust_source_path(path) and not allowlisted(path):
            findings.append(
                Finding(
                    category="rust-source-outside-allowlist",
                    path=path,
                    detail="Rust source or Cargo metadata outside the inert reference allowlist.",
                )
            )
        if path.startswith(RUST_FFI_PREFIX):
            findings.append(
                Finding(
                    category="active-rust-fixture",
                    path=path,
                    detail="Active Rust FFI cross-validation fixture (removed in design 0053 phase 6).",
                )
            )

    for path in sorted(files):
        if not BUILD_GRAPH_FILE_RE.search(path):
            continue
        text = files[path]
        rust_tokens = sorted(t for t in RUST_BUILD_TOKENS if t in text)
        if rust_tokens:
            findings.append(
                Finding(
                    category="rust-build-edge",
                    path=path,
                    detail="References Rust build rules: " + ", ".join(rust_tokens),
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
        if not allowlisted(path):
            reference_tokens = sorted(t for t in ALLOWLIST_REFERENCE_TOKENS if t in text)
            # A build file may legitimately live next to the snapshot (e.g. the
            # tiny-skia-cpp package) without building it; only flag references
            # that name paths inside the snapshot.
            if reference_tokens:
                findings.append(
                    Finding(
                        category="reference-into-allowlist",
                        path=path,
                        detail="Build-graph file references the inert Rust reference snapshot: "
                        + ", ".join(reference_tokens),
                    )
                )

    return findings


def git_tracked_files(repo_root: Path) -> list[str]:
    """Returns all git-tracked paths (repo-relative, sorted)."""
    output = subprocess.check_output(["git", "ls-files", "-z"], cwd=repo_root, text=True)
    return sorted(p for p in output.split("\0") if p)


def collect_scannable_files(repo_root: Path) -> dict[str, str]:
    """Reads tracked files the verifier inspects (Rust sources + build graph)."""
    files: dict[str, str] = {}
    for path in git_tracked_files(repo_root):
        if not (
            is_rust_source_path(path)
            or path.startswith(RUST_FFI_PREFIX)
            or BUILD_GRAPH_FILE_RE.search(path)
        ):
            continue
        try:
            files[path] = (repo_root / path).read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            files[path] = ""
    for path in GENERATED_BUILD_STATE_PATHS:
        full = repo_root / path
        if path not in files and full.exists():
            try:
                files[path] = full.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                files[path] = ""
    return files


def load_allowlist_prefixes(allowlist_path: Path) -> list[str]:
    """Loads the inert-reference allowlist path prefixes."""
    data = json.loads(allowlist_path.read_text(encoding="utf-8"))
    return list(data["inertReferencePrefixes"])


def format_report(findings: list[Finding]) -> str:
    """Renders findings grouped by category with counts."""
    if not findings:
        return "No Rust dependency edges found."
    lines = [f"{len(findings)} Rust dependency finding(s):", ""]
    by_category: dict[str, list[Finding]] = {}
    for finding in findings:
        by_category.setdefault(finding.category, []).append(finding)
    for category in sorted(by_category):
        group = by_category[category]
        lines.append(f"[{category}] ({len(group)})")
        for finding in group:
            lines.append(f"  {finding.path}: {finding.detail}")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--blocking",
        action="store_true",
        help="Exit nonzero when findings exist (design 0053 phase 6 mode).",
    )
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="Repository root to scan.")
    args = parser.parse_args()

    allowlist_prefixes = load_allowlist_prefixes(args.root / "tools/gpu_inventory/rust_allowlist.json")
    findings = check(collect_scannable_files(args.root), allowlist_prefixes)
    print(format_report(findings), end="")

    if findings and args.blocking:
        return 1
    if findings:
        print("(report-only mode: exiting 0; design 0053 phase 6 switches to --blocking)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
