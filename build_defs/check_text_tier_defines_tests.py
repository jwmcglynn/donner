"""Tests for build_defs/check_text_tier_defines.py."""

import textwrap
import unittest

from build_defs import check_text_tier_defines as check

_TEXT_BUILD = "donner/svg/text/BUILD.bazel"


def _problems(text, path=_TEXT_BUILD):
    return check.file_problems(path, textwrap.dedent(text))


class TextTierDefinesTest(unittest.TestCase):
    def test_reviewed_local_defines_pass(self):
        self.assertEqual([], _problems("""
            donner_cc_library(
                name = "text_engine",
                local_defines = select({
                    "//donner/svg/renderer:text_enabled": ["DONNER_TEXT_ENABLED"],
                    "//conditions:default": [],
                }) + select({
                    "//donner/svg/renderer:text_full_enabled": ["DONNER_TEXT_FULL"],
                    "//conditions:default": [],
                }),
            )
            """))

    def test_public_defines_fail(self):
        problems = _problems("""
            donner_cc_library(
                name = "text_engine",
                defines = select({
                    "//donner/svg/renderer:text_enabled": ["DONNER_TEXT_ENABLED"],
                    "//conditions:default": [],
                }),
            )
            """)
        self.assertEqual(1, len(problems), problems)
        self.assertRegex(problems[0], r"^donner/svg/text/BUILD\.bazel:5: text_engine passes a text "
                                      r"tier macro through `defines`")

    def test_every_macro_and_value_form_is_caught(self):
        problems = _problems("""
            cc_library(
                name = "consumer",
                defines = ["DONNER_TEXT_FULL"],
                copts = ["-DDONNER_TEXT_WOFF2_ENABLED"],
                local_defines = ["DONNER_TEXT_ENABLED=1"],
            )
            """, path="donner/svg/BUILD.bazel")
        self.assertEqual(3, len(problems), problems)
        self.assertIn("through `defines`", problems[0])
        self.assertIn("through `copts`", problems[1])
        self.assertIn("without a reviewed reason", problems[2])

    def test_reviewed_target_in_another_file_is_not_reviewed(self):
        problems = _problems("""
            cc_library(
                name = "text_engine",
                local_defines = ["DONNER_TEXT_ENABLED"],
            )
            """, path="donner/svg/BUILD.bazel")
        self.assertEqual(1, len(problems), problems)

    def test_macro_outside_a_rule_attribute_fails(self):
        problems = _problems("""
            def wrapper(**kwargs):
                kwargs["defines"] = ["DONNER_TEXT_ENABLED"]
                native.cc_library(**kwargs)
            """, path="build_defs/example.bzl")
        self.assertEqual(1, len(problems), problems)
        self.assertIn("outside a reviewed `local_defines`", problems[0])

    def test_unrelated_macros_and_comments_pass(self):
        self.assertEqual([], _problems("""
            # DONNER_TEXT_ENABLED is mentioned in a comment only.
            cc_library(
                name = "renderer_tiny_skia",
                defines = ["DONNER_FILTERS_ENABLED", "DONNER_TEXT_ENABLED_SOMETHING_ELSE"],
            )
            """))


def _reader(files):
    """A repository reader in which every reviewed entry is a live target.

    Each reviewed target sets a macro privately and lists `<name>.cc`, except text_tier, which
    lists TextTier.cc.
    """
    def read(path):
        if path in files:
            return files[path]
        targets = sorted(name for build, name in check.REVIEWED_LOCAL_DEFINES if build == path)
        return "".join(
            'cc_library(name = "%s", srcs = ["%s"], local_defines = ["DONNER_TEXT_FULL"])\n'
            % (name, "TextTier.cc" if name == "text_tier" else name + ".cc")
            for name in targets) or None
    return read


def _tree_problems(files):
    return check.problems_of(files, _reader(files))


class TextTierSourcesTest(unittest.TestCase):
    def test_reviewed_target_sources_pass(self):
        self.assertEqual([], _tree_problems(
            {"donner/svg/text/TextTier.cc": "#ifdef DONNER_TEXT_ENABLED\n#endif\n"}))

    def test_header_fails_even_in_a_reviewed_package(self):
        problems = _tree_problems({"donner/svg/text/TextTier.h": "// DONNER_TEXT_FULL\n"})
        self.assertEqual(1, len(problems), problems)
        self.assertRegex(problems[0], r"^donner/svg/text/TextTier\.h:1: a header must not")

    def test_every_header_suffix_fails(self):
        for suffix in (".h", ".hh", ".hpp", ".inc"):
            with self.subTest(suffix=suffix):
                self.assertEqual(1, len(_tree_problems(
                    {"donner/svg/Example" + suffix: "#if DONNER_TEXT_WOFF2_ENABLED\n"})))

    def test_source_outside_reviewed_targets_fails(self):
        problems = _tree_problems({"donner/svg/text/Other.cc": "\n#ifdef DONNER_TEXT_ENABLED\n"})
        self.assertEqual(1, len(problems), problems)
        self.assertRegex(problems[0], r"^donner/svg/text/Other\.cc:2: only the sources of a "
                                      r"reviewed target")

    def test_bazelrc_define_fails_and_comment_passes(self):
        problems = _tree_problems({".bazelrc": "# DONNER_TEXT_FULL is private\n"
                                               "build --copt=-DDONNER_TEXT_FULL\n"})
        self.assertEqual([".bazelrc:2: a text tier macro must not be set from a .bazelrc file"],
                         problems)

    def test_stale_reviewed_entry_fails(self):
        live = _reader({})("donner/svg/text/BUILD.bazel")
        stale = live.replace('"TextTier.cc"], local_defines = ["DONNER_TEXT_FULL"]',
                             '"TextTier.cc"]')
        self.assertNotEqual(live, stale)
        files = {"donner/svg/text/BUILD.bazel": stale}
        problems = check.problems_of(files, _reader(files), full=False)
        self.assertEqual(1, len(problems), problems)
        self.assertRegex(problems[0], r"^donner/svg/text/BUILD\.bazel: reviewed entry text_tier "
                                      r"sets no text tier macro")

    def test_missing_reviewed_target_fails_only_for_its_file_when_partial(self):
        files = {"donner/svg/BUILD.bazel": ""}
        read = _reader({})

        def without_text(path):
            return None if path == "donner/svg/text/BUILD.bazel" else read(path)

        self.assertEqual([], check.problems_of(files, without_text, full=False))
        stale = check.problems_of(files, without_text, full=True)
        self.assertEqual(sum(1 for build, _ in check.REVIEWED_LOCAL_DEFINES
                             if build == "donner/svg/text/BUILD.bazel"), len(stale), stale)


if __name__ == "__main__":
    unittest.main()
