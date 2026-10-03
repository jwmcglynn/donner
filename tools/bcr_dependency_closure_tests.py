"""Contracts for the configured closure of the four Donner libraries a BCR consumer builds."""

from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import tempfile
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

# A passing `cquery --output=label_kind` result: the four libraries, the default renderer and
# text backend, vendored repositories, and the C++ toolchain resolved in the libraries'
# configuration and in the tool configuration.
BASE_ROWS = (
    "cc_library rule @donner//donner/base:base (5a7f3c1)",
    "cc_library rule @donner//donner/css:css (5a7f3c1)",
    "cc_library rule @donner//donner/svg:svg (5a7f3c1)",
    "cc_library rule @donner//donner/svg/renderer:renderer (5a7f3c1)",
    "cc_library rule @donner//donner/svg/renderer:renderer_tiny_skia (5a7f3c1)",
    "source file @donner//donner/svg/renderer:RendererTinySkiaBackend.cc (null)",
    "cc_library rule @donner//donner/svg/text:text_backend_simple (5a7f3c1)",
    "source file @donner//donner/svg/text:TextBackendSimple.cc (null)",
    "cc_library rule @@donner++_repo_rules+tiny-skia-cpp//src:tiny_skia_lib (5a7f3c1)",
    "cc_library rule @@donner++_repo_rules+entt//src:entt (5a7f3c1)",
    "cc_library rule @@donner++_repo_rules2+stb//:stb (5a7f3c1)",
    "toolchain_type rule @bazel_tools//tools/cpp:toolchain_type (5a7f3c1)",
    "cc_toolchain rule @@rules_cc++cc_configure_extension+local_config_cc//:cc-compiler (5a7f3c1)",
    "cc_toolchain rule @@rules_cc++cc_configure_extension+local_config_cc//:cc-compiler (e062f01)",
)
BASE = "\n".join(BASE_ROWS) + "\n"


def parse(text: str) -> tuple[closure.ConfiguredTarget, ...]:
    return closure.configured_targets(text)


def with_rows(*rows: str) -> tuple[closure.ConfiguredTarget, ...]:
    """The passing closure with each query row appended."""
    return parse(BASE + "".join(row + "\n" for row in rows))


def without(*fragments: str) -> tuple[closure.ConfiguredTarget, ...]:
    """The passing closure without the rows containing any fragment."""
    return parse("".join(row + "\n" for row in BASE_ROWS
                         if not any(fragment in row for fragment in fragments)))


class DependencyClosureTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dev = closure.dev_repository_names(MODULE)

    def test_dev_repo_discovery_keeps_production_vendored_repos(self) -> None:
        self.assertTrue({"googletest", "com_google_gtest", "rules_rust",
                         "wgpu_native", "harfbuzz", "browser", "playwright"} <= self.dev)
        self.assertFalse({"entt", "stb", "rules_cc"} & self.dev)

    def test_accepts_tiny_skia_with_base_text_and_production_vendor_repos(self) -> None:
        closure.check_closure(parse(BASE), self.dev)

    def test_accepts_bazel_7_and_8_canonical_repository_names(self) -> None:
        for separator in ("+", "~"):
            with self.subTest(separator=separator):
                text = BASE.replace("@donner//", f"@@donner{separator}//")
                text += f"toolchain_type rule @@rules_cc{separator}//cc:toolchain_type (5a7f3c1)\n"
                text += f"cc_library rule @@trustfall{separator}//:trust (5a7f3c1)\n"
                closure.check_closure(parse(text), self.dev)

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
                closure.check_closure(with_rows(f"alias rule {label} (5a7f3c1)"), self.dev)

    def test_rejects_geode_webgpu_and_full_text(self) -> None:
        for label in (
            "@donner//donner/svg/renderer:renderer_geode",
            "@donner//donner/svg/renderer:RendererGeodeBackend.cc",
            "@donner//donner/svg/renderer/geode:geode_device",
            "@donner//donner/gpu:gpu",
            "@donner//third_party/webgpu-cpp:webgpu_cpp",
            "@donner//donner/svg/text:text_backend_full",
        ):
            with self.subTest(label=label), \
                    self.assertRaisesRegex(ValueError, "Geode|WebGPU|full-text"):
                closure.check_closure(with_rows(f"cc_library rule {label} (5a7f3c1)"), self.dev)

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
                closure.check_closure(with_rows(f"filegroup rule {label} (5a7f3c1)"), self.dev)

    def test_rejects_rust_rules_by_kind_whatever_their_label(self) -> None:
        for kind in ("rust_library rule", "rust_static_library rule", "rust_proc_macro rule",
                     "rust_toolchain rule", "cargo_build_script rule", "crate_universe rule"):
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "Rust rule"):
                closure.check_closure(with_rows(f"{kind} @donner//donner/svg:plain (5a7f3c1)"),
                                      self.dev)

    def test_rejects_rust_sources_and_cargo_manifests(self) -> None:
        for name in ("lib.rs", "build.RS", "Cargo.toml", "Cargo.lock", "CARGO.LOCK"):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Rust source"):
                closure.check_closure(with_rows(f"source file @donner//donner/svg:{name} (null)"),
                                      self.dev)

    def test_accepts_names_that_only_contain_the_letters(self) -> None:
        closure.check_closure(
            with_rows("trusted_cc_library rule @donner//donner/svg:plain (5a7f3c1)",
                      "untrusted_filegroup rule @donner//donner/svg:other (5a7f3c1)",
                      "source file @donner//donner/svg:trust.rst (null)",
                      "source file @donner//donner/svg:MyCargo.toml.in (null)"),
            self.dev,
        )

    def test_rejects_a_closure_queried_without_implicit_or_tool_dependencies(self) -> None:
        with self.assertRaisesRegex(ValueError, "no cc_toolchain rule"):
            closure.check_closure(without("cc_toolchain rule"), self.dev)
        with self.assertRaisesRegex(ValueError, "only one configuration"):
            closure.check_closure(without("cc-compiler (e062f01)"), self.dev)
        same_configuration = parse(BASE.replace("(e062f01)", "(5a7f3c1)"))
        with self.assertRaisesRegex(ValueError, "only one configuration"):
            closure.check_closure(same_configuration, self.dev)

    def test_injected_rust_edges_are_all_rejected_from_a_passing_closure(self) -> None:
        self.assertEqual(closure.check_injected_rust_edges(BASE, self.dev),
                         len(closure.INJECTED_RUST_EDGES))

    def test_injected_edge_check_fails_when_the_check_cannot_detect_rust(self) -> None:
        original = closure.check_external_repositories
        try:
            closure.check_external_repositories = lambda labels, dev: None
            with self.assertRaisesRegex(ValueError, "accepted an injected Rust edge"):
                closure.check_injected_rust_edges(BASE, self.dev)
        finally:
            closure.check_external_repositories = original

    def test_injected_edge_check_requires_the_rust_rejection(self) -> None:
        text = "".join(row + "\n" for row in BASE_ROWS if "donner/base:base" not in row)
        with self.assertRaisesRegex(ValueError, "failed for another reason"):
            closure.check_injected_rust_edges(text, self.dev)

    def test_rejects_missing_backend_or_base_text(self) -> None:
        for fragment in (
            "donner/base:base",
            "donner/css:css",
            "donner/svg:svg",
            "renderer:renderer_tiny_skia",
            "text:text_backend_simple",
            "src:tiny_skia_lib",
        ):
            with self.subTest(fragment=fragment), self.assertRaises(ValueError):
                closure.check_closure(without(fragment), self.dev)

    def test_reads_label_kind_query_output_with_configurations(self) -> None:
        self.assertEqual(
            closure.configured_targets(
                "cc_library rule @@donner+//donner/svg:svg (5a7f3c1)\n"
                "source file @@donner~//donner/svg:SVG.cc (null)\n"
                "toolchain_type rule @bazel_tools//tools/cpp:toolchain_type\n"
                "cc_library rule @@donner+//donner/svg:svg (5a7f3c1)\n"
            ),
            (
                closure.ConfiguredTarget("cc_library rule", "@@donner+//donner/svg:svg",
                                         "5a7f3c1"),
                closure.ConfiguredTarget("source file", "@@donner~//donner/svg:SVG.cc", "null"),
                closure.ConfiguredTarget("toolchain_type rule",
                                         "@bazel_tools//tools/cpp:toolchain_type", ""),
            ),
        )

    def test_rejects_query_output_without_kinds_or_labels(self) -> None:
        for contents in (
            "@donner//donner/svg/renderer:renderer (abc)\n",
            "cc_library rule @donner//donner/svg/renderer:renderer (abc)\nINFO: incomplete\n",
            "",
        ):
            with self.subTest(contents=contents), self.assertRaises(ValueError):
                closure.configured_targets(contents)


class CommandLineTest(unittest.TestCase):
    """Runs the checker as the consumer job does, on files."""

    def _run(self, labels_text: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory(prefix="bcr closure ") as temporary:
            root = Path(temporary)
            (root / "MODULE.bazel").write_text(MODULE, encoding="utf-8")
            (root / "closure.labels").write_text(labels_text, encoding="utf-8")
            return subprocess.run(
                [sys.executable, closure.__file__, "--module", str(root / "MODULE.bazel"),
                 "--labels", str(root / "closure.labels")],
                capture_output=True, text=True, check=False,
            )

    def test_passing_closure_reports_every_injected_edge_rejected(self) -> None:
        result = self._run(BASE)

        self.assertEqual(result.returncode, 0, result.stderr)
        count = len(closure.INJECTED_RUST_EDGES)
        self.assertIn(f"{count} of {count} injected Rust edges rejected", result.stdout)
        self.assertIn(f"{len(BASE_ROWS)} configured targets in 2 configurations", result.stdout)

    def test_closure_without_the_tool_configuration_toolchain_fails(self) -> None:
        result = self._run("".join(row + "\n" for row in BASE_ROWS if "(e062f01)" not in row))

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("only one configuration", result.stderr)

    def test_closure_with_an_appended_rust_rule_fails(self) -> None:
        result = self._run(BASE + "rust_library rule @@donner+//donner/svg:appended (5a7f3c1)\n")

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("closure includes a Rust rule", result.stderr)
        self.assertNotIn("injected Rust edges rejected", result.stdout)

    def test_closure_without_tool_dependencies_fails(self) -> None:
        result = self._run("".join(row + "\n" for row in BASE_ROWS if "cc_toolchain" not in row))

        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("no cc_toolchain rule", result.stderr)


if __name__ == "__main__":
    unittest.main()
