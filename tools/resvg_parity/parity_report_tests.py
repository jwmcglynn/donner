#!/usr/bin/env python3
"""Verify registration parsing and corpus accounting without rendering SVGs.

Live inventory totals are report metrics; controlled fixtures test their meaning.
"""

from __future__ import annotations

from collections import Counter

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

    def add_case(self, relative: str, reference: bytes | None = None) -> None:
        path = self.tests_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("<svg/>\n", encoding="utf-8")
        if reference is not None:
            path.with_suffix(".png").write_bytes(reference)

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

    def test_resolving_exceptions_preserves_corpus_coverage(self) -> None:
        for filename in ("skip.svg", "render-only.svg", "compare.svg"):
            self.fixture.add_case("painting/fill/" + filename)
        before = parity_report.build_report(
            _registration(
                "PaintingFill",
                '\"painting/fill\", {'
                '{\"skip.svg\", Params::Skip(\"pending fix\")},'
                '{\"render-only.svg\", Params::RenderOnly(\"pending oracle\")},'
                '{\"compare.svg\", Params::WithThreshold(0.1f, 200, \"pending precision fix\")}'
                '}',
            ),
            self.fixture.tests_root,
        )
        after = parity_report.build_report(
            _registration("PaintingFill", '\"painting/fill\"'), self.fixture.tests_root
        )

        self.assertEqual([case["path"] for case in before["cases"]],
                         [case["path"] for case in after["cases"]])
        self.assertEqual(after["summary"]["total_cases"], 3)
        self.assertEqual(after["summary"]["compare_cases"], 3)
        self.assertEqual(after["summary"]["skip_cases"], 0)
        self.assertEqual(after["summary"]["render_only_cases"], 0)
        self.assertEqual(after["summary"]["pixel_budget_sources"], 0)

    def test_category_budget_sources_and_affected_files_are_separate(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        self.fixture.add_case("painting/fill/b.svg")
        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '\"painting/fill\", {}, Params::WithThreshold(0.1f, 200, \"category budget\")',
            ),
            self.fixture.tests_root,
        )

        self.assertEqual(report["summary"]["pixel_budget_sources"], 1)
        self.assertEqual(report["summary"]["effective_pixel_budget_cases"], 2)

    def test_shared_goldens_do_not_imply_backend_specific_goldens(self) -> None:
        self.fixture.add_case("painting/fill/shared.svg")
        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '\"painting/fill\", {{\"shared.svg\", Params::WithGoldenOverride(\"shared.png\")}}',
            ),
            self.fixture.tests_root,
        )

        self.assertEqual(report["cases"][0]["exception_types"], ["shared_golden"])
        self.assertEqual(report["summary"]["shared_golden_cases"], 1)
        self.assertEqual(report["summary"]["geode_golden_cases"], 0)

    def test_render_only_reasons_must_mark_undefined_behavior(self) -> None:
        for name in ("ub.svg", "bug.svg", "empty.svg", "skip.svg"):
            self.fixture.add_case(f"painting/fill/{name}")
        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '\"painting/fill\", {'
                '{\"ub.svg\", Params::RenderOnly(\"UB: negative opacity\")},'
                '{\"bug.svg\", Params::RenderOnly(\"Bug #1: glyph weight differs\")},'
                '{\"empty.svg\", Params::RenderOnly(\"UB: \")},'
                '{\"skip.svg\", Params::Skip(\"Bug #1: glyph weight differs\")}'
                '}',
            ),
            self.fixture.tests_root,
        )

        self.assertEqual(
            parity_report.render_only_policy_violations(report),
            [
                "painting/fill/bug.svg: 'Bug #1: glyph weight differs'",
                "painting/fill/empty.svg: 'UB: '",
            ],
        )

    def test_render_only_category_default_is_checked_per_case(self) -> None:
        self.fixture.add_case("filters/enable-background/a.svg")
        report = parity_report.build_report(
            _registration(
                "FiltersEnableBackground",
                '\"filters/enable-background\", {}, Params::RenderOnly(\"Deprecated feature\")',
            ),
            self.fixture.tests_root,
        )

        self.assertEqual(
            parity_report.render_only_policy_violations(report),
            ["filters/enable-background/a.svg: 'Deprecated feature'"],
        )

    def test_render_only_references_must_be_the_ub_marker_or_allowlisted(self) -> None:
        marker = b"ub marker bytes"
        self.fixture.add_case("painting/fill/marker.svg", marker)
        self.fixture.add_case("painting/fill/bad.svg", b"a real rendering")
        self.fixture.add_case("painting/fill/spec-ub.svg", b"an implementation's rendering")
        self.fixture.add_case("painting/fill/marker-listed.svg", marker)
        self.fixture.add_case("painting/fill/stale.svg", b"now compared")
        marker_path = self.fixture.root / "ub.png"
        marker_path.write_bytes(marker)
        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '\"painting/fill\", {'
                '{\"marker.svg\", Params::RenderOnly(\"UB: marker\")},'
                '{\"bad.svg\", Params::RenderOnly(\"UB: claimed without a marker\")},'
                '{\"spec-ub.svg\", Params::RenderOnly(\"UB: implementation-defined\")},'
                '{\"marker-listed.svg\", Params::RenderOnly(\"UB: marker\")}'
                '}',
            ),
            self.fixture.tests_root,
        )

        violations = parity_report.render_only_reference_violations(
            report,
            self.fixture.tests_root,
            marker_path,
            {"painting/fill/spec-ub.svg", "painting/fill/marker-listed.svg",
             "painting/fill/stale.svg"},
        )

        self.assertEqual(
            violations,
            [
                "painting/fill/bad.svg: reference is not the UB marker and the case is not "
                "allowlisted as specification UB",
                "painting/fill/marker-listed.svg: allowlisted, but its reference is the UB marker",
                "painting/fill/stale.svg: allowlisted, but not an active render-only case",
            ],
        )

    def test_state_fields_cannot_bypass_the_named_factories(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        for params in ("Params{.renderOnly = true}", "Params{.skip = true}"):
            with self.subTest(params=params):
                with self.assertRaisesRegex(parity_report.ParityReportError,
                                            "through Params::Skip or Params::RenderOnly"):
                    parity_report.build_report(
                        _registration("PaintingFill",
                                      f'\"painting/fill\", {{{{\"a.svg\", {params}}}}}'),
                        self.fixture.tests_root,
                    )

        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '\"painting/fill\", {{\"a.svg\", Params().withReason(\"not renderOnly or skip\")}}',
            ),
            self.fixture.tests_root,
        )
        self.assertEqual(report["cases"][0]["primary_state"], "compare")

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
    def test_current_corpus_is_fully_registered_and_consistently_reported(self) -> None:
        repo_root = parity_report._default_repo_root()
        self.assertTrue(
            (repo_root / parity_report.REGISTRATION_SOURCE).is_file(),
            "registration source is missing from the Bazel runfiles tree",
        )
        tests_root = repo_root / parity_report.TESTS_ROOT
        self.assertTrue(
            tests_root.is_dir(),
            "vendored resvg corpus is missing from the Bazel runfiles tree",
        )

        report = parity_report.generate_report(repo_root)
        cases = report["cases"]
        summary = report["summary"]
        vendored_paths = {path.relative_to(tests_root).as_posix()
                          for path in tests_root.rglob("*.svg")}
        reported_paths = [case["path"] for case in cases]

        self.assertEqual(set(reported_paths), vendored_paths)
        self.assertEqual(len(reported_paths), len(vendored_paths), "duplicate reported SVG paths")
        self.assertEqual({case["registration"] for case in cases}, {"active"})
        self.assertEqual(report["source"]["disabled_categories"], [])
        self.assertEqual(summary["total_cases"], len(vendored_paths))
        self.assertEqual(summary["active_cases"], len(cases))
        self.assertEqual(summary["disabled_cases"], 0)

        states = Counter(case["primary_state"] for case in cases)
        self.assertLessEqual(set(states), {"compare", "skip", "render-only"})
        for metric, state in (("compare_cases", "compare"), ("skip_cases", "skip"),
                              ("render_only_cases", "render-only")):
            self.assertEqual(summary[metric], states[state], metric)
        self.assertEqual(sum(states.values()), summary["active_cases"])

        metrics = {
            "effective_pixel_budget_cases": "threshold",
            "shared_golden_cases": "shared_golden",
            "geode_golden_cases": "geode_golden",
            "geode_pixel_budget_cases": "geode_threshold",
            "simple_text_pixel_budget_cases": "simple_text_threshold",
            "text_full_gated_cases": "text_full_gate",
        }
        for metric, flag in metrics.items():
            self.assertEqual(summary[metric],
                             sum(flag in case["exception_types"] for case in cases), metric)
        self.assertEqual(summary["backend_disabled_cases"],
                         sum(bool(case["disabled_backends"]) for case in cases))

    def test_live_render_only_rows_are_reserved_for_undefined_behavior(self) -> None:
        report = parity_report.generate_report(parity_report._default_repo_root())

        violations = parity_report.render_only_policy_violations(report)

        self.assertEqual(
            violations,
            [],
            "Params::RenderOnly is reserved for undefined or implementation-defined behavior "
            f"and its reason must start with {parity_report.RENDER_ONLY_REASON_PREFIX!r}; use "
            "Params::Skip with a bug number or reference reason instead:\n" + "\n".join(violations),
        )

    def test_live_render_only_references_are_the_ub_marker_or_spec_ub(self) -> None:
        repo_root = parity_report._default_repo_root()
        report = parity_report.generate_report(repo_root)

        violations = parity_report.render_only_reference_violations(
            report,
            repo_root / parity_report.TESTS_ROOT,
            repo_root / parity_report.UB_MARKER_REFERENCE,
        )

        self.assertEqual(
            violations,
            [],
            "Params::RenderOnly cases need the resvg UB marker reference "
            f"({parity_report.UB_MARKER_REFERENCE}) or an entry in "
            "SPEC_UB_RENDER_ONLY_ALLOWLIST explaining the specification-level UB:\n"
            + "\n".join(violations),
        )


if __name__ == "__main__":
    unittest.main()
