import importlib.util
import sys
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("feature_loc.py")
MODULE_SPEC = importlib.util.spec_from_file_location("feature_loc", MODULE_PATH)
assert MODULE_SPEC is not None
assert MODULE_SPEC.loader is not None
feature_loc = importlib.util.module_from_spec(MODULE_SPEC)
sys.modules[MODULE_SPEC.name] = feature_loc
MODULE_SPEC.loader.exec_module(feature_loc)


class FeatureLocTests(unittest.TestCase):
    def test_parse_cloc_csv_skips_sum_row(self):
        rows = feature_loc.parse_cloc_by_file_csv(
            "\n".join(
                [
                    "language,filename,blank,comment,code,version",
                    "C++,donner/svg/text/TextEngine.cc,1,2,42",
                    "SUM,,1,2,42",
                ]
            )
        )

        self.assertEqual(
            rows,
            [feature_loc.FileLoc(path="donner/svg/text/TextEngine.cc", code=42)],
        )

    def test_feature_for_path_uses_first_specific_bucket(self):
        self.assertEqual(
            feature_loc.feature_for_path(
                "donner/svg/renderer/FilterGraphExecutor.cc"
            ).label,
            "Filters",
        )
        self.assertEqual(
            feature_loc.feature_for_path("donner/svg/renderer/RendererDriver.cc").label,
            "Rendering",
        )
        self.assertEqual(
            feature_loc.feature_for_path("donner/editor/TextEditor.cc").label,
            "Editor",
        )

    def test_normalize_paths_makes_absolute_paths_workspace_relative(self):
        rows = feature_loc.normalize_paths(
            [
                feature_loc.FileLoc(
                    "/workspace/donner/svg/renderer/RendererDriver.cc", 10
                )
            ],
            Path("/workspace"),
        )

        self.assertEqual(
            rows,
            [feature_loc.FileLoc("donner/svg/renderer/RendererDriver.cc", 10)],
        )

    def test_aggregate_splits_product_and_support_loc(self):
        stats = feature_loc.aggregate_by_feature(
            [
                feature_loc.FileLoc("donner/svg/text/TextEngine.cc", 100),
                feature_loc.FileLoc("donner/svg/renderer/tests/TextEngine_tests.cc", 25),
                feature_loc.FileLoc("donner/svg/renderer/RendererDriver.cc", 50),
            ]
        )

        self.assertEqual(stats["Text"].product_loc, 100)
        self.assertEqual(stats["Text"].support_loc, 25)
        self.assertEqual(stats["Rendering"].product_loc, 50)

    def test_breakdown_is_scoped_to_parent_feature(self):
        stats = feature_loc.aggregate_by_breakdown(
            [
                feature_loc.FileLoc("donner/editor/TextEditor.cc", 100),
                feature_loc.FileLoc("donner/editor/tests/TextEditor_tests.cc", 25),
                feature_loc.FileLoc("donner/svg/renderer/tests/TextEngine_tests.cc", 10),
            ],
            "Editor",
            feature_loc.EDITOR_BREAKDOWN,
        )

        self.assertEqual(stats["Text editor"].product_loc, 100)
        self.assertEqual(stats["Text editor"].support_loc, 25)
        self.assertEqual(sum(feature_stats.total_loc for feature_stats in stats.values()), 125)

    def test_render_markdown_table_includes_totals(self):
        stats = feature_loc.aggregate_by_feature(
            [
                feature_loc.FileLoc("donner/svg/components/filter/FilterSystem.cc", 80),
                feature_loc.FileLoc(
                    "donner/svg/components/filter/tests/FilterSystem_tests.cc", 20
                ),
            ]
        )

        table = feature_loc.render_markdown_table(stats)

        self.assertIn("| Filters | 80 | 20 | 100 | 2 |", table)
        self.assertIn("| **Total** | **80** | **20** | **100** | **2** |", table)

    def test_render_markdown_includes_editor_and_rendering_breakdowns(self):
        report = feature_loc.render_markdown(
            [
                feature_loc.FileLoc("donner/editor/TextEditor.cc", 100),
                feature_loc.FileLoc("donner/svg/renderer/RendererDriver.cc", 50),
            ]
        )

        self.assertIn("### Editor Breakdown", report)
        self.assertIn("| Text editor | 100 | 0 | 100 | 1 |", report)
        self.assertIn("### Rendering Breakdown", report)
        self.assertIn("| Traversal and render context | 50 | 0 | 50 | 1 |", report)


if __name__ == "__main__":
    unittest.main()
