"""Tests for the product NOTICE coverage check."""

import unittest

from tools import notice_coverage_tests

TINY_SKIA = "@@//third_party/licenses:tiny-skia-cpp"
ZLIB = "@@zlib+//:license"


def fixture():
    coverage = {
        "target": "//tools/ci:release_svg",
        "components": ["platforms", "tiny-skia-cpp", "zlib"],
        "covered": {"tiny-skia-cpp": [TINY_SKIA], "zlib": [ZLIB]},
        "exempt": {"platforms": "Bazel platform constraints."},
        "uncovered": {},
        "unexempt_linkopts": [],
    }
    manifest = {"licenses": [
        {"label": TINY_SKIA, "package_name": "tiny-skia-cpp"},
        {"label": ZLIB, "package_name": "zlib"},
    ]}
    notice = "header\n" + "".join(
        f"{'-' * 72}\nPackage: {name}\n\nlicense text\n" for name in ("tiny-skia-cpp", "zlib"))
    return coverage, manifest, notice


class NoticeCoverageTest(unittest.TestCase):
    def test_complete_notice_passes(self):
        self.assertEqual(notice_coverage_tests.check(*fixture(), ["tiny-skia-cpp"]), [])

    def test_component_without_catalog_entry_fails_and_names_it(self):
        coverage, manifest, notice = fixture()
        coverage["components"].append("newlib")
        coverage["uncovered"] = {"newlib": ["@@newlib+//:newlib"]}
        problems = notice_coverage_tests.check(coverage, manifest, notice, [])
        self.assertEqual(len(problems), 1)
        self.assertIn("newlib is linked into //tools/ci:release_svg", problems[0])
        self.assertIn("@@newlib+//:newlib", problems[0])

    def test_unexempt_library_linker_option_fails_and_names_it(self):
        coverage, manifest, notice = fixture()
        coverage["unexempt_linkopts"] = ["@@//donner/foo:bar -lz"]
        problems = notice_coverage_tests.check(coverage, manifest, notice, [])
        self.assertEqual(len(problems), 1)
        self.assertIn("linker option '-lz' from @@//donner/foo:bar", problems[0])
        self.assertIn("exempt_linkopts", problems[0])

    def test_remediation_names_the_exempt_labels_attribute(self):
        coverage, manifest, notice = fixture()
        coverage["uncovered"] = {"newlib": ["@@newlib+//:newlib"]}
        problem = notice_coverage_tests.check(coverage, manifest, notice, [])[0]
        self.assertIn("exempt_labels entry", problem)
        self.assertNotIn("exempt_components", problem)

    def test_expected_failure_fixture_passes_only_on_the_expected_problem(self):
        problems = ["linker option '-lz' from @@//tools:x links a library"]
        self.assertEqual(notice_coverage_tests._expected_failures(problems, ["'-lz'"]), 0)
        self.assertEqual(notice_coverage_tests._expected_failures(problems, ["'-lm'"]), 1)
        self.assertEqual(notice_coverage_tests._expected_failures([], ["'-lz'"]), 1)

    def test_notice_missing_or_adding_an_entry_fails(self):
        coverage, manifest, notice = fixture()
        manifest["licenses"].pop()
        self.assertIn(f"the NOTICE manifest is missing {ZLIB}",
                      notice_coverage_tests.check(coverage, manifest, notice, []))
        coverage, manifest, notice = fixture()
        manifest["licenses"].append({"label": "@@libpng+//:license", "package_name": "libpng"})
        problems = notice_coverage_tests.check(coverage, manifest, notice, [])
        self.assertIn("the NOTICE manifest lists @@libpng+//:license, which the closure does not "
                      "reach", problems)
        self.assertIn("the NOTICE text has no section for libpng", problems)

    def test_required_component_and_empty_notice_fail(self):
        coverage, manifest, notice = fixture()
        self.assertIn("the closure of //tools/ci:release_svg no longer reaches public-sans",
                      notice_coverage_tests.check(coverage, manifest, notice, ["public-sans"]))
        self.assertIn("required component platforms is not covered by a catalog entry",
                      notice_coverage_tests.check(coverage, manifest, notice, ["platforms"]))
        coverage["covered"] = {}
        manifest["licenses"] = []
        self.assertIn("the NOTICE covers no third-party component",
                      notice_coverage_tests.check(coverage, manifest, notice, []))

    def test_font_alias_is_limited_to_the_exact_three_reviewed_files(self):
        files = {"fonts/NotoSans-Bold.ttf", "fonts/NotoSans-Italic.ttf", "fonts/NotoSans-Regular.ttf"}
        requirements = ["resvg-test-suite=" + path for path in sorted(files)]
        for prefix in ("@@//third_party/resvg-test-suite:", "//third_party/resvg-test-suite:",
                       "@@resvg-test-suite+//:", "@@+deps+resvg-test-suite//:"):
            coverage = {"component_labels": {"resvg-test-suite": [prefix + path for path in files]}}
            self.assertEqual(notice_coverage_tests._exact_component_files(coverage, requirements), [])
            coverage["component_labels"]["resvg-test-suite"].append(prefix + "tests/non-font.svg")
            self.assertTrue(notice_coverage_tests._exact_component_files(coverage, requirements))

    def test_missing_file_receipt_and_foreign_repository_fail_closed(self):
        requirements = ["resvg-test-suite=fonts/NotoSans-Regular.ttf"]
        for labels in ([], ["@@another-repo+//:fonts/NotoSans-Regular.ttf"],
                       ["@@//third_party/other:fonts/NotoSans-Regular.ttf"]):
            with self.subTest(labels=labels):
                coverage = {"component_labels": {"resvg-test-suite": labels}}
                self.assertTrue(notice_coverage_tests._exact_component_files(coverage, requirements))



if __name__ == "__main__":
    unittest.main()
