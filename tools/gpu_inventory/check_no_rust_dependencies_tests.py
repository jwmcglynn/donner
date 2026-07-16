"""Unit tests for check_no_rust_dependencies.py verifier logic."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_no_rust_dependencies as verifier

ALLOWLIST = [
    "third_party/resvg-test-suite/",
    "third_party/tiny-skia-cpp/third_party/tiny-skia/",
]


def categories(findings):
    return sorted({f.category for f in findings})


class CheckTest(unittest.TestCase):
    def test_clean_tree_has_no_findings(self):
        files = {
            "MODULE.bazel": 'bazel_dep(name = "googletest", version = "1.15")\n',
            "donner/base/BUILD.bazel": 'donner_cc_library(name = "base")\n',
        }
        self.assertEqual(verifier.check(files, ALLOWLIST), [])

    def test_allowlisted_rust_source_is_not_flagged(self):
        files = {
            "third_party/tiny-skia-cpp/third_party/tiny-skia/src/lib.rs": "fn f() {}",
            "third_party/tiny-skia-cpp/third_party/tiny-skia/Cargo.toml": "[package]",
        }
        self.assertEqual(verifier.check(files, ALLOWLIST), [])

    def test_rust_source_outside_allowlist_is_flagged(self):
        files = {"donner/experiment/helper.rs": "fn f() {}"}
        findings = verifier.check(files, ALLOWLIST)
        self.assertEqual(categories(findings), ["rust-source-outside-allowlist"])
        self.assertEqual(findings[0].path, "donner/experiment/helper.rs")

    def test_rust_build_edge_in_module_bazel(self):
        files = {"MODULE.bazel": 'bazel_dep(name = "rules_rust", version = "0.71.3")\n'}
        findings = verifier.check(files, ALLOWLIST)
        self.assertEqual(categories(findings), ["rust-build-edge"])
        self.assertIn("rules_rust", findings[0].detail)

    def test_rust_build_edge_in_generated_lockfile(self):
        files = {"MODULE.bazel.lock": '{"key": "@@rules_rust+//crate_universe"}'}
        findings = verifier.check(files, ALLOWLIST)
        self.assertEqual(categories(findings), ["rust-build-edge"])

    def test_rust_built_archive_download_is_flagged(self):
        files = {
            "third_party/bazel/non_bcr_deps.bzl": (
                'url = "https://github.com/gfx-rs/wgpu-native/releases/download/v1/x.zip"\n'
            )
        }
        findings = verifier.check(files, ALLOWLIST)
        self.assertEqual(categories(findings), ["rust-built-archive"])

    def test_active_rust_fixture_is_flagged_per_file(self):
        files = {
            "third_party/tiny-skia-cpp/tests/rust_ffi/BUILD.bazel": (
                'load("@rules_rust//rust:defs.bzl", "rust_static_library")\n'
            ),
            "third_party/tiny-skia-cpp/tests/rust_ffi/src/lib.rs": "fn f() {}",
        }
        findings = verifier.check(files, ALLOWLIST)
        self.assertEqual(
            categories(findings),
            ["active-rust-fixture", "rust-build-edge", "rust-source-outside-allowlist"],
        )

    def test_reference_into_allowlist_from_build_file(self):
        files = {
            "third_party/tiny-skia-cpp/tests/integration/BUILD.bazel": (
                'data = ["//third_party/tiny-skia/tests:png"]\n'
            )
        }
        findings = verifier.check(files, ALLOWLIST)
        self.assertEqual(categories(findings), ["reference-into-allowlist"])

    def test_build_file_inside_allowlist_is_not_a_reference_finding(self):
        files = {
            "third_party/tiny-skia-cpp/third_party/tiny-skia/BUILD.bazel": (
                '# inert snapshot references third_party/tiny-skia/ internally\n'
            )
        }
        self.assertEqual(categories(verifier.check(files, ALLOWLIST)), [])

    def test_overlay_build_file_is_scanned(self):
        files = {
            "third_party/BUILD.wgpu_native_platform": (
                '# overlay for the wgpu_native_macos_aarch64 archive\n'
            )
        }
        findings = verifier.check(files, ALLOWLIST)
        self.assertEqual(categories(findings), ["rust-built-archive"])

    def test_label_form_reference_into_snapshot_is_flagged(self):
        files = {
            "third_party/tiny-skia-cpp/BUILD.bazel": (
                'deps = ["//third_party/tiny-skia-cpp/third_party/tiny-skia:src"]\n'
            )
        }
        findings = verifier.check(files, ALLOWLIST)
        self.assertEqual(categories(findings), ["reference-into-allowlist"])

    def test_non_build_files_are_not_scanned_for_edges(self):
        files = {"docs/history.md": "The old backend used rules_rust and wgpu_native_ archives."}
        self.assertEqual(verifier.check(files, ALLOWLIST), [])


class FormatReportTest(unittest.TestCase):
    def test_empty_report(self):
        self.assertEqual(verifier.format_report([]), "No Rust dependency edges found.\n")

    def test_report_groups_by_category_with_counts(self):
        findings = [
            verifier.Finding("rust-build-edge", "MODULE.bazel", "References Rust build rules: rules_rust"),
            verifier.Finding("rust-build-edge", "a/BUILD.bazel", "References Rust build rules: rules_rust"),
        ]
        report = verifier.format_report(findings)
        self.assertIn("2 Rust dependency finding(s):", report)
        self.assertIn("[rust-build-edge] (2)", report)
        self.assertIn("MODULE.bazel: References Rust build rules: rules_rust", report)


if __name__ == "__main__":
    unittest.main()
