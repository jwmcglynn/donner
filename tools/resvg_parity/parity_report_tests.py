#!/usr/bin/env python3
"""Tests for the generated resvg parity report."""

from __future__ import annotations

import io
import json
from pathlib import Path
import tempfile
import textwrap
import unittest

import parity_report


def _registration(suite: str, category_call: str) -> str:
    return textwrap.dedent(
        f"""
        INSTANTIATE_TEST_SUITE_P(
            {suite}, ImageComparisonTestFixture,
            Combine(ValuesIn(getTestsInCategory({category_call})),
                    ValuesIn(ActiveComparisonModes())),
            TestNameFromFilename);
        """
    )


class Fixture:
    def __init__(self) -> None:
        self._temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary_directory.name)
        self.tests_root = self.root / parity_report.TESTS_ROOT

    def add_case(self, relative: str) -> None:
        path = self.tests_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("<svg/>\n", encoding="utf-8")

    def close(self) -> None:
        self._temporary_directory.cleanup()


class ParityReportTests(unittest.TestCase):
    def setUp(self) -> None:
        self.fixture = Fixture()

    def tearDown(self) -> None:
        self.fixture.close()

    def test_balanced_cpp_parsing(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source = _registration(
            "PaintingFill",
            textwrap.dedent(
                """
                "painting/fill",
                {
                    {"a.svg", Params::WithThreshold(
                        Calculate(0.1f, Nested(2, 3)), 120,
                        "reason with (parentheses), braces {ok}, and // text")},
                }
                """
            ),
        )

        report = parity_report.build_report(source, self.fixture.tests_root)

        case = report["cases"][0]
        self.assertEqual(case["primary_state"], "compare")
        self.assertIn("threshold", case["exception_types"])
        self.assertEqual(case["reason"], "reason with (parentheses), braces {ok}, and // text")

    def test_adjacent_string_literals_form_filename_and_reason(self) -> None:
        self.fixture.add_case("text/text/joined.svg")
        source = _registration(
            "TextText",
            textwrap.dedent(
                """
                "text/" "text",
                {
                    {"join" "ed.svg", Params::Skip("needs " "two pieces")},
                }
                """
            ),
        )

        case = parity_report.build_report(source, self.fixture.tests_root)["cases"][0]

        self.assertEqual(case["reason"], "needs two pieces")
        self.assertEqual(case["raw_params_expression"], 'Params::Skip("needs " "two pieces")')
        self.assertEqual(case["primary_state"], "skip")

    def test_default_category_params_apply_to_every_case(self) -> None:
        self.fixture.add_case("filters/defaults/a.svg")
        self.fixture.add_case("filters/defaults/b.svg")
        source = _registration(
            "FiltersDefaults",
            '"filters/defaults", {}, Params::RenderOnly("category default")',
        )

        report = parity_report.build_report(source, self.fixture.tests_root)

        self.assertEqual([case["primary_state"] for case in report["cases"]], [
            "render-only",
            "render-only",
        ])
        self.assertEqual([case["params_source"] for case in report["cases"]], [
            "category-default",
            "category-default",
        ])
        self.assertEqual(report["summary"]["render_only_cases"], 2)

    def test_disabled_comment_block_is_registration_but_example_is_not(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        self.fixture.add_case("filters/filter-functions/f.svg")
        source = (
            '// Example only: getTestsInCategory("painting/not-registered")\n'
            + _registration("PaintingFill", '"painting/fill"')
            + textwrap.dedent(
                """
                // INSTANTIATE_TEST_SUITE_P(
                //     FiltersFilterFunctions, ImageComparisonTestFixture,
                //     Combine(ValuesIn(getTestsInCategory("filters/filter-functions")),
                //             ValuesIn(ActiveComparisonModes())),
                //     TestNameFromFilename);
                """
            )
        )

        report = parity_report.build_report(source, self.fixture.tests_root)

        self.assertEqual(report["source"]["disabled_categories"], ["filters/filter-functions"])
        cases = {case["path"]: case for case in report["cases"]}
        self.assertEqual(cases["painting/fill/a.svg"]["registration"], "active")
        self.assertEqual(cases["filters/filter-functions/f.svg"]["registration"], "disabled")
        self.assertEqual(cases["filters/filter-functions/f.svg"]["comparison_modes"], [])

    def test_disabled_pixel_budget_is_not_an_active_source(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        self.fixture.add_case("filters/filter-functions/f.svg")
        source = _registration("PaintingFill", '"painting/fill"') + textwrap.dedent(
            """
            // INSTANTIATE_TEST_SUITE_P(
            //     FiltersFilterFunctions, ImageComparisonTestFixture,
            //     Combine(ValuesIn(getTestsInCategory(
            //                 "filters/filter-functions", {},
            //                 Params::WithThreshold(0.1f, 100, "disabled"))),
            //             ValuesIn(ActiveComparisonModes())),
            //     TestNameFromFilename);
            """
        )

        report = parity_report.build_report(source, self.fixture.tests_root)

        self.assertEqual(report["summary"]["pixel_budget_sources"], 0)
        disabled = next(case for case in report["cases"] if case["registration"] == "disabled")
        self.assertIn("threshold", disabled["exception_types"])

    def test_overlapping_exception_types_and_final_reason(self) -> None:
        self.fixture.add_case("text/text/a.svg")
        source = _registration(
            "TextText",
            textwrap.dedent(
                """
                "text/text",
                {
                    {"a.svg", Params::WithGoldenOverride("shared.png")
                        .withGeodeGoldenOverride("geode.png", "geode " "reason")
                        .withMaxPixelsDifferent(200)
                        .onlyTextFull()},
                }
                """
            ),
        )

        case = parity_report.build_report(source, self.fixture.tests_root)["cases"][0]

        self.assertEqual(
            case["exception_types"],
            ["threshold", "shared_golden", "geode_golden", "text_full_gate"],
        )
        self.assertEqual(case["reason"], "geode reason")
        self.assertEqual(case["backend_requirements"], ["text", "text-full"])

    def test_backend_gate_reasons_are_separate_from_comparison_reasons(self) -> None:
        self.fixture.add_case("painting/fill/required.svg")
        self.fixture.add_case("painting/fill/disabled.svg")
        source = _registration(
            "PaintingFill",
            textwrap.dedent(
                """
                "painting/fill",
                {
                    {"required.svg", Params::WithThreshold(0.1f, 2, "pixel budget")
                        .requireFeature(RendererBackendFeature::Text, "text rendering")},
                    {"disabled.svg", Params().disableBackend(
                        RendererBackend::Geode, "Geode path unavailable")},
                }
                """
            ),
        )

        cases = {
            case["path"]: case
            for case in parity_report.build_report(source, self.fixture.tests_root)["cases"]
        }

        required = cases["painting/fill/required.svg"]
        self.assertEqual(required["reason"], "pixel budget")
        self.assertEqual(required["backend_requirement_reason"], "text rendering")
        disabled = cases["painting/fill/disabled.svg"]
        self.assertIsNone(disabled["reason"])
        self.assertEqual(disabled["backend_requirement_reason"], "Geode path unavailable")

    def test_unregistered_category_fails_closed(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        self.fixture.add_case("painting/stroke/b.svg")
        source = _registration("PaintingStroke", '"painting/stroke"')
        with self.assertRaisesRegex(parity_report.ParityReportError, "unregistered"):
            parity_report.build_report(source, self.fixture.tests_root)

    def test_duplicate_registration_fails_closed(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source = _registration("One", '"painting/fill"') + _registration(
            "Two", '"painting/fill"'
        )
        with self.assertRaisesRegex(parity_report.ParityReportError, "duplicate category"):
            parity_report.build_report(source, self.fixture.tests_root)

    def test_override_without_vendored_case_fails_closed(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source = _registration(
            "PaintingFill",
            '"painting/fill", {{"missing.svg", Params::Skip("missing")}}',
        )
        with self.assertRaisesRegex(parity_report.ParityReportError, "overrides missing"):
            parity_report.build_report(source, self.fixture.tests_root)

    def test_duplicate_override_fails_closed(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source = _registration(
            "PaintingFill",
            textwrap.dedent(
                """
                "painting/fill",
                {
                    {"a.svg", Params::Skip("one")},
                    {"a.svg", Params::Skip("two")},
                }
                """
            ),
        )
        with self.assertRaisesRegex(parity_report.ParityReportError, "duplicate override"):
            parity_report.build_report(source, self.fixture.tests_root)

    def test_json_is_deterministic_and_cases_are_sorted(self) -> None:
        self.fixture.add_case("painting/fill/z.svg")
        self.fixture.add_case("painting/fill/a.svg")
        source = _registration("PaintingFill", '"painting/fill"')

        first = parity_report.build_report(source, self.fixture.tests_root)
        second = parity_report.build_report(source, self.fixture.tests_root)
        first_json = json.dumps(first, indent=2, sort_keys=True, ensure_ascii=True)
        second_json = json.dumps(second, indent=2, sort_keys=True, ensure_ascii=True)

        self.assertEqual(first_json, second_json)
        self.assertEqual([case["path"] for case in first["cases"]], [
            "painting/fill/a.svg",
            "painting/fill/z.svg",
        ])

    def test_cli_resolves_explicit_repo_root_independent_of_cwd(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source_path = self.fixture.root / parity_report.REGISTRATION_SOURCE
        source_path.parent.mkdir(parents=True, exist_ok=True)
        source_path.write_text(_registration("PaintingFill", '"painting/fill"'), encoding="utf-8")
        stdout = io.StringIO()

        result = parity_report.main(["--repo-root", str(self.fixture.root)], stdout=stdout)

        self.assertEqual(result, 0)
        self.assertEqual(json.loads(stdout.getvalue())["summary"]["total_cases"], 1)


class RealTreeParityReportTests(unittest.TestCase):
    def test_current_source_facts(self) -> None:
        repo_root = parity_report._default_repo_root()
        self.assertTrue(
            (repo_root / parity_report.REGISTRATION_SOURCE).is_file(),
            "registration source is missing from the Bazel runfiles tree",
        )
        self.assertTrue(
            (repo_root / parity_report.TESTS_ROOT).is_dir(),
            "vendored resvg corpus is missing from the Bazel runfiles tree",
        )

        report = parity_report.generate_report(repo_root)
        summary = report["summary"]

        self.assertEqual(summary["total_cases"], 1679)
        self.assertEqual(summary["active_cases"], 1636)
        self.assertEqual(summary["disabled_cases"], 43)
        self.assertEqual(summary["skip_cases"], 124)
        self.assertEqual(summary["render_only_cases"], 78)
        self.assertEqual(summary["pixel_budget_sources"], 103)
        self.assertEqual(summary["effective_pixel_budget_cases"], 125)
        self.assertEqual(summary["shared_golden_cases"], 36)
        self.assertEqual(summary["geode_golden_cases"], 6)
        self.assertEqual(summary["backend_disabled_cases"], 0)


if __name__ == "__main__":
    unittest.main()
