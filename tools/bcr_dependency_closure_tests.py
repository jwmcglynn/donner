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

BASE_LABELS = {
    "@donner//donner/svg/renderer:renderer",
    "@donner//donner/svg/renderer:renderer_tiny_skia",
    "@donner//donner/svg/renderer:RendererTinySkiaBackend.cc",
    "@donner//donner/svg/text:text_backend_simple",
    "@donner//donner/svg/text:TextBackendSimple.cc",
    "@@donner++_repo_rules+tiny-skia-cpp//src:tiny_skia_lib",
    "@@donner++_repo_rules+entt//src:entt",
    "@@donner++_repo_rules2+stb//:stb",
    "@bazel_tools//tools/cpp:toolchain_type",
}


class DependencyClosureTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dev = closure.dev_repository_names(MODULE)

    def test_dev_repo_discovery_keeps_production_vendored_repos(self) -> None:
        self.assertTrue({"googletest", "com_google_gtest", "rules_rust",
                         "wgpu_native", "harfbuzz", "browser", "playwright"} <= self.dev)
        self.assertFalse({"entt", "stb", "rules_cc"} & self.dev)

    def test_accepts_tiny_skia_with_base_text_and_production_vendor_repos(self) -> None:
        closure.check_closure(BASE_LABELS, self.dev)

    def test_rejects_dev_repos_even_when_root_consumer_has_them(self) -> None:
        for label in (
            "@com_google_gtest//:gtest",
            "@@rules_rust+//:toolchain",
            "@@donner++private+wgpu_native//:library",
        ):
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, "development-only"):
                closure.check_closure(BASE_LABELS | {label}, self.dev)

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
                closure.check_closure(BASE_LABELS | {label}, self.dev)

    def test_rejects_rust_namespaces_outside_declared_dev_repos(self) -> None:
        for label in (
            "@@crates_io//:crate",
            "@@rustls//:rustls",
            "@donner//third_party/rustls:rustls",
        ):
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, "Rust"):
                closure.check_closure(BASE_LABELS | {label}, self.dev)

    def test_rejects_in_tree_test_packages_and_targets(self) -> None:
        for label in (
            "@donner//donner/svg/renderer/tests:pilot_corpus_manifest",
            "@donner//third_party/stb/tests/pngsuite:sample",
            "@donner//donner/svg/renderer:renderer_tests",
        ):
            with self.subTest(label=label), self.assertRaisesRegex(ValueError, "test targets"):
                closure.check_closure(BASE_LABELS | {label}, self.dev)

    def test_rejects_missing_backend_or_base_text(self) -> None:
        for label in (
            "@donner//donner/svg/renderer:renderer_tiny_skia",
            "@donner//donner/svg/text:text_backend_simple",
            "@@donner++_repo_rules+tiny-skia-cpp//src:tiny_skia_lib",
        ):
            with self.subTest(label=label), self.assertRaises(ValueError):
                closure.check_closure(BASE_LABELS - {label}, self.dev)

    def test_rejects_non_label_query_output(self) -> None:
        with self.assertRaisesRegex(ValueError, "non-label"):
            closure.configured_labels("@donner//donner/svg/renderer:renderer (abc)\nINFO: incomplete\n")


if __name__ == "__main__":
    unittest.main()
