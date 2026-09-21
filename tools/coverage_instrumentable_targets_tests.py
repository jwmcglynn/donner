#!/usr/bin/env python3
"""Tests for coverage_instrumentable_targets.py."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import coverage_instrumentable_targets as mod

# The build-configuration guard tests emitted by
# donner/editor/editor_product_dependency_tests.bzl, as
# `bazel query --output label_kind` reports them.
_GUARD_TEST_LINES = [
    "_guard_accepts_full_text_test rule "
    "//donner/editor/tests:editor_guard_accepts_full_text_test",
    "_guard_rejects_basic_test rule "
    "//donner/editor/tests:editor_guard_rejects_basic_test",
    "_guard_rejects_no_text_test rule "
    "//donner/editor/tests:editor_guard_rejects_no_text_test",
    "_guard_rejects_text_full_without_text_test rule "
    "//donner/editor/tests:editor_guard_rejects_text_full_without_text_test",
]


class ClassifyTest(unittest.TestCase):
    def test_docs_and_tools_only_is_not_instrumentable(self):
        # A docs + python/shell tooling change (the #808 shape): filegroup for
        # docs plus tool py_tests. No C/C++ compilation unit is affected.
        lines = [
            "filegroup rule //:docs_files",
            "py_test rule //tools:filter_coverage_tests",
            "py_test rule //tools:check_lcov_report_tests",
            "sh_test rule //tools:some_shell_test",
            "genrule rule //tools:generate_embedded_test_resources",
        ]
        result = mod.classify(lines)
        self.assertFalse(result.instrumentable_present)
        self.assertEqual(result.instrumentable, [])
        self.assertEqual(len(result.non_instrumentable), 5)

    def test_host_compatible_javascript_test_is_not_cpp_instrumentable(self):
        label = "//donner/editor/wasm/tests:playwright_bazel_config_tests"
        result = mod.classify([f"js_test rule {label}"])
        self.assertEqual([], result.instrumentable)
        self.assertEqual([label], result.non_instrumentable)
        self.assertFalse(result.instrumentable_present)

    def test_javascript_test_preserves_native_and_unknown_coverage(self):
        for kind in ("cc_test", "_donner_multi_transitioned_test", "unknown_test"):
            with self.subTest(kind=kind):
                result = mod.classify(
                    [
                        "js_test rule //donner/editor/wasm/tests:playwright_bazel_config_tests",
                        f"{kind} rule //donner/base:native_tests",
                    ]
                )
                self.assertEqual(["//donner/base:native_tests"], result.instrumentable)
                self.assertTrue(result.instrumentable_present)

    def test_js_test_is_not_host_cpp_instrumentable(self):
        result = mod.classify([
            "js_test rule //donner/editor/wasm/tests:playwright_bazel_config_tests",
        ])
        self.assertFalse(result.instrumentable_present)
        self.assertEqual(result.instrumentable, [])

    def test_host_cpp_still_runs_alongside_js_tests(self):
        result = mod.classify([
            "js_test rule //donner/editor/wasm/tests:playwright_bazel_config_tests",
            "cc_test rule //donner/base:string_utils_tests",
        ])
        self.assertTrue(result.instrumentable_present)
        self.assertEqual(result.instrumentable, ["//donner/base:string_utils_tests"])

    def test_unknown_javascript_wrapper_still_fails_closed(self):
        result = mod.classify([
            "custom_js_native_test rule //donner/base:unknown_wrapper",
        ])
        self.assertTrue(result.instrumentable_present)
        self.assertEqual(result.instrumentable, ["//donner/base:unknown_wrapper"])

    def test_cc_target_forces_coverage_run(self):
        # A change touching a native C++ target must run coverage even though a
        # lint py_test is also affected.
        lines = [
            "cc_library rule //donner/base:foo",
            "py_test rule //donner/base:foo_banned_patterns_lint_test",
            "filegroup rule //:docs_files",
        ]
        result = mod.classify(lines)
        self.assertTrue(result.instrumentable_present)
        self.assertEqual(result.instrumentable, ["//donner/base:foo"])

    def test_donner_wrapper_rule_is_instrumentable(self):
        # The custom donner C++ wrapper rules forward InstrumentedFilesInfo and
        # must be treated as instrumentable, not as unknown-and-skipped.
        lines = [
            "donner_multi_transitioned_test rule //donner/svg:bar_skia",
            "_donner_perf_sensitive_cc_library rule //donner/svg:hot",
        ]
        result = mod.classify(lines)
        self.assertTrue(result.instrumentable_present)
        self.assertEqual(len(result.instrumentable), 2)
        self.assertEqual(result.non_instrumentable, [])

    def test_wasm_wrapper_rule_is_not_host_instrumentable(self):
        # Regression (#810 shape): the emsdk wasm_cc_binary wrapper transitions
        # to the wasm platform and emits .js/.wasm only. It can never produce
        # host profile data, so a wasm-packaging-only affected set must SKIP
        # coverage instead of building a guaranteed-empty report that trips the
        # empty-coverage guard (a deterministic false RUN).
        lines = [
            "_wasm_cc_binary rule //donner/svg/renderer/wasm:donner_wasm",
            "web_package rule //donner/editor/wasm:wasm_web_package",
            "serve_http rule //donner/svg/renderer/wasm:serve_test",
            "filegroup rule //donner/svg/renderer/wasm:test_page",
        ]
        result = mod.classify(lines)
        self.assertFalse(result.instrumentable_present)
        self.assertEqual(result.instrumentable, [])

    def test_host_incompatible_cc_binary_is_not_instrumentable(self):
        # The wasm bridge shim is a plain cc_binary by kind but is marked
        # target_compatible_with @platforms//:incompatible on the host; bazel's
        # --skip_incompatible_explicit_targets silently drops it from the
        # coverage build. With the cquery result supplied, PR #810's real
        # affected set classifies as skip.
        lines = [
            "filegroup rule //:docs_files",
            "_wasm_cc_binary rule //donner/svg/renderer/wasm:donner_wasm",
            "cc_binary rule //donner/svg/renderer/wasm:donner_wasm_bin",
            "serve_http rule //donner/svg/renderer/wasm:serve_test",
            "filegroup rule //donner/svg/renderer/wasm:test_page",
            "py_test rule //tools:binary_size_tests",
            "py_test rule //tools:coverage_instrumentable_targets_tests",
        ]
        incompatible = frozenset(["//donner/svg/renderer/wasm:donner_wasm_bin"])
        result = mod.classify(lines, incompatible)
        self.assertFalse(result.instrumentable_present)
        self.assertEqual(result.instrumentable, [])

    def test_host_incompatible_labels_normalize_canonical_form(self):
        # cquery prints canonical labels (@@//pkg:name); query prints
        # apparent ones (//pkg:name). They must match after normalization.
        self.assertEqual(
            mod.normalize_label("@@//donner/svg/renderer/wasm:donner_wasm_bin"),
            "//donner/svg/renderer/wasm:donner_wasm_bin",
        )
        lines = ["cc_binary rule //donner/svg/renderer/wasm:donner_wasm_bin"]
        incompatible = frozenset(
            [mod.normalize_label("@@//donner/svg/renderer/wasm:donner_wasm_bin")]
        )
        result = mod.classify(lines, incompatible)
        self.assertFalse(result.instrumentable_present)

    def test_cc_binary_not_listed_incompatible_stays_instrumentable(self):
        # Fail closed: a cc_binary absent from the host-incompatible list keeps
        # its kind-based classification and forces a coverage run.
        lines = [
            "cc_binary rule //donner/svg/tool:donner-svg",
            "cc_binary rule //donner/svg/renderer/wasm:donner_wasm_bin",
        ]
        incompatible = frozenset(["//donner/svg/renderer/wasm:donner_wasm_bin"])
        result = mod.classify(lines, incompatible)
        self.assertTrue(result.instrumentable_present)
        self.assertEqual(result.instrumentable, ["//donner/svg/tool:donner-svg"])

    def test_unknown_rule_kind_fails_closed(self):
        # An unrecognized rule kind must run coverage (uncertainty never skips).
        lines = [
            "brand_new_custom_cc_rule rule //donner/svg:mystery",
            "filegroup rule //:docs_files",
        ]
        result = mod.classify(lines)
        self.assertTrue(result.instrumentable_present)
        self.assertEqual(result.instrumentable, ["//donner/svg:mystery"])

    def test_configuration_guard_tests_alone_do_not_force_a_coverage_run(self):
        # Observed coverage-lane failure: on a change confined to the editor
        # test package the classifier reported `total=49 instrumentable=4`, the
        # four being the text-configuration guards below. `bazel query --output
        # label_kind` reports them under their Starlark rule names, which the
        # classifier did not recognize, so `bazel coverage` ran over a set that
        # compiles nothing and the report check failed with "Coverage report
        # has no executable line data (records=896, source_files=896, DA=0,
        # LF=0, LH=0)" - a red no rerun can clear. The sh_test labels here are
        # synthetic stand-ins for the other 45 targets in that set, whose kinds
        # the classifier already recognized.
        audits = [
            f"sh_test rule //donner/editor/tests:editor_audit_{index}_dependency_test"
            for index in range(45)
        ]
        result = mod.classify(audits + _GUARD_TEST_LINES)
        self.assertEqual(result.total, 49)
        self.assertEqual(result.instrumentable, [])
        self.assertFalse(result.instrumentable_present)

    def test_configuration_guard_tests_never_mask_a_real_cpp_test(self):
        # The guard-rule exemption must not weaken the empty-report guard for
        # an affected set that also compiles C++.
        result = mod.classify(
            _GUARD_TEST_LINES + ["cc_test rule //donner/editor/tests:editor_shell_tests"]
        )
        self.assertEqual(
            result.instrumentable, ["//donner/editor/tests:editor_shell_tests"]
        )
        self.assertTrue(result.instrumentable_present)

    def test_any_kind_following_the_guard_convention_is_exempt(self):
        # The exemption applies to the naming convention, not to the four rules
        # that carry it today, so a guard added later needs no edit here. The
        # cost is stated where the convention is defined: a rule that compiles
        # or wraps C/C++ must not take the name.
        result = mod.classify(
            ["_guard_future_backend_test rule //donner/editor/tests:future_guard"]
        )
        self.assertEqual(result.instrumentable, [])
        self.assertFalse(result.instrumentable_present)

    def test_guard_kind_lookalikes_still_fail_closed(self):
        # The exemption is anchored at both ends of the rule kind, so a kind
        # outside the guard naming convention keeps forcing a coverage run.
        for kind in (
            "guard_rejects_basic_test",
            "_guardian_cc_test",
            "_guard_rejects_basic",
            "_donner_guard_cc_test",
        ):
            with self.subTest(kind=kind):
                result = mod.classify([f"{kind} rule //donner/editor/tests:lookalike"])
                self.assertEqual(
                    result.instrumentable, ["//donner/editor/tests:lookalike"]
                )
                self.assertTrue(result.instrumentable_present)

    def test_alias_only_set_is_instrumentable(self):
        # Regression: a BUILD-only change whose affected set is a single alias
        # that resolves to a cc_test (e.g. the default alias
        # donner_variant_cc_test creates) must run coverage. The coverage lane
        # resolves the alias to its `actual` before classification, so the tool
        # sees the resolved cc_test rather than the `alias` kind.
        resolved = mod.classify(["cc_test rule //donner/svg:foo_impl"])
        self.assertTrue(resolved.instrumentable_present)
        self.assertEqual(resolved.instrumentable, ["//donner/svg:foo_impl"])

    def test_unresolved_alias_kind_fails_closed(self):
        # If an `alias` kind still reaches the classifier (resolution could not
        # expand it), it must be treated as instrumentable, never skipped.
        result = mod.classify(["alias rule //donner/svg:mystery_alias"])
        self.assertTrue(result.instrumentable_present)
        self.assertEqual(result.instrumentable, ["//donner/svg:mystery_alias"])

    def test_alias_resolving_to_docs_is_not_instrumentable(self):
        # An alias whose `actual` is a filegroup resolves to a non-instrumentable
        # kind, so an alias-to-docs-only change still skips coverage.
        result = mod.classify(
            [
                "filegroup rule //:docs_files",
                "py_test rule //tools:some_tool_test",
            ]
        )
        self.assertFalse(result.instrumentable_present)

    def test_non_rule_lines_are_not_instrumentable(self):
        lines = [
            "source file //donner/base:foo.cc",
            "generated file //donner/base:generated.h",
            "package group //donner:pkg",
        ]
        result = mod.classify(lines)
        self.assertFalse(result.instrumentable_present)
        self.assertEqual(result.total, 3)

    def test_empty_input_fails_closed(self):
        # Every target dropped by a keep-going query => run coverage.
        result = mod.classify([])
        self.assertTrue(result.instrumentable_present)
        self.assertEqual(result.total, 0)

    def test_blank_lines_ignored(self):
        lines = ["", "  ", "filegroup rule //:docs_files", ""]
        result = mod.classify(lines)
        self.assertEqual(result.total, 1)
        self.assertFalse(result.instrumentable_present)

    def test_malformed_single_token_fails_closed(self):
        result = mod.classify(["//weird:label_with_no_kind"])
        self.assertTrue(result.instrumentable_present)


class RestrictLinesTest(unittest.TestCase):
    """The coverage lane decides on its FINAL list, not on the affected set.

    Narrowing (tests only, tag-filtered, variant-trimmed) happens before the
    decision, and judging a wider set is how three production failures got
    past the gate: a tag-filtered cc_test and a non-test helper each voted in
    a decision about targets they were not part of.
    """

    def test_selects_only_the_final_labels(self):
        lines = [
            "js_test rule //donner/editor/wasm/tests:browser_presentation_regression_test",
            "directory_path rule //donner/editor/wasm/tests:"
            "browser_presentation_regression_test__entry_point",
            "js_test rule //donner/editor/wasm/tests:chromium_remote_smoke",
        ]
        selected, missing = mod.restrict_lines(
            lines,
            [
                "//donner/editor/wasm/tests:browser_presentation_regression_test",
                "//donner/editor/wasm/tests:chromium_remote_smoke",
            ],
        )
        self.assertEqual([], missing)
        self.assertEqual(
            [
                "js_test rule //donner/editor/wasm/tests:browser_presentation_regression_test",
                "js_test rule //donner/editor/wasm/tests:chromium_remote_smoke",
            ],
            selected,
        )

    def test_the_narrowed_set_no_longer_inherits_a_helpers_verdict(self):
        # The incident: the host-compatible `directory_path` __entry_point is
        # not a test, so it is not in the final list. Restricted to the two
        # host-incompatible js_tests, the set classifies as a skip; unrestricted
        # (with the helper still voting) it does not.
        lines = [
            "js_test rule //donner/editor/wasm/tests:browser_presentation_regression_test",
            "directory_path rule //donner/editor/wasm/tests:"
            "browser_presentation_regression_test__entry_point",
            "js_test rule //donner/editor/wasm/tests:chromium_remote_smoke",
        ]
        incompatible = frozenset(
            [
                "//donner/editor/wasm/tests:browser_presentation_regression_test",
                "//donner/editor/wasm/tests:chromium_remote_smoke",
            ]
        )
        self.assertTrue(mod.classify(lines, incompatible).instrumentable_present)

        selected, _ = mod.restrict_lines(lines, sorted(incompatible))
        self.assertFalse(mod.classify(selected, incompatible).instrumentable_present)

    def test_missing_label_is_reported_not_dropped(self):
        # `tests()` expansion can name a test_suite member that was never in
        # the queried set. An unclassified target must keep the coverage run,
        # so the caller has to hear about it rather than silently see a
        # smaller, skippable set.
        selected, missing = mod.restrict_lines(
            ["py_test rule //tools:filter_coverage_tests"],
            ["//tools:filter_coverage_tests", "//donner/svg:suite_member_tests"],
        )
        self.assertEqual(["py_test rule //tools:filter_coverage_tests"], selected)
        self.assertEqual(["//donner/svg:suite_member_tests"], missing)

    def test_canonical_and_apparent_labels_match(self):
        selected, missing = mod.restrict_lines(
            ["cc_test rule @@//donner/base:string_utils_tests"],
            ["//donner/base:string_utils_tests"],
        )
        self.assertEqual([], missing)
        self.assertEqual(1, len(selected))


if __name__ == "__main__":
    unittest.main()
