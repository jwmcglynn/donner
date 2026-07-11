#!/usr/bin/env python3
"""Tests for coverage_instrumentable_targets.py."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import coverage_instrumentable_targets as mod


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

    def test_instrumentable_lines_expose_kinds(self):
        # The coverage lane's recheck gate needs the KINDS of the instrumentable
        # residue (it only cqueries cc_binary-only sets).
        lines = [
            "filegroup rule //:docs_files",
            "cc_binary rule //donner/svg/renderer/wasm:donner_wasm_bin",
        ]
        result = mod.classify(lines)
        self.assertEqual(
            result.instrumentable_lines,
            ["cc_binary rule //donner/svg/renderer/wasm:donner_wasm_bin"],
        )

    def test_unknown_rule_kind_fails_closed(self):
        # An unrecognized rule kind must run coverage (uncertainty never skips).
        lines = [
            "brand_new_custom_cc_rule rule //donner/svg:mystery",
            "filegroup rule //:docs_files",
        ]
        result = mod.classify(lines)
        self.assertTrue(result.instrumentable_present)
        self.assertEqual(result.instrumentable, ["//donner/svg:mystery"])

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


if __name__ == "__main__":
    unittest.main()
