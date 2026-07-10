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
            "_wasm_cc_binary rule //donner/svg:wasm_thing",
        ]
        result = mod.classify(lines)
        self.assertTrue(result.instrumentable_present)
        self.assertEqual(len(result.instrumentable), 3)
        self.assertEqual(result.non_instrumentable, [])

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
