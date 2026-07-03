"""Unit tests for gen_cmakelists.py.

These tests cover the offline helpers that don't require running bazel
query. That includes the parser/validator helpers plus the opt-in CMake
build-validation helper exercised against synthetic source trees.
"""

import os
import sys
import tempfile
import textwrap
import unittest
from unittest import mock
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


class ExternalDepManifestTest(unittest.TestCase):
    def test_maps_cmake_target_dependency(self):
        dep = g._resolve_external_dep("@zlib//:z")
        self.assertEqual(dep, g.ExternalDepResolution("target", "zlib"))

    def test_maps_system_dependency(self):
        dep = g._resolve_external_dep("@harfbuzz//:harfbuzz")
        self.assertEqual(
            dep,
            g.ExternalDepResolution(
                "system",
                "${HARFBUZZ_LIBRARIES}",
                "${HARFBUZZ_INCLUDE_DIRS}",
            ),
        )

    def test_ignores_toolchain_and_absl_prefix_deps(self):
        self.assertEqual(
            g._resolve_external_dep("@rules_cc//:link_extra_lib"),
            g.ExternalDepResolution("ignore"),
        )
        self.assertEqual(
            g._resolve_external_dep("@@abseil-cpp+//absl/flags:flag"),
            g.ExternalDepResolution("ignore"),
        )

    def test_explicit_absl_mapping_wins_over_prefix_ignore(self):
        dep = g._resolve_external_dep("@com_google_absl//absl/debugging:symbolize")
        self.assertEqual(dep, g.ExternalDepResolution("target", "absl::symbolize"))


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


class CqueryBuildOutputTest(unittest.TestCase):
    def test_parse_configured_cc_library(self):
        with tempfile.TemporaryDirectory() as td:
            workspace = Path(td)
            output = textwrap.dedent(
                f"""\
                # {workspace}/donner/svg/renderer/BUILD.bazel:423:18
                cc_library(
                  name = "renderer",
                  srcs = ["//donner/svg/renderer:Renderer.cc", "//donner/svg/renderer:RendererTinySkiaBackend.cc"],
                  copts = ["-I."],
                  hdrs = ["//donner/svg/renderer:Renderer.h"],
                  deps = ["//donner/svg/renderer:renderer_tiny_skia"],
                  include_prefix = "donner/svg/renderer",
                )
                # Rule renderer instantiated at (most recent call last):
                """
            )

            targets = g._parse_cquery_build_output(output, workspace)

        self.assertEqual(len(targets), 1)
        self.assertEqual(targets[0].label, "//donner/svg/renderer:renderer")
        self.assertEqual(targets[0].kind, "cc_library")
        self.assertTrue(targets[0].compatible)
        self.assertEqual(
            targets[0].attrs["srcs"],
            [
                "//donner/svg/renderer:Renderer.cc",
                "//donner/svg/renderer:RendererTinySkiaBackend.cc",
            ],
        )

    def test_parse_incompatible_and_embed_resources_targets(self):
        with tempfile.TemporaryDirectory() as td:
            workspace = Path(td)
            output = textwrap.dedent(
                f"""\
                # {workspace}/donner/svg/text/BUILD.bazel:88:18
                cc_library(
                  name = "text_backend_full",
                  target_compatible_with = ["@platforms//:incompatible"],
                  srcs = ["//donner/svg/text:TextBackendFull.cc"],
                )

                # {workspace}/third_party/roboto/BUILD.bazel:3:16
                embed_resources_generate_header(
                  name = "roboto_header_gen",
                  generator_name = "roboto",
                  variable_names = ["kRobotoRegularTtf"],
                  out = "//third_party/roboto:RobotoFont.h",
                )
                """
            )

            targets = g._parse_cquery_build_output(output, workspace)

        self.assertEqual(targets[0].label, "//donner/svg/text:text_backend_full")
        self.assertFalse(targets[0].compatible)
        self.assertEqual(targets[1].label, "//third_party/roboto:roboto")
        self.assertEqual(targets[1].kind, "embed_resources")


class CqueryExpressionTest(unittest.TestCase):
    def test_query_uses_positive_leaf_dependency_closure(self):
        self.assertEqual(g._CMAKE_LEAF_TARGET_PATTERNS, ("//:donner",))
        self.assertIn("deps((", g._CQUERY_TARGET_EXPRESSION)
        self.assertNotIn(" except ", g._CQUERY_TARGET_EXPRESSION)
        self.assertIn("//:donner", g._CQUERY_TARGET_EXPRESSION)
        self.assertNotIn("/...", g._CQUERY_TARGET_EXPRESSION)
        self.assertNotIn("//donner/svg/renderer/tests:all", g._CQUERY_TARGET_EXPRESSION)
        self.assertNotIn("//examples:render_test", g._CQUERY_TARGET_EXPRESSION)
        self.assertNotIn("//examples:svg_viewer", g._CQUERY_TARGET_EXPRESSION)
        self.assertNotIn("//donner/editor", g._CQUERY_TARGET_EXPRESSION)

    def test_skipped_packages_are_only_dedicated_cmake_packages(self):
        self.assertTrue(g._is_skipped_package("third_party/stb"))
        self.assertTrue(g._is_skipped_package("third_party/tiny-skia-cpp"))
        self.assertFalse(g._is_skipped_package("donner/editor"))
        self.assertFalse(g._is_skipped_package("donner/svg/renderer/geode"))


class GeneratedRootCmakeTest(unittest.TestCase):
    def test_cmake_consumer_example_is_not_part_of_generated_root(self):
        root_target = g.CMakeTarget(
            label="//:donner",
            package="",
            name="donner",
            kind="cc_library",
            configs=set(g._ALL_CONFIG_NAMES),
            values=g._target_value_map(),
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            previous_cwd = Path.cwd()
            try:
                os.chdir(temp_dir)
                with mock.patch.object(g, "get_fetchcontent_externals", return_value=[]):
                    with mock.patch.object(
                        g,
                        "extract_versions_from_module_bazel",
                        return_value={},
                    ):
                        with mock.patch.object(
                            g, "get_cmake_targets", return_value={"//:donner": root_target}
                        ):
                            g.generate_root()
                            g.generate_all_packages()
            finally:
                os.chdir(previous_cwd)

            contents = (Path(temp_dir) / "CMakeLists.txt").read_text()

        self.assertNotIn("DONNER_BUILD_EXAMPLES", contents)
        self.assertIn("if(DONNER_BUILD_TESTS)", contents)
        self.assertIn("add_library(donner INTERFACE)", contents)
        self.assertNotIn("examples/cmake_consumer", contents)


class TinySkiaCmakeGenerationTest(unittest.TestCase):
    def test_generates_tiny_skia_cmake_from_bazel_sources(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir) / "third_party/tiny-skia-cpp"
            (root / "src/tiny_skia/filter").mkdir(parents=True)
            (root / "src/tiny_skia").mkdir(parents=True, exist_ok=True)
            (root / "src").mkdir(exist_ok=True)

            (root / "src/BUILD.bazel").write_text(
                textwrap.dedent(
                    """
                    cc_library(
                        name = "tiny_skia_lib",
                        deps = ["//src/tiny_skia:tiny_skia_core"],
                    )
                    """
                )
            )
            (root / "src/tiny_skia/BUILD.bazel").write_text(
                textwrap.dedent(
                    """
                    tiny_skia_cc_library(
                        name = "tiny_skia_core",
                        srcs = [
                            "Canvas.cpp",
                            "//src/tiny_skia/filter:srcs",
                        ],
                        deps = [],
                    )
                    """
                )
            )
            (root / "src/tiny_skia/filter/BUILD.bazel").write_text(
                textwrap.dedent(
                    """
                    filegroup(
                        name = "srcs",
                        srcs = ["FilterGraph.cpp"],
                    )

                    tiny_skia_cc_library(
                        name = "filter",
                        srcs = ["GaussianBlur.cpp"],
                        deps = ["//src/tiny_skia:tiny_skia_core"],
                    )
                    """
                )
            )

            with mock.patch.object(g, "_TINY_SKIA_ROOT", root):
                sources = g._collect_tiny_skia_sources()
                g.generate_tiny_skia_cmake()

            cmake = (root / "CMakeLists.txt").read_text()

        self.assertEqual(
            sources,
            [
                "src/tiny_skia/Canvas.cpp",
                "src/tiny_skia/filter/FilterGraph.cpp",
                "src/tiny_skia/filter/GaussianBlur.cpp",
            ],
        )
        self.assertIn("Generated by tools/cmake/gen_cmakelists.py - DO NOT EDIT", cmake)
        self.assertIn("add_tiny_skia_target(tiny_skia ", cmake)
        self.assertIn("  src/tiny_skia/filter/GaussianBlur.cpp\n", cmake)


class ConditionDerivationTest(unittest.TestCase):
    def _configs_where(self, predicate):
        return {config.name for config in g.CMAKE_CONFIGS if predicate(config)}

    def test_common_feature_conditions_are_simplified(self):
        self.assertIsNone(g._condition_for_config_names(g._ALL_CONFIG_NAMES))
        self.assertEqual(
            g._condition_for_config_names(self._configs_where(lambda c: c.text)),
            "DONNER_TEXT",
        )
        self.assertEqual(
            g._condition_for_config_names(self._configs_where(lambda c: c.text_full)),
            "DONNER_TEXT_FULL",
        )
        self.assertEqual(
            g._condition_for_config_names(self._configs_where(lambda c: c.filters)),
            "DONNER_FILTERS",
        )

    def test_values_for_target_uses_feature_conditions(self):
        target = g.CMakeTarget(
            label="//donner/svg/renderer:renderer_tiny_skia",
            package="donner/svg/renderer",
            name="renderer_tiny_skia",
            kind="cc_library",
            configs=set(g._ALL_CONFIG_NAMES),
            values={
                "defines": {
                    "DONNER_TEXT_ENABLED": self._configs_where(lambda c: c.text),
                    "DONNER_FILTERS_ENABLED": self._configs_where(lambda c: c.filters),
                },
            },
        )

        fixed, conditional = g._values_for_target(target, "defines")

        self.assertEqual(fixed, [])
        self.assertEqual(
            dict(conditional),
            {"DONNER_TEXT_ENABLED": "DONNER_TEXT", "DONNER_FILTERS_ENABLED": "DONNER_FILTERS"},
        )

    def test_woff2_condition_uses_cmake_specific_option(self):
        target = g.CMakeTarget(
            label="//donner/svg/resources:font_manager",
            package="donner/svg/resources",
            name="font_manager",
            kind="cc_library",
            configs=set(g._ALL_CONFIG_NAMES),
            values={"defines": {}},
        )

        condition = g._condition_for_item(
            target,
            "DONNER_TEXT_WOFF2_ENABLED",
            self._configs_where(lambda c: c.text_full),
        )

        self.assertEqual(condition, "DONNER_TEXT_WOFF2")

    def test_dependency_condition_respects_referenced_target_guard(self):
        all_configs_target = g.CMakeTarget(
            label="//donner/svg/renderer/tests:text_backend_tests",
            package="donner/svg/renderer/tests",
            name="text_backend_tests",
            kind="cc_test",
            configs=set(g._ALL_CONFIG_NAMES),
            values={"deps": {}},
        )
        text_full_dep = g.CMakeTarget(
            label="//donner/svg/text:text_backend_full",
            package="donner/svg/text",
            name="text_backend_full",
            kind="cc_library",
            configs=self._configs_where(lambda c: c.text_full),
            values={"deps": {}},
        )

        condition = g._condition_for_dep(
            all_configs_target,
            "donner_svg_text_text_backend_full",
            set(g._ALL_CONFIG_NAMES),
            {"donner_svg_text_text_backend_full": text_full_dep},
        )

        self.assertEqual(condition, "DONNER_TEXT_FULL")

    def test_target_configs_constrained_by_unavailable_direct_dep(self):
        text_full_configs = self._configs_where(lambda c: c.text_full)
        all_config_values = g._target_value_map()
        all_config_values["srcs"]["TextBackend_tests.cc"] = set(g._ALL_CONFIG_NAMES)
        all_config_values["deps"]["//donner/svg/text:text_backend_full"] = set(g._ALL_CONFIG_NAMES)
        text_full_values = g._target_value_map()
        text_full_values["srcs"]["TextBackendFull.cc"] = set(text_full_configs)
        targets = {
            "//donner/svg/renderer/tests:text_backend_tests": g.CMakeTarget(
                label="//donner/svg/renderer/tests:text_backend_tests",
                package="donner/svg/renderer/tests",
                name="text_backend_tests",
                kind="cc_test",
                configs=set(g._ALL_CONFIG_NAMES),
                values=all_config_values,
            ),
            "//donner/svg/text:text_backend_full": g.CMakeTarget(
                label="//donner/svg/text:text_backend_full",
                package="donner/svg/text",
                name="text_backend_full",
                kind="cc_library",
                configs=set(text_full_configs),
                values=text_full_values,
            ),
        }

        g._constrain_target_configs_by_dependency_compatibility(targets)

        constrained = targets["//donner/svg/renderer/tests:text_backend_tests"]
        self.assertEqual(constrained.configs, text_full_configs)
        self.assertEqual(constrained.values["srcs"]["TextBackend_tests.cc"], text_full_configs)
        self.assertEqual(
            constrained.values["deps"]["//donner/svg/text:text_backend_full"],
            text_full_configs,
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


class CmakeBuildValidationTest(unittest.TestCase):
    def _completed_cmake_process(self, returncode=0, stdout=""):
        return g.subprocess.CompletedProcess(
            args=[],
            returncode=returncode,
            stdout=stdout,
        )

    def test_reports_missing_cmake_command(self):
        with tempfile.TemporaryDirectory() as source_dir_str:
            source_dir = Path(source_dir_str)
            build_dir = source_dir / "build"

            with mock.patch.object(g.subprocess, "run", side_effect=FileNotFoundError):
                error = g._run_cmake_build_validation(source_dir, build_dir, jobs=1)

            self.assertIsNotNone(error)
            self.assertIn("cmake command not found", error)

    def test_build_succeeds_for_valid_tree(self):
        with tempfile.TemporaryDirectory() as source_dir_str:
            source_dir = Path(source_dir_str)
            build_dir = source_dir / "build"
            (source_dir / "CMakeLists.txt").write_text(
                textwrap.dedent(
                    """\
                    cmake_minimum_required(VERSION 3.20)
                    project(cmake_build_validation_ok LANGUAGES C)
                    add_library(smoke STATIC smoke.c)
                    """
                )
            )
            (source_dir / "smoke.c").write_text("int Smoke(void) { return 42; }\n")

            with mock.patch.object(
                g.subprocess,
                "run",
                side_effect=[
                    self._completed_cmake_process(),
                    self._completed_cmake_process(),
                ],
            ) as run:
                error = g._run_cmake_build_validation(source_dir, build_dir, jobs=1)

            self.assertIsNone(error)
            self.assertEqual(
                run.call_args_list,
                [
                    mock.call(
                        ["cmake", "-S", ".", "-B", str(build_dir)],
                        cwd=source_dir,
                        text=True,
                        stdout=g.subprocess.PIPE,
                        stderr=g.subprocess.STDOUT,
                        check=False,
                    ),
                    mock.call(
                        ["cmake", "--build", str(build_dir), "--parallel", "1"],
                        cwd=source_dir,
                        text=True,
                        stdout=g.subprocess.PIPE,
                        stderr=g.subprocess.STDOUT,
                        check=False,
                    ),
                ],
            )

    def test_build_reports_compile_failure(self):
        with tempfile.TemporaryDirectory() as source_dir_str:
            source_dir = Path(source_dir_str)
            build_dir = source_dir / "build"
            (source_dir / "CMakeLists.txt").write_text(
                textwrap.dedent(
                    """\
                    cmake_minimum_required(VERSION 3.20)
                    project(cmake_build_validation_fail LANGUAGES C)
                    add_library(smoke STATIC smoke.c)
                    """
                )
            )
            (source_dir / "smoke.c").write_text(
                '#include "missing_header_for_build_validation.h"\n'
                "int Smoke(void) { return 42; }\n"
            )

            with mock.patch.object(
                g.subprocess,
                "run",
                side_effect=[
                    self._completed_cmake_process(),
                    self._completed_cmake_process(
                        returncode=2,
                        stdout=(
                            "smoke.c:1:10: fatal error: "
                            "'missing_header_for_build_validation.h' file not found\n"
                        ),
                    ),
                ],
            ):
                error = g._run_cmake_build_validation(source_dir, build_dir, jobs=1)

            self.assertIsNotNone(error)
            self.assertIn("CMake build failed:", error)
            self.assertIn("missing_header_for_build_validation.h", error)


if __name__ == "__main__":
    unittest.main()
