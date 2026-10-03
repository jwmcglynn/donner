"""Contracts for the downstream renderer closure admitted by BCR Preflight."""

from __future__ import annotations

import unittest

from tools import bcr_dependency_closure as closure


MODULE = '''
bazel_dep(name = "rules_cc", version = "0.2.25")
bazel_dep(name = "googletest", dev_dependency = True, repo_name = "com_google_gtest")
bazel_dep(name = "rules_rust", dev_dependency = True)
local_repository(name = "entt", path = "third_party/entt")
new_local_repository(name = "stb", path = "third_party/stb", build_file = "//:BUILD.stb")
private = use_extension("//:dev.bzl", "private", dev_dependency = True)
private.repo(name = "wgpu_native")
use_repo(private, "harfbuzz", browser = "playwright")
'''

BASE_TARGETS = {
    "@donner//donner/base:base": "cc_library rule",
    "@donner//donner/css:css": "cc_library rule",
    "@donner//donner/svg:svg": "cc_library rule",
    "@donner//donner/svg/renderer:renderer": "cc_library rule",
    "@donner//donner/svg/renderer:renderer_tiny_skia": "cc_library rule",
    "@donner//donner/svg/renderer:RendererTinySkiaBackend.cc": "source file",
    "@donner//donner/svg/text:text_backend_simple": "cc_library rule",
    "@donner//donner/svg/text:TextBackendSimple.cc": "source file",
    "@@donner++_repo_rules+tiny-skia-cpp//src:tiny_skia_lib": "cc_library rule",
    "@@donner++_repo_rules+entt//src:entt": "cc_library rule",
    "@@donner++_repo_rules2+stb//:stb": "cc_library rule",
    "@bazel_tools//tools/cpp:toolchain_type": "toolchain_type rule",
}


def with_targets(*extra: tuple[str, str]) -> dict[str, str]:
    """The passing closure plus each (kind, label) pair."""
    return {**BASE_TARGETS, **{label: kind for kind, label in extra}}


def without(*labels: str) -> dict[str, str]:
    return {label: kind for label, kind in BASE_TARGETS.items() if label not in labels}


class DependencyClosureTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dev = closure.dev_repository_names(MODULE)

    def test_dev_repo_discovery_keeps_production_vendored_repos(self) -> None:
        self.assertTrue({"googletest", "com_google_gtest", "rules_rust",
                         "wgpu_native", "harfbuzz", "browser", "playwright"} <= self.dev)
        self.assertFalse({"entt", "stb", "rules_cc"} & self.dev)

    def test_accepts_tiny_skia_with_base_text_and_production_vendor_repos(self) -> None:
        closure.check_closure(BASE_TARGETS, self.dev)

    def test_accepts_bazel_7_and_8_canonical_repository_names(self) -> None:
        for separator in ("+", "~"):
            with self.subTest(separator=separator):
                targets = {
                    label.replace("@donner//", f"@@donner{separator}//"): kind
                    for label, kind in BASE_TARGETS.items()
                }
                targets[f"@@rules_cc{separator}//cc:toolchain_type"] = "toolchain_type rule"
                targets[f"@@trustfall{separator}//:trust"] = "cc_library rule"
                closure.check_closure(targets, self.dev)

    def test_rejects_dev_repos_even_when_root_consumer_has_them(self) -> None:
        for label in (
            "@com_google_gtest//:gtest",
            "@@rules_rust+//:toolchain",
            "@@donner++private+wgpu_native//:library",
        ):
            # Rust and WebGPU repositories are named as such before development-only membership.
            named = "rust" in label or "wgpu" in label
            expected = "Rust or WebGPU" if named else "development-only"
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, expected):
                closure.check_closure(with_targets(("alias rule", label)), self.dev)

    def test_rejects_geode_webgpu_and_full_text(self) -> None:
        for label in (
            "@donner//donner/svg/renderer:renderer_geode",
            "@donner//donner/svg/renderer:RendererGeodeBackend.cc",
            "@donner//donner/svg/renderer/geode:geode_device",
            "@donner//donner/gpu:gpu",
            "@donner//third_party/webgpu-cpp:webgpu_cpp",
            "@donner//donner/svg/text:text_backend_full",
        ):
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, "Geode|WebGPU|full-text"):
                closure.check_closure(with_targets(("cc_library rule", label)), self.dev)

    def test_rejects_rust_namespaces_outside_declared_dev_repos(self) -> None:
        for label in (
            "@@crates_io//:crate",
            "@@rustls//:rustls",
            "@donner//third_party/rustls:rustls",
            "@@rules_rust~//rust:toolchain_type",
            "@@rules_rust~~rust~rust_toolchains//:toolchain",
            "@@rules_rust~~crate~crates__serde-1.0.0//:serde",
            "@@rules_rust++rust+rust_linux_x86_64__x86_64-unknown-linux-gnu__stable_tools//:rustc",
            "@@rules_rust++crate+crate_index//:serde",
        ):
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, "Rust"):
                closure.check_closure(with_targets(("filegroup rule", label)), self.dev)

    def test_rejects_rust_rules_by_kind_whatever_their_label(self) -> None:
        for kind in ("rust_library rule", "rust_static_library rule", "rust_proc_macro rule",
                     "rust_toolchain rule", "cargo_build_script rule", "crate_universe rule"):
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "Rust rule"):
                closure.check_closure(with_targets((kind, "@donner//donner/svg:plain")), self.dev)

    def test_accepts_kinds_that_only_contain_the_letters(self) -> None:
        closure.check_closure(
            with_targets(("trusted_cc_library rule", "@donner//donner/svg:plain"),
                         ("untrusted_filegroup rule", "@donner//donner/svg:other")),
            self.dev,
        )

    def test_injected_rust_edges_are_rejected_by_a_passing_closure(self) -> None:
        closure.check_injected_rust_edges(BASE_TARGETS, self.dev)

    def test_injected_edge_check_fails_when_the_check_cannot_detect_rust(self) -> None:
        original = closure.check_external_repositories
        try:
            closure.check_external_repositories = lambda labels, dev: None
            with self.assertRaisesRegex(ValueError, "accepted an injected Rust edge"):
                closure.check_injected_rust_edges(BASE_TARGETS, self.dev)
        finally:
            closure.check_external_repositories = original

    def test_injected_edge_check_requires_the_rust_rejection(self) -> None:
        with self.assertRaisesRegex(ValueError, "failed for another reason"):
            closure.check_injected_rust_edges(without("@donner//donner/base:base"), self.dev)

    def test_rejects_in_tree_test_packages_and_targets(self) -> None:
        for label in (
            "@donner//donner/svg/renderer/tests:pilot_corpus_manifest",
            "@donner//third_party/stb/tests/pngsuite:sample",
            "@donner//donner/svg/renderer:renderer_tests",
        ):
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, "test targets"):
                closure.check_closure(with_targets(("cc_library rule", label)), self.dev)

    def test_rejects_missing_backend_or_base_text(self) -> None:
        for label in (
            "@donner//donner/base:base",
            "@donner//donner/css:css",
            "@donner//donner/svg:svg",
            "@donner//donner/svg/renderer:renderer_tiny_skia",
            "@donner//donner/svg/text:text_backend_simple",
            "@@donner++_repo_rules+tiny-skia-cpp//src:tiny_skia_lib",
        ):
            with self.subTest(label=label), self.assertRaises(ValueError):
                closure.check_closure(without(label), self.dev)

    def test_reads_label_kind_query_output(self) -> None:
        self.assertEqual(
            closure.configured_targets(
                "cc_library rule @@donner+//donner/svg:svg (5a7f3)\n"
                "source file @@donner~//donner/svg:SVG.cc (null)\n"
                "toolchain_type rule @bazel_tools//tools/cpp:toolchain_type\n"
            ),
            {
                "@@donner+//donner/svg:svg": "cc_library rule",
                "@@donner~//donner/svg:SVG.cc": "source file",
                "@bazel_tools//tools/cpp:toolchain_type": "toolchain_type rule",
            },
        )

    def test_rejects_query_output_without_kinds_or_labels(self) -> None:
        for contents in (
            "@donner//donner/svg/renderer:renderer (abc)\n",
            "cc_library rule @donner//donner/svg/renderer:renderer (abc)\nINFO: incomplete\n",
            "",
        ):
            with self.subTest(contents=contents), self.assertRaises(ValueError):
                closure.configured_targets(contents)


if __name__ == "__main__":
    unittest.main()
