#!/usr/bin/env python3
"""Tests for coverage_variant_trim."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from coverage_variant_trim import WRAPPER_KIND, parse_label_kinds, trim_variants


def kinds_for(wrappers=(), others=()):
    """Build a label->kind map: wrappers get WRAPPER_KIND, others cc_library."""
    result = {label: WRAPPER_KIND for label in wrappers}
    result.update({label: "cc_library" for label in others})
    return result


class TrimVariantsTest(unittest.TestCase):
    def test_wrapper_with_base_sibling_is_dropped(self):
        labels = [
            "//donner/svg/compositor:compositor_tests",
            "//donner/svg/compositor:compositor_tests_geode",
            "//donner/svg/compositor:compositor_tests_text_full",
            "//donner/svg/compositor:compositor_tests_tiny",
        ]
        kinds = kinds_for(wrappers=labels[1:], others=labels[:1])
        kept, dropped = trim_variants(labels, kinds)
        self.assertEqual(kept, ["//donner/svg/compositor:compositor_tests"])
        self.assertEqual(len(dropped), 3)

    def test_handwritten_geode_library_is_kept(self):
        # The review scenario: renderer_geode is a real cc_library beside
        # renderer. The kind gate must keep it despite the suffix+sibling.
        labels = [
            "//donner/svg/renderer:renderer",
            "//donner/svg/renderer:renderer_geode",
        ]
        kinds = kinds_for(others=labels)
        kept, dropped = trim_variants(labels, kinds)
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_unknown_kind_is_never_trimmed(self):
        # Fail-safe: a label absent from the kind map keeps its target.
        labels = [
            "//donner/a:foo_tests",
            "//donner/a:foo_tests_geode",
        ]
        kept, dropped = trim_variants(labels, kinds_for())  # empty map
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_no_kind_map_trims_nothing(self):
        labels = [
            "//donner/a:foo_tests",
            "//donner/a:foo_tests_geode",
        ]
        kept, dropped = trim_variants(labels, None)
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_wrapper_without_base_sibling_is_kept(self):
        labels = ["//donner/svg/renderer/tests:resvg_test_suite_geode"]
        kept, dropped = trim_variants(labels, kinds_for(wrappers=labels))
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_non_variant_labels_untouched(self):
        labels = [
            "//donner/editor:editor_shell",
            "//donner/base:base_tests",
            "//donner/base:base_tests_lint",
        ]
        kept, dropped = trim_variants(labels, kinds_for(others=labels))
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_text_full_recognized_as_one_suffix(self):
        labels = [
            "//donner/svg/components/paint:paint_component_tests",
            "//donner/svg/components/paint:paint_component_tests_text_full",
        ]
        kinds = kinds_for(wrappers=labels[1:], others=labels[:1])
        kept, dropped = trim_variants(labels, kinds)
        self.assertEqual(
            kept, ["//donner/svg/components/paint:paint_component_tests"]
        )
        self.assertEqual(
            dropped,
            ["//donner/svg/components/paint:paint_component_tests_text_full"],
        )

    def test_suffix_only_name_is_kept(self):
        labels = ["//pkg:_geode", "//pkg:_tiny"]
        kept, dropped = trim_variants(labels, kinds_for(wrappers=labels))
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_base_in_other_package_does_not_count(self):
        labels = [
            "//donner/a:foo_tests",
            "//donner/b:foo_tests_geode",
        ]
        kinds = kinds_for(wrappers=["//donner/b:foo_tests_geode"],
                          others=["//donner/a:foo_tests"])
        kept, dropped = trim_variants(labels, kinds)
        self.assertEqual(kept, labels)
        self.assertEqual(dropped, [])

    def test_order_preserved(self):
        labels = ["//a:t2", "//a:t1", "//a:t3"]
        kept, _ = trim_variants(labels, kinds_for(others=labels))
        self.assertEqual(kept, labels)


class ParseLabelKindsTest(unittest.TestCase):
    def test_parses_label_kind_lines(self):
        text = (
            "cc_test rule //donner/svg/compositor:compositor_tests\n"
            "donner_multi_transitioned_test rule //donner/svg/compositor:compositor_tests_geode\n"
            "cc_library rule //donner/svg/renderer:renderer_geode\n"
        )
        kinds = parse_label_kinds(text)
        self.assertEqual(kinds["//donner/svg/compositor:compositor_tests"], "cc_test")
        self.assertEqual(
            kinds["//donner/svg/compositor:compositor_tests_geode"], WRAPPER_KIND
        )
        self.assertEqual(kinds["//donner/svg/renderer:renderer_geode"], "cc_library")

    def test_malformed_lines_skipped(self):
        kinds = parse_label_kinds("garbage\n\nrule\n//lonely:label\n")
        self.assertEqual(kinds, {})


if __name__ == "__main__":
    unittest.main()
