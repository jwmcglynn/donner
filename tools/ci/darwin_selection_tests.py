import unittest

from darwin_selection import darwin_variants, root_query, variant


class DarwinSelectionTests(unittest.TestCase):
    def test_wildcard_removes_manual_roots_before_expanding_suites(self):
        query = root_query(True, "")
        self.assertIn('set("//...")', query)
        self.assertIn(r"attr(tags, '(\[|, )manual(, |\])', $input)", query)
        self.assertIn('union tests($roots)', query)

    def test_affected_selection_preserves_exact_labels(self):
        query = root_query(False, "//:a //pkg:npm+target")
        self.assertIn('set("//:a" "//pkg:npm+target")', query)
        self.assertIn("except attr(tags,", query)
        self.assertIn("package_group|toolchain_type", query)
        self.assertEqual(variant("//pkg:npm+target"), "//pkg:ci_darwin_npm+target")
        self.assertEqual(variant("//:a"), "//:ci_darwin_a")

    def test_third_party_targets_are_dependencies_only_on_darwin(self):
        vendor = ["//third_party:notice", "//third_party/google_fonts:catalog"]
        self.assertEqual(list(darwin_variants(vendor)), [])
        roots = [*vendor, "//donner/svg:renderer", "//third_party_tools:check"]
        self.assertEqual(list(darwin_variants(roots)),
                         ["//donner/svg:ci_darwin_renderer", "//third_party_tools:ci_darwin_check"])

    def test_empty_or_external_selection_fails_closed(self):
        with self.assertRaises(ValueError):
            root_query(False, "")
        with self.assertRaises(ValueError):
            variant("@other//pkg:name")


if __name__ == "__main__":
    unittest.main()
