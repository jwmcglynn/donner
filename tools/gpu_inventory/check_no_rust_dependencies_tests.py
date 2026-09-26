"""Unit tests for check_no_rust_dependencies.py verifier logic."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_no_rust_dependencies as verifier
from rust_scopes import RustScopes

SCOPES = RustScopes(
    inert_reference_prefixes=(
        "third_party/resvg-test-suite/",
        "third_party/tiny-skia-cpp/third_party/tiny-skia/",
    ),
    test_only_rust_prefixes=(
        "third_party/tiny-skia-cpp/MODULE.bazel",
        "third_party/tiny-skia-cpp/tests/rust_ffi/",
    ),
    test_only_consumer_prefixes=("third_party/tiny-skia-cpp/tests/",),
    test_only_gpu_oracle_archives=(
        ("wgpu_native_linux_aarch64", "wgpu-linux-aarch64-release.zip",
         "97786f622d6d4f9aaa87c27d165de8db65daf1d391e0bcc32a2dd9bb45fcd299"),
        ("wgpu_native_linux_x86_64", "wgpu-linux-x86_64-release.zip",
         "86f3eb9f74d7f1ac82ee52d9b2ab15e366ef86a932759c750b7472652836ee59"),
    ),
)

FIXTURE_BUILD = "third_party/tiny-skia-cpp/tests/rust_ffi/BUILD.bazel"


def categories(findings):
    return sorted({f.category for f in findings})


class CheckTest(unittest.TestCase):
    def test_clean_tree_has_no_findings(self):
        files = {
            "donner/base/BUILD.bazel": 'donner_cc_library(name = "base")\n',
        }
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_allowlisted_rust_source_is_not_flagged(self):
        files = {
            "third_party/tiny-skia-cpp/third_party/tiny-skia/src/lib.rs": "fn f() {}",
            "third_party/tiny-skia-cpp/third_party/tiny-skia/Cargo.toml": "[package]",
        }
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_rust_source_outside_allowlist_is_flagged(self):
        files = {"donner/experiment/helper.rs": "fn f() {}"}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-source-outside-allowlist"])
        self.assertEqual(findings[0].path, "donner/experiment/helper.rs")

    def test_rust_build_edge_in_module_bazel(self):
        files = {"third_party/example/MODULE.bazel": 'bazel_dep(name = "rules_rust", version = "0.71.3")\n'}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])
        self.assertIn("rules_rust", findings[0].detail)

    def test_rust_build_edge_in_a_tracked_lockfile(self):
        """A checked-in lockfile is a deliberate act and is scanned.

        The untracked one a local build leaves behind is not: it records the
        whole transitive Bzlmod graph, including the rules_rust that protobuf
        declares and nothing fetches, which is the graph rather than the
        closure. `collect_scannable_files` reads tracked files only.
        """
        files = {"MODULE.bazel.lock": '{"key": "@@rules_rust+//crate_universe"}'}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])

    def test_rust_build_edge_in_cmake_input(self):
        """CMake is a shipped build graph too, and was outside the old scan."""
        files = {"CMakeLists.txt": "corrosion_import_crate(MANIFEST_PATH rules_rust)\n"}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])

    def test_rust_built_archive_download_is_flagged(self):
        files = {
            "third_party/bazel/non_bcr_deps.bzl": (
                'url = "https://github.com/gfx-rs/wgpu-native/releases/download/v1/x.zip"\n'
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-built-archive"])

    def test_overlay_build_file_is_scanned(self):
        files = {
            "third_party/BUILD.wgpu_native_platform": (
                'cc_library(\n    name = "wgpu_native",\n'
                '    testonly = True,\n    srcs = ["lib/libwgpu_native.dylib"],\n)\n'
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-built-archive"])

    def test_archive_name_in_comment_is_not_a_build_edge(self):
        files = {"donner/gpu/BUILD.bazel": "# old wgpu_native_macos archive\n"}
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_non_build_files_are_not_scanned_for_edges(self):
        files = {"docs/history.md": "The old backend used rules_rust and wgpu_native_ archives."}
        self.assertEqual(verifier.check(files, SCOPES), [])


class TestOnlyOracleTest(unittest.TestCase):
    """The oracle is retained, so containment is what the verifier enforces."""

    def test_contained_oracle_is_not_a_finding(self):
        files = {
            "third_party/tiny-skia-cpp/MODULE.bazel": (
                'bazel_dep(name = "rules_rust", version = "0.71.3")\n'
                'crate.from_cargo(name = "crates")\n'
            ),
            FIXTURE_BUILD: (
                'load("@rules_rust//rust:defs.bzl", "rust_static_library")\n'
                'rust_static_library(name = "tiny_skia_ffi_rust", srcs = ["src/lib.rs"])\n'
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//tests:__subpackages__"],\n'
                '    deps = [":tiny_skia_ffi_rust"],\n'
                ")\n"
            ),
            "third_party/tiny-skia-cpp/tests/rust_ffi/src/lib.rs": "fn f() {}",
            "third_party/tiny-skia-cpp/tests/rust_ffi/Cargo.toml": "[package]",
            "third_party/tiny-skia-cpp/tests/test_utils/BUILD.bazel": (
                'cc_library(name = "rust_reference", deps = ["//tests/rust_ffi:tiny_skia_ffi"])\n'
            ),
        }
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_public_fixture_visibility_is_flagged(self):
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//visibility:public"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("//visibility:public", findings[0].detail)

    def test_non_test_fixture_visibility_is_flagged(self):
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//src:__subpackages__"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("//src:__subpackages__", findings[0].detail)

    def test_private_fixture_visibility_is_allowed(self):
        files = {FIXTURE_BUILD: 'cc_library(\n    visibility = ["//visibility:private"],\n)\n'}
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_donner_target_depending_on_the_oracle_is_flagged(self):
        """The finding that matters: Rust entering a non-test closure."""
        files = {
            "donner/svg/renderer/BUILD.bazel": (
                'cc_test(name = "parity", deps = ["@tiny-skia-cpp//tests/rust_ffi:tiny_skia_ffi"])\n'
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("tests/rust_ffi", findings[0].detail)

    def test_reference_to_the_derived_oracle_libraries_is_flagged(self):
        files = {
            "donner/svg/renderer/BUILD.bazel": (
                'cc_test(name = "parity", deps = ["@tiny-skia-cpp//tests/test_utils:cross_validator"])\n'
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])

    def test_sibling_test_package_may_consume_the_oracle(self):
        files = {
            "third_party/tiny-skia-cpp/tests/benchmarks/BUILD.bazel": (
                '_COMMON_DEPS = ["//tests/rust_ffi:tiny_skia_ffi"]\n'
            )
        }
        self.assertEqual(verifier.check(files, SCOPES), [])


class ReferenceIntoAllowlistTest(unittest.TestCase):
    """Compiling the inert snapshot is a finding; staging its goldens is not."""

    def test_snapshot_label_in_deps_is_flagged(self):
        files = {
            "third_party/tiny-skia-cpp/BUILD.bazel": (
                "cc_library(\n"
                '    name = "port",\n'
                '    deps = ["//third_party/tiny-skia:src"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["reference-into-allowlist"])

    def test_snapshot_rust_source_in_srcs_is_flagged(self):
        files = {
            "third_party/tiny-skia-cpp/BUILD.bazel": (
                "filegroup(\n"
                '    name = "src",\n'
                '    srcs = ["third_party/tiny-skia/src/lib.rs"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["reference-into-allowlist"])

    def test_golden_images_as_test_data_are_not_flagged(self):
        """The real integration BUILD shape: a constant staged through `data`."""
        files = {
            "third_party/tiny-skia-cpp/tests/integration/BUILD.bazel": (
                '_GOLDEN_IMAGES = ["//third_party/tiny-skia/tests/images:golden_images"]\n'
                "\n"
                "cc_test(\n"
                '    name = "fill_test",\n'
                '    srcs = ["FillTest.cpp"],\n'
                "    data = _GOLDEN_IMAGES,\n"
                ")\n"
            )
        }
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_inline_golden_image_data_is_not_flagged(self):
        files = {
            "third_party/tiny-skia-cpp/tests/integration/BUILD.bazel": (
                "cc_test(\n"
                '    name = "fill_test",\n'
                '    data = ["//third_party/tiny-skia/tests/images:golden_images"],\n'
                ")\n"
            )
        }
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_golden_constant_also_used_in_deps_is_flagged(self):
        """A constant reaching any compile attribute fails, not just `data`."""
        files = {
            "third_party/tiny-skia-cpp/tests/integration/BUILD.bazel": (
                '_GOLDEN_IMAGES = ["//third_party/tiny-skia/tests/images:golden_images"]\n'
                "\n"
                "cc_test(\n"
                '    name = "fill_test",\n'
                "    deps = _GOLDEN_IMAGES,\n"
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["reference-into-allowlist"])

    def test_unplaceable_reference_fails_closed(self):
        files = {"third_party/tiny-skia-cpp/BUILD.bazel": "//third_party/tiny-skia:src\n"}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["reference-into-allowlist"])

    def test_build_file_inside_allowlist_is_not_a_reference_finding(self):
        files = {
            "third_party/tiny-skia-cpp/third_party/tiny-skia/BUILD.bazel": (
                'srcs = ["third_party/tiny-skia/src/lib.rs"]\n'
            )
        }
        self.assertEqual(categories(verifier.check(files, SCOPES)), [])


class ContainmentEvasionTest(unittest.TestCase):
    """Escapes an independent review drove through the first containment check.

    Every one of these exited 0 against a verifier that only looked at
    visibility inside the fixture package and at fixture tokens outside the
    vendored test tree.
    """

    def test_alias_reexporting_the_oracle_is_flagged(self):
        files = {
            "third_party/tiny-skia-cpp/tests/BUILD.bazel": (
                'package(default_visibility = ["//tests:__subpackages__"])\n'
                "\n"
                "alias(\n"
                '    name = "oracle",\n'
                '    actual = "//tests/rust_ffi:tiny_skia_ffi",\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("alias", findings[0].detail)

    def test_bzl_under_the_test_tree_handing_out_the_oracle_is_flagged(self):
        files = {
            "third_party/tiny-skia-cpp/tests/oracle.bzl": (
                'ORACLE = "//tests/rust_ffi:tiny_skia_ffi"\n'
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])

    def test_public_default_visibility_in_a_consumer_package_is_flagged(self):
        """The consumer prefix is not a licence to re-export."""
        files = {
            "third_party/tiny-skia-cpp/tests/test_utils/BUILD.bazel": (
                'package(default_visibility = ["//visibility:public"])\n'
                'cc_library(name = "rust_reference", deps = ["//tests/rust_ffi:tiny_skia_ffi"])\n'
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("//visibility:public", findings[0].detail)

    def test_cross_repository_visibility_entry_is_flagged(self):
        """`@donner//...:__pkg__` leaves the vendored workspace entirely."""
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["@donner//donner/base/tests:__pkg__"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])

    def test_a_tests_segment_deeper_in_the_path_is_not_the_test_tree(self):
        """`//src/tiny_skia/tests` is a source package, not `//tests`."""
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//src/tiny_skia/tests:__pkg__"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])

    def test_visibility_from_a_constant_is_flagged(self):
        """A non-literal visibility is unreadable here, so it fails closed."""
        files = {
            FIXTURE_BUILD: (
                '_VIS = ["//visibility:public"]\n'
                "\n"
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                "    visibility = _VIS,\n"
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("not a literal list", findings[0].detail)

    def test_visibility_concatenating_a_public_list_is_flagged(self):
        """Reading to the first `]` stopped at the safe half of the expression."""
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//tests:__subpackages__"] + ["//visibility:public"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("not a literal list", findings[0].detail)

    def test_visibility_concatenating_a_source_package_is_flagged(self):
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//tests:__subpackages__"] + ["//src:__subpackages__"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("not a literal list", findings[0].detail)

    def test_a_plain_list_followed_by_a_comment_is_still_literal(self):
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//tests:__subpackages__"],  # the vendored test tree\n'
                ")\n"
            )
        }
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_a_package_default_closing_the_call_is_still_literal(self):
        files = {
            "third_party/tiny-skia-cpp/tests/test_utils/BUILD.bazel": (
                'package(default_visibility = ["//tests:__subpackages__"])\n'
            )
        }
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_test_tree_subpackage_visibility_is_still_allowed(self):
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//tests:__subpackages__"],\n'
                ")\n"
            ),
            "third_party/tiny-skia-cpp/tests/test_utils/BUILD.bazel": (
                'package(default_visibility = ["//tests/integration:__pkg__"])\n'
                'cc_library(name = "rust_reference", deps = ["//tests/rust_ffi:tiny_skia_ffi"])\n'
            ),
        }
        self.assertEqual(verifier.check(files, SCOPES), [])


class CmakeRustTokenTest(unittest.TestCase):
    """CMake reaches Rust with its own vocabulary, not Bazel's."""

    def test_corrosion_import_crate_is_flagged(self):
        files = {"examples/CMakeLists.txt": "corrosion_import_crate(MANIFEST_PATH Cargo.toml)\n"}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])
        self.assertIn("corrosion", findings[0].detail)

    def test_cargo_build_custom_command_is_flagged(self):
        files = {"examples/CMakeLists.txt": "add_custom_command(COMMAND cargo build --release)\n"}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])

    def test_find_package_rust_is_flagged(self):
        files = {"cmake/FindRust.cmake": "find_program(RUSTC_EXECUTABLE rustc)\n"}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])

    def test_cmake_tokens_do_not_fire_on_bazel_files(self):
        """`cargo` and `rustc` are matched in CMake inputs, not everywhere."""
        files = {"donner/BUILD.bazel": '# the cargo cult of rustc corrosion\n'}
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_plain_cmake_is_not_flagged(self):
        files = {"examples/CMakeLists.txt": "add_executable(demo main.cc)\n"}
        self.assertEqual(verifier.check(files, SCOPES), [])


class AttributeScanRobustnessTest(unittest.TestCase):
    def test_an_unbalanced_paren_in_a_comment_does_not_shift_attribution(self):
        """The exact evasion: a stray `(` plus a top-level `data` variable.

        Bracket counting over raw lines let the comment's paren hold the depth
        open, so a later `srcs = glob([...])` inside a rule was attributed to
        the `data` variable and read as staged test data.
        """
        files = {
            "third_party/tiny-skia-cpp/BUILD.bazel": (
                "# a helpful note with an unclosed paren (see the docs\n"
                'data = ["nothing"]\n'
                "\n"
                "cc_library(\n"
                '    name = "port",\n'
                '    srcs = glob(["third_party/tiny-skia/src/**"]),\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["reference-into-allowlist"])

    def test_a_bracket_inside_a_string_does_not_shift_attribution(self):
        files = {
            "third_party/tiny-skia-cpp/BUILD.bazel": (
                'data = ["a ( b [ c"]\n'
                "\n"
                "cc_library(\n"
                '    name = "port",\n'
                '    deps = ["//third_party/tiny-skia:src"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["reference-into-allowlist"])


class BazelrcImportTest(unittest.TestCase):
    def test_an_imported_bazelrc_is_scanned(self):
        files = {"ci.bazelrc": "build --@rules_rust//:extra_rustc_flags=-Copt-level=3\n"}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])


class DirectToolInvocationTest(unittest.TestCase):
    """Bazel can run the toolchain without ever naming a Rust rule."""

    def test_genrule_running_cargo_is_flagged(self):
        files = {
            "donner/tools/BUILD.bazel": (
                "genrule(\n"
                '    name = "gen",\n'
                '    cmd = "cargo build --release && cp target/release/x $@",\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])
        self.assertIn("cargo", findings[0].detail)

    def test_bzl_action_running_rustc_is_flagged(self):
        files = {
            "build_defs/rust_shim.bzl": (
                "def _impl(ctx):\n"
                "    ctx.actions.run_shell(\n"
                '        command = "rustc --edition 2021 $1",\n'
                "    )\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])
        self.assertIn("rustc", findings[0].detail)

    def test_rustup_download_is_flagged(self):
        files = {"third_party/deps.bzl": 'cmd = "rustup toolchain install stable"\n'}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])

    def test_rule_set_names_are_not_split_into_bare_commands(self):
        """`cargo_bazel` is one identifier, not the `cargo` command."""
        files = {"third_party/example/MODULE.bazel": 'use_extension("@rules_rust//crate_universe:cargo_bazel.bzl")\n'}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])
        self.assertEqual(
            findings[0].detail,
            "References a Rust toolchain outside the test-only oracle: "
            "cargo_bazel, crate_universe, rules_rust",
        )

    def test_a_cargo_lockfile_label_is_not_a_command(self):
        files = {"donner/BUILD.bazel": 'exports_files(["Cargo.lock", "Cargo.toml"])\n'}
        self.assertEqual(
            [f for f in verifier.check(files, SCOPES) if f.category == "rust-build-edge"], []
        )

    def test_prose_in_a_comment_is_not_a_command(self):
        files = {"donner/BUILD.bazel": "# we do not run cargo or rustc here, ever\n"}
        self.assertEqual(verifier.check(files, SCOPES), [])


class CmakeGeneratorSourceTest(unittest.TestCase):
    """Emitted CMakeLists.txt files are git-ignored, so read what emits them."""

    def test_generator_source_is_read_with_the_cmake_vocabulary(self):
        files = {"tools/cmake/gen_cmakelists.py": 'lines.append("corrosion_import_crate(...)")\n'}
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-build-edge"])
        self.assertIn("corrosion", findings[0].detail)

    def test_generator_test_source_is_not_scanned(self):
        """Its fixtures hold these strings on purpose, and it emits nothing."""
        files = {"tools/cmake/gen_cmakelists_test.py": 'EXPECTED = "cargo build"\n'}
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_an_unrelated_tool_source_is_not_scanned(self):
        files = {"tools/other/thing.py": 'note = "corrosion and cargo"\n'}
        self.assertEqual(verifier.check(files, SCOPES), [])


class VisibilityDecodingTest(unittest.TestCase):
    def test_single_quoted_public_visibility_is_flagged(self):
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                "    visibility = ['//visibility:public'],\n"
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("//visibility:public", findings[0].detail)

    def test_single_quoted_test_tree_visibility_is_allowed(self):
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                "    visibility = ['//tests:__subpackages__'],\n"
                ")\n"
            )
        }
        self.assertEqual(verifier.check(files, SCOPES), [])

    def test_an_undecodable_element_fails_closed(self):
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//tests:__subpackages__", EXTRA_PACKAGES],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("not a literal list", findings[0].detail)

    def test_package_group_label_is_flagged(self):
        """A package_group's membership is declared somewhere else."""
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//tests/policy:everyone"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])
        self.assertIn("//tests/policy:everyone", findings[0].detail)

    def test_a_bare_package_without_a_target_is_flagged(self):
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//tests/integration"],\n'
                ")\n"
            )
        }
        findings = verifier.check(files, SCOPES)
        self.assertEqual(categories(findings), ["rust-fixture-containment"])

    def test_pkg_and_subpackages_targets_are_allowed(self):
        files = {
            FIXTURE_BUILD: (
                "cc_library(\n"
                '    name = "tiny_skia_ffi",\n'
                '    visibility = ["//tests:__pkg__", "//tests/integration:__subpackages__"],\n'
                ")\n"
            )
        }
        self.assertEqual(verifier.check(files, SCOPES), [])


class BlockingSelectionTest(unittest.TestCase):
    def test_absent_flag_blocks_nothing(self):
        self.assertEqual(verifier.parse_blocking(None), ())

    def test_bare_flag_blocks_every_category(self):
        self.assertEqual(verifier.parse_blocking("all"), verifier.CATEGORIES)

    def test_category_list_is_parsed(self):
        self.assertEqual(
            verifier.parse_blocking("rust-build-edge, rust-built-archive"),
            ("rust-build-edge", "rust-built-archive"),
        )

    def test_unknown_category_is_rejected(self):
        with self.assertRaises(SystemExit) as raised:
            verifier.parse_blocking("rust-build-edge,typo")
        self.assertIn("typo", str(raised.exception))

    def test_default_blocking_includes_unexpected_archives(self):
        self.assertEqual(verifier.DEFAULT_BLOCKING, verifier.CATEGORIES)

    def test_the_default_keyword_selects_that_set(self):
        """The CI set lives in the tool, so the workflow spells one word."""
        self.assertEqual(verifier.parse_blocking("default"), verifier.DEFAULT_BLOCKING)


class LinuxGpuOracleArchiveTest(unittest.TestCase):
    """The sole Rust-built GPU archive crosses only a checksum-pinned test boundary."""

    @staticmethod
    def allowed_files():
        return {
            "third_party/bazel/non_bcr_deps.bzl": (
                '_WGPU_NATIVE_PLATFORMS = [\n'
                '    struct(\n        name = "wgpu_native_linux_aarch64",\n'
                '        asset = "wgpu-linux-aarch64-release.zip",\n'
                '        sha256 = "97786f622d6d4f9aaa87c27d165de8db65daf1d391e0bcc32a2dd9bb45fcd299",\n'
                '    ),\n'
                '    struct(\n        name = "wgpu_native_linux_x86_64",\n'
                '        asset = "wgpu-linux-x86_64-release.zip",\n'
                '        sha256 = "86f3eb9f74d7f1ac82ee52d9b2ab15e366ef86a932759c750b7472652836ee59",\n'
                '    ),\n]\n'
                'for p in _WGPU_NATIVE_PLATFORMS:\n'
                '    http_archive(\n'
                '        url = "https://github.com/gfx-rs/wgpu-native/releases/download/{}/{}",\n'
                '        sha256 = p.sha256,\n'
                '        build_file = "//third_party:BUILD.wgpu_native_platform",\n'
                '    )\n'
            ),
            "MODULE.bazel": (
                'use_repo(\n    non_bcr_deps,\n'
                '    "wgpu_native_linux_aarch64",\n'
                '    "wgpu_native_linux_x86_64",\n)\n'
            ),
            "third_party/BUILD.wgpu_native_platform": (
                'cc_library(\n    name = "wgpu_native",\n    testonly = True,\n'
                '    srcs = ["lib/libwgpu_native.so"],\n)\n'
            ),
            "third_party/webgpu-cpp/BUILD.bazel": (
                'cc_library(\n    name = "webgpu_cpp",\n    testonly = True,\n'
                '    deps = [":wgpu_native_platform"],\n)\n'
                'cc_library(\n    name = "wgpu_native_reference_runtime",\n'
                '    testonly = True,\n'
                '    target_compatible_with = ["@platforms//os:linux"],\n'
                '    visibility = ["//donner/svg/renderer/tests:__pkg__"],\n'
                '    deps = [":webgpu_cpp"],\n)\n'
                'alias(\n    name = "wgpu_native_platform",\n    testonly = True,\n'
                '    actual = ":wgpu_native_linux",\n)\n'
                'alias(\n    name = "wgpu_native_linux",\n    testonly = True,\n'
                '    actual = select({\n'
                '        "@platforms//cpu:aarch64": "@wgpu_native_linux_aarch64//:wgpu_native",\n'
                '        "@platforms//cpu:x86_64": "@wgpu_native_linux_x86_64//:wgpu_native",\n'
                '    }),\n)\n'
            ),
            "donner/svg/renderer/tests/BUILD.bazel": (
                'donner_cc_test(\n    name = "resvg_test_suite_wgpu_reference_linux_impl",\n'
                '    target_compatible_with = ["@platforms//os:linux"],\n'
                '    deps = ["//third_party/webgpu-cpp:wgpu_native_reference_runtime"],\n)\n'
            ),
        }

    def test_exact_linux_oracle_is_allowed(self):
        self.assertEqual(categories(verifier.check(self.allowed_files(), SCOPES)), [])

    def test_each_required_oracle_edge_fails_closed_when_removed(self):
        omissions = {
            "third_party/bazel/non_bcr_deps.bzl": "def non_bcr_deps(ctx):\n    pass\n",
            "MODULE.bazel": "use_repo(non_bcr_deps)\n",
            "third_party/BUILD.wgpu_native_platform": (
                'cc_library(\n    name = "headers_only",\n)\n'
            ),
            "third_party/webgpu-cpp/BUILD.bazel": (
                'cc_library(\n    name = "webgpu_cpp_headers",\n)\n'
            ),
            "donner/svg/renderer/tests/BUILD.bazel": (
                'donner_cc_test(\n    name = "resvg_test_suite_wgpu_reference_linux_impl",\n'
                '    deps = [],\n)\n'
            ),
        }
        for path, replacement in omissions.items():
            with self.subTest(path=path):
                files = self.allowed_files()
                files[path] = replacement
                findings = verifier.check(files, SCOPES)
                self.assertIn("rust-built-archive", categories(findings))
                self.assertIn(path, [finding.path for finding in findings])

    def test_new_macos_archive_fails_default_blocking(self):
        files = self.allowed_files()
        files["third_party/bazel/non_bcr_deps.bzl"] = files["third_party/bazel/non_bcr_deps.bzl"].replace(
            "]\nfor p in _WGPU_NATIVE_PLATFORMS:",
            '    struct(\n        name = "wgpu_native_macos_aarch64",\n'
            '        asset = "wgpu-macos-aarch64-release.zip",\n'
            '        sha256 = "f140ff27234ebfa9fcca2b492d0cb499f2e197424b9edc45134bcbad0f8d3a78",\n'
            '    ),\n]\nfor p in _WGPU_NATIVE_PLATFORMS:',
        )
        findings = verifier.check(files, SCOPES)
        self.assertIn("rust-built-archive", categories(findings))
        self.assertIn("rust-built-archive", verifier.parse_blocking("default"))

    def test_changed_or_empty_sha_is_rejected(self):
        for replacement in ("0" * 64, "", "f" * 64):
            with self.subTest(sha256=replacement):
                files = self.allowed_files()
                files["third_party/bazel/non_bcr_deps.bzl"] = files["third_party/bazel/non_bcr_deps.bzl"].replace(
                    "86f3eb9f74d7f1ac82ee52d9b2ab15e366ef86a932759c750b7472652836ee59",
                    replacement,
                )
                self.assertIn("rust-built-archive", categories(verifier.check(files, SCOPES)))

    def test_production_reference_is_rejected(self):
        files = self.allowed_files()
        files["donner/editor/BUILD.bazel"] = (
            'cc_library(\n    name = "editor",\n'
            '    deps = ["//third_party/webgpu-cpp:wgpu_native_reference_runtime"],\n)\n'
        )
        self.assertIn("rust-built-archive", categories(verifier.check(files, SCOPES)))
        files["donner/editor/BUILD.bazel"] = (
            'cc_library(\n    name = "editor",\n'
            '    deps = ["@wgpu_native_freebsd_arm64//:wgpu_native"],\n)\n'
        )
        self.assertIn("rust-built-archive", categories(verifier.check(files, SCOPES)))

    def test_overlay_or_runtime_exposure_is_rejected(self):
        files = self.allowed_files()
        files["third_party/BUILD.wgpu_native_platform"] = files["third_party/BUILD.wgpu_native_platform"].replace(
            'srcs = ["lib/libwgpu_native.so"]',
            'srcs = ["lib/libwgpu_native.so", "lib/libwgpu_native.dylib"]',
        )
        self.assertIn("rust-built-archive", categories(verifier.check(files, SCOPES)))

    def test_each_wrapper_alias_link_is_required(self):
        removals = (
            (':webgpu_cpp', ':missing_wrapper'),
            (':wgpu_native_platform', ':missing_platform'),
            (':wgpu_native_linux', ':missing_linux'),
            ('@wgpu_native_linux_aarch64//:wgpu_native', '@missing_archive//:wgpu_native'),
        )
        for old, new in removals:
            with self.subTest(edge=old):
                files = self.allowed_files()
                files["third_party/webgpu-cpp/BUILD.bazel"] = files[
                    "third_party/webgpu-cpp/BUILD.bazel"
                ].replace(old, new)
                findings = verifier.check(files, SCOPES)
                self.assertIn("rust-built-archive", categories(findings))

    def test_label_in_tags_does_not_count_as_wrapper_dependency(self):
        files = self.allowed_files()
        files["third_party/webgpu-cpp/BUILD.bazel"] = files[
            "third_party/webgpu-cpp/BUILD.bazel"
        ].replace(
            'deps = [":wgpu_native_platform"],',
            'deps = [],\n    tags = [":wgpu_native_platform"],',
        )
        self.assertIn("rust-built-archive", categories(verifier.check(files, SCOPES)))
        files = self.allowed_files()
        files["third_party/webgpu-cpp/BUILD.bazel"] = files["third_party/webgpu-cpp/BUILD.bazel"].replace(
            'name = "webgpu_cpp",\n    testonly = True,',
            'name = "webgpu_cpp",',
        )
        self.assertIn("rust-built-archive", categories(verifier.check(files, SCOPES)))


class FormatReportTest(unittest.TestCase):
    def test_empty_report(self):
        self.assertEqual(verifier.format_report([], verifier.CATEGORIES), "No Rust dependency edges found.\n")

    def test_report_groups_by_category_with_counts_and_mode(self):
        findings = [
            verifier.Finding("rust-build-edge", "MODULE.bazel", "References Rust build rules"),
            verifier.Finding("rust-built-archive", "MODULE.bazel", "Declares archives"),
        ]
        report = verifier.format_report(findings, ("rust-build-edge",))
        self.assertIn("2 Rust dependency finding(s):", report)
        self.assertIn("[rust-build-edge] (1, blocking)", report)
        self.assertIn("[rust-built-archive] (1, report-only)", report)


if __name__ == "__main__":
    unittest.main()
