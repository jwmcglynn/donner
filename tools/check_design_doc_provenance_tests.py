import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("check_design_doc_provenance.py")
MODULE_SPEC = importlib.util.spec_from_file_location("check_design_doc_provenance", MODULE_PATH)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
provenance = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = provenance
MODULE_SPEC.loader.exec_module(provenance)


class DesignDocProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.design_root = self.root / "docs" / "design_docs"
        self.design_root.mkdir(parents=True)
        self.allowlist = self.design_root / "provenance_debt.txt"
        self.allowlist.write_text("", encoding="utf-8")

    def tearDown(self):
        self.temporary_directory.cleanup()

    def write_design(self, name: str, metadata: str) -> Path:
        path = self.design_root / name
        path.write_text(f"# Design: Test\n\n{metadata}\n", encoding="utf-8")
        return path

    def test_exact_model_identifier_passes(self):
        self.write_design("0001-test.md", "**Author:** GPT-5.6 Sol")

        self.assertEqual(provenance.check(self.root, self.allowlist), [])

    def test_missing_author_fails(self):
        self.write_design("0001-test.md", "**Status:** Draft")

        self.assertIn("missing **Author:** metadata", provenance.check(self.root, self.allowlist)[0])

    def test_generic_surface_name_fails(self):
        self.write_design("0001-test.md", "**Author:** Codex")

        self.assertIn("generic surface name", provenance.check(self.root, self.allowlist)[0])

    def test_human_only_design_is_explicit(self):
        self.write_design(
            "0001-test.md",
            "**Author:** Ada Lovelace\n**Model:** None (human-only)",
        )

        self.assertEqual(provenance.check(self.root, self.allowlist), [])

    def test_human_author_with_exact_model_passes(self):
        self.write_design(
            "0001-test.md",
            "**Author:** Ada Lovelace\n**Model:** GPT-5.6 Sol",
        )

        self.assertEqual(provenance.check(self.root, self.allowlist), [])

    def test_author_metadata_must_not_contain_role_annotations(self):
        self.write_design(
            "0001-test.md",
            "**Author:** Claude Opus 4.8 (Architect)",
        )

        self.assertIn(
            "not an exact model identifier",
            provenance.check(self.root, self.allowlist)[0],
        )

    def test_allowlist_ratchets_historical_debt(self):
        path = self.write_design("0001-test.md", "**Author:** Codex")
        relative_path = path.relative_to(self.root).as_posix()
        self.allowlist.write_text(f"{relative_path}\n", encoding="utf-8")

        self.assertEqual(provenance.check(self.root, self.allowlist), [])

        path.write_text("# Design: Test\n\n**Author:** GPT-5\n", encoding="utf-8")
        self.assertIn("stale provenance-debt entry", provenance.check(self.root, self.allowlist)[0])

    def test_non_design_markdown_is_ignored(self):
        self.write_design("README.md", "No author metadata")
        self.write_design("developer_template.md", "No author metadata")

        self.assertEqual(provenance.check(self.root, self.allowlist), [])


if __name__ == "__main__":
    unittest.main()
