"""Unit tests for gen_cmakelists.py.

These tests cover the offline/pure-Python helpers that don't require
running bazel query. The full generation + validation flow is exercised
by the --check mode (run from CI and presubmit.sh), not from here.
"""

import os
import sys
import unittest
from pathlib import Path

# Allow running as either a standalone script or a bazel py_test
_SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_SCRIPT_DIR))

import gen_cmakelists as g


class NormalizeVersionTest(unittest.TestCase):
    def test_strip_bcr_suffix(self):
        self.assertEqual(g._normalize_version("1.17.0.bcr.2", use_v_prefix=True), "v1.17.0")
        self.assertEqual(g._normalize_version("3.12.0.bcr.1", use_v_prefix=True), "v3.12.0")

    def test_v_prefix_added(self):
        self.assertEqual(g._normalize_version("1.3.2", use_v_prefix=True), "v1.3.2")

    def test_v_prefix_not_added(self):
        self.assertEqual(g._normalize_version("0.2.17", use_v_prefix=False), "0.2.17")

    def test_existing_v_prefix_preserved(self):
        self.assertEqual(g._normalize_version("v3.16.0", use_v_prefix=True), "v3.16.0")

    def test_commit_hash_passthrough(self):
        sha = "1f184d05566b3e25827a1f8e68eb82b9ccf54f3b"
        self.assertEqual(g._normalize_version(sha, use_v_prefix=True), sha)
        self.assertEqual(g._normalize_version(sha, use_v_prefix=False), sha)


class ModuleBazelExtractionTest(unittest.TestCase):
    def test_extracts_known_deps(self):
        versions = g.extract_versions_from_module_bazel()
        # These dependencies must exist in MODULE.bazel with a version.
        # entt is intentionally absent: it is vendored as a git subtree under
        # third_party/entt, so there's no bazel_dep/git_repository block to
        # read. The CMake FetchContent entry comes from _HARDCODED_FETCHCONTENT.
        required = ["googletest", "nlohmann_json", "zlib",
                    "rules_cc", "pixelmatch-cpp17", "woff2"]
        for dep in required:
            self.assertIn(dep, versions, f"Missing dep: {dep}")

    def test_entt_is_hardcoded(self):
        # entt is vendored as a git subtree, so it's not in MODULE.bazel.
        # CMake users get it via _HARDCODED_FETCHCONTENT; Bazel users get the
        # vendored tree via local_repository(path = "third_party/entt").
        versions = g.extract_versions_from_module_bazel()
        self.assertNotIn("entt", versions,
                         "entt should be vendored, not declared as a bazel_dep/git_repository")
        self.assertIn("entt", g._HARDCODED_FETCHCONTENT,
                      "entt must have a hardcoded FetchContent entry")

    def test_woff2_is_commit(self):
        versions = g.extract_versions_from_module_bazel()
        # woff2 uses commit = "1f184d05..." (new_git_repository)
        import re
        self.assertTrue(re.fullmatch(r"[0-9a-f]{40}", versions["woff2"]),
                        f"Expected commit hash, got: {versions['woff2']}")


class FetchContentExternalsTest(unittest.TestCase):
    def test_returns_all_known_deps(self):
        externals = g.get_fetchcontent_externals()
        names = {name for name, _, _ in externals}
        self.assertIn("entt", names)
        self.assertIn("googletest", names)
        self.assertIn("nlohmann_json", names)
        self.assertIn("zlib", names)
        self.assertIn("rules_cc", names)
        self.assertIn("absl", names)  # hardcoded

    def test_no_bcr_suffix_in_tags(self):
        externals = g.get_fetchcontent_externals()
        for name, url, tag in externals:
            self.assertNotIn(".bcr.", tag, f"{name} has BCR suffix: {tag}")


class AbslDepHandlingTest(unittest.TestCase):
    def test_absl_dep_pattern_canonical(self):
        self.assertTrue(
            g._ABSL_DEP_RE.match("@@abseil-cpp+//absl/container:flat_hash_set")
        )

    def test_absl_dep_pattern_legacy(self):
        self.assertTrue(g._ABSL_DEP_RE.match("@com_google_absl//absl/types:any"))

    def test_non_absl_does_not_match(self):
        self.assertFalse(g._ABSL_DEP_RE.match("@foo//bar:baz"))

    def test_unmapped_absl_dep_is_internal(self):
        # Unmapped abseil deps are silently dropped (matches pre-existing
        # behavior; some absl rules don't export under absl::<rulename>).
        self.assertTrue(
            g._is_known_bazel_internal("@@abseil-cpp+//absl/flags:flag")
        )

    def test_mapped_absl_dep_is_not_internal(self):
        # Explicitly mapped absl deps must NOT be ignored as internal.
        # failure_signal_handler is in KNOWN_BAZEL_TO_CMAKE_DEPS.
        self.assertFalse(
            g._is_known_bazel_internal(
                "@com_google_absl//absl/debugging:failure_signal_handler"
            )
        )


class IsKnownBazelInternalTest(unittest.TestCase):
    def test_bazel_tools(self):
        self.assertTrue(g._is_known_bazel_internal("@bazel_tools//tools/cpp:link_extra_lib"))
        self.assertTrue(g._is_known_bazel_internal("@bazel_tools//tools/cpp:malloc"))
        self.assertTrue(g._is_known_bazel_internal("@rules_cc//:link_extra_lib"))

    def test_handled_externally(self):
        self.assertTrue(g._is_known_bazel_internal("@harfbuzz//:harfbuzz"))
        self.assertTrue(g._is_known_bazel_internal("@freetype//:freetype"))

    def test_ignored_deps(self):
        self.assertTrue(g._is_known_bazel_internal("@re2//:re2"))
        self.assertTrue(g._is_known_bazel_internal("@glfw//:glfw"))

    def test_real_dep_not_internal(self):
        self.assertFalse(g._is_known_bazel_internal("@zlib//:z"))
        self.assertFalse(g._is_known_bazel_internal("//donner/base:base"))


class SkipCmakeDepTest(unittest.TestCase):
    def test_geode_subpackage_dep_is_skipped(self):
        self.assertTrue(g._should_skip_cmake_dep("donner_svg_renderer_geode_geo_encoder"))

    def test_normal_dep_is_not_skipped(self):
        self.assertFalse(g._should_skip_cmake_dep("donner_svg_renderer_renderer_tiny_skia"))
        self.assertFalse(g._should_skip_cmake_dep(None))


class CmakeTargetNameTest(unittest.TestCase):
    def test_root_package(self):
        self.assertEqual(g.cmake_target_name("", "donner"), "donner")

    def test_donner_package(self):
        self.assertEqual(g.cmake_target_name("donner/svg", "svg_core"), "donner_svg_svg_core")

    def test_nested_package(self):
        self.assertEqual(
            g.cmake_target_name("donner/svg/renderer", "renderer"),
            "donner_svg_renderer_renderer",
        )


class ExtractTargetsAndRefsTest(unittest.TestCase):
    """Smoke test the CMake parser on synthetic input."""

    def test_parse_add_library(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "CMakeLists.txt").write_text(
                "add_library(foo foo.cc foo.h)\n"
                "target_link_libraries(foo PUBLIC bar baz)\n"
                "add_library(bar bar.cc)\n"
                "target_sources(bar PRIVATE bar_extra.cc)\n"
            )
            defined, linked, sources = g._extract_cmake_targets_and_refs(root)
            self.assertIn("foo", defined)
            self.assertIn("bar", defined)
            # foo links to bar and baz
            foo_links = {t for t, _ in linked.get("foo", [])}
            self.assertEqual(foo_links, {"bar", "baz"})
            # bar sources include bar.cc and bar_extra.cc
            bar_srcs = {s for s, _ in sources.get("bar", [])}
            self.assertEqual(bar_srcs, {"bar.cc", "bar_extra.cc"})

    def test_parse_add_executable(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "CMakeLists.txt").write_text(
                "add_executable(my_test my_test.cc)\n"
            )
            defined, _, sources = g._extract_cmake_targets_and_refs(root)
            self.assertIn("my_test", defined)
            self.assertIn("my_test.cc", {s for s, _ in sources.get("my_test", [])})

    def test_comments_stripped(self):
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "CMakeLists.txt").write_text(
                "# add_library(fake fake.cc)\n"
                "add_library(real real.cc)\n"
            )
            defined, _, _ = g._extract_cmake_targets_and_refs(root)
            self.assertIn("real", defined)
            self.assertNotIn("fake", defined)


if __name__ == "__main__":
    unittest.main()
