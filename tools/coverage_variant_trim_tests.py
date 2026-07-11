#!/usr/bin/env python3
"""Tests for coverage_variant_trim.trim_variants."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from coverage_variant_trim import trim_variants


class TrimVariantsTest(unittest.TestCase):
    def test_variant_with_base_sibling_is_dropped(self):
        labels = [
            "//donner/svg/compositor:compositor_tests",
            "//donner/svg/compositor:compositor_tests_geode",
            "//donner/svg/compositor:compositor_tests_text_full",
            "//donner/svg/compositor:compositor_tests_tiny",
        ]
        kept, dropped = trim_variants(labels)
        self.assertEqual(kept, ["//donner/svg/compositor:compositor_tests"])
        self.assertEqual(len(dropped), 3)

    def test_variant_without_base_sibling_is_kept(self):
        # Fail-safe: a variant-only entry keeps its coverage representative.
        labels = ["//donner/svg/renderer/tests:resvg_test_suite_geode"]
        kept, dropped = trim_variants(labels)
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_non_variant_labels_untouched(self):
        labels = [
            "//donner/editor:editor_shell",
            "//donner/base:base_tests",
            "//donner/base:base_tests_lint",
            "//donner/editor/tests:lock_state_tests",
        ]
        kept, dropped = trim_variants(labels)
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_text_full_not_misread_as_tiny_or_geode(self):
        # _text_full must be recognized as one suffix; base is the stripped name.
        labels = [
            "//donner/svg/components/paint:paint_component_tests",
            "//donner/svg/components/paint:paint_component_tests_text_full",
        ]
        kept, dropped = trim_variants(labels)
        self.assertEqual(
            kept, ["//donner/svg/components/paint:paint_component_tests"]
        )
        self.assertEqual(
            dropped, ["//donner/svg/components/paint:paint_component_tests_text_full"]
        )

    def test_suffix_only_name_is_kept(self):
        # A label whose name IS the suffix has no valid base; keep it.
        labels = ["//pkg:_geode", "//pkg:_tiny"]
        kept, dropped = trim_variants(labels)
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_base_in_other_package_does_not_count(self):
        # The sibling must be the same package-qualified label.
        labels = [
            "//donner/a:foo_tests",
            "//donner/b:foo_tests_geode",
        ]
        kept, dropped = trim_variants(labels)
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_mixed_realistic_set(self):
        labels = [
            "//donner/svg/compositor:layer_resolver_tests_text_full",
            "//donner/svg/compositor:layer_resolver_tests",
            "//donner/svg/components/shadow:shadow_tree_system_tests_text_full",
            "//donner/editor:select_tool",
            "//donner/svg/renderer/tests:renderer_driver_tests_geode",
            "//donner/svg/renderer/tests:renderer_driver_tests",
        ]
        kept, dropped = trim_variants(labels)
        self.assertIn("//donner/svg/compositor:layer_resolver_tests", kept)
        self.assertIn(
            "//donner/svg/components/shadow:shadow_tree_system_tests_text_full", kept
        )  # variant-only: kept
        self.assertIn("//donner/editor:select_tool", kept)
        self.assertIn("//donner/svg/renderer/tests:renderer_driver_tests", kept)
        self.assertEqual(
            sorted(dropped),
            [
                "//donner/svg/compositor:layer_resolver_tests_text_full",
                "//donner/svg/renderer/tests:renderer_driver_tests_geode",
            ],
        )

    def test_order_preserved(self):
        labels = ["//a:t2", "//a:t1", "//a:t3"]
        kept, _ = trim_variants(labels)
        self.assertEqual(kept, labels)


if __name__ == "__main__":
    unittest.main()
