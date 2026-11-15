#!/usr/bin/env python3

import importlib.util
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("check_design_doc_index.py")
MODULE_SPEC = importlib.util.spec_from_file_location("check_design_doc_index", MODULE_PATH)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
index_check = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = index_check
MODULE_SPEC.loader.exec_module(index_check)


class DesignDocIndexTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.root = Path(self.temporary_directory.name)
        self.design_root = self.root / "docs" / "design_docs"
        (self.design_root / "text").mkdir(parents=True)
        self.rows = []
        self.pages = []

    def add_doc(self, path, status="Draft", *, indexed=None, nav=True, title="# Design: Test"):
        body = f"{title}\n\n" + (f"**Status:** {status}\n" if status is not None else "")
        (self.design_root / path).write_text(body, encoding="utf-8")
        cell = status if indexed is None else indexed
        if cell is not False:
            self.rows.append(f"| 0001 | [doc]({path}) | {cell} | Summary. |")
        if nav:
            self.pages.append(index_check.doxygen_page_id(f"docs/design_docs/{path}"))

    def check(self):
        readme = "\n".join(
            ["# Design Docs", "", "## Document Index", "", "| # | Doc | Status | Summary |",
             "| - | --- | ------ | ------- |", *self.rows, "", "## Cross-reference", "",
             "| x | [elsewhere](missing.md) | y | z |"]
        )
        navigation = "/**\n@page P Title\n" + "".join(f"- \\subpage {p}\n" for p in self.pages)
        (self.design_root / "README.md").write_text(readme + "\n", encoding="utf-8")
        (self.design_root / "navigation.dox").write_text(navigation + "*/\n", encoding="utf-8")
        return index_check.check(self.root)

    def test_consistent_index_passes(self):
        self.add_doc("0001-first.md", "Implemented")
        self.add_doc("0002-2-second.md", "In Progress - milestone one landed",
                     indexed="In Progress")
        self.add_doc("text/0003-nested.md", "Superseded by [0001](../0001-first.md)",
                     indexed="Superseded by [0001](../0001-first.md)")

        self.assertEqual([], self.check())

    def test_doxygen_page_ids_escape_underscores_and_directories(self):
        self.assertEqual(
            "md_docs_2design__docs_2text_20052-3-rtl__and__complex__scripts",
            index_check.doxygen_page_id("docs/design_docs/text/0052-3-rtl_and_complex_scripts.md"),
        )

    def test_status_keyword_stops_at_the_first_delimiter(self):
        cases = {
            "Shipped (Phases 1-2a, CMake backend selection)": "Shipped",
            "**Implemented (as-built).** Geode now computes": "Implemented",
            "Design\\": "Design",
            "Partial \u2014 core properties": "Partial",
            "In Progress - Milestones 1-5 complete": "In Progress",
            "Superseded by [0044-x](0044-x.md)": "Superseded by",
            "Fixed for the v0.8 replay; stress coverage added": "Fixed for the v0.8 replay",
            "Active release preparation. Downstream validation": "Active release preparation",
            "Implementing: wave 1": "Implementing",
        }
        for text, keyword in cases.items():
            with self.subTest(text=text):
                self.assertEqual(keyword, index_check.status_keyword(text))

    def test_reports_numbered_doc_missing_from_index_and_navigation(self):
        self.add_doc("0001-first.md", indexed=False, nav=False)
        self.add_doc("notes.md", indexed=False, nav=False)

        self.assertEqual(
            [
                "README.md: 0001-first.md is missing from the Document Index",
                "navigation.dox: 0001-first.md has no \\subpage md_docs_2design__docs_20001-first",
            ],
            self.check(),
        )

    def test_reports_index_row_for_missing_file(self):
        self.rows.append("| 0009 | [gone](0009-gone.md) | Draft | Summary. |")

        self.assertEqual(["README.md:7: index row links to missing file 0009-gone.md"],
                         self.check())

    def test_reports_navigation_page_for_missing_document(self):
        self.pages.append("md_docs_2design__docs_20009-gone")

        self.assertEqual(
            ["navigation.dox: \\subpage md_docs_2design__docs_20009-gone names no design document"],
            self.check(),
        )

    def test_reports_status_cell_that_disagrees_with_the_document(self):
        self.add_doc("0001-first.md", "Implementing (wave 1 landed)", indexed="Draft")
        self.add_doc("0002-second.md", "Implemented", indexed="Implemented later")

        self.assertEqual(
            [
                "README.md:7: status cell 'Draft' does not start with 0001-first.md's "
                "'Implementing'",
                "README.md:8: status cell 'Implemented later' does not start with "
                "0002-second.md's 'Implemented'",
            ],
            self.check(),
        )

    def test_reports_document_without_status_unless_exempt(self):
        self.add_doc("0005-unstated.md", None, indexed="Draft")
        self.add_doc("0001-terminal_image_viewer.md", None, indexed="\u2014")

        self.assertEqual(["README.md:7: 0005-unstated.md has no **Status:** line"], self.check())

    def test_title_anchor_names_the_navigation_page(self):
        self.add_doc("0001-first.md", nav=False, title="# First Guide {#FirstGuide}")
        self.pages.append("FirstGuide")

        self.assertEqual([], self.check())

    def test_main_reports_failure_exit_status(self):
        self.add_doc("0001-first.md", indexed="Design")
        self.check()

        with unittest.mock.patch("sys.stdout"), unittest.mock.patch("sys.stderr"):
            self.assertEqual(1, index_check.main(["check", f"--root={self.root}"]))


if __name__ == "__main__":
    unittest.main()
