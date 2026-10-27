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
            '"filters/defaults", {}, Params::DeprecatedInSVG2("category default")',
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
        self.assertEqual(report["summary"]["render_only_deprecated_in_svg2_cases"], 2)
        self.assertEqual(report["summary"]["render_only_undefined_behavior_cases"], 0)

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
                '{\"render-only.svg\", Params::UndefinedBehavior(\"pending oracle\")},'
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

    def test_render_only_cases_must_use_a_named_bucket_with_a_reason(self) -> None:
        for name in ("ub.svg", "deprecated.svg", "generic.svg", "empty.svg", "skip.svg"):
            self.fixture.add_case(f"painting/fill/{name}")
        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '\"painting/fill\", {'
                '{\"ub.svg\", Params::UndefinedBehavior(\"negative opacity\")},'
                '{\"deprecated.svg\", Params::DeprecatedInSVG2(\"icc-color() paint\")},'
                '{\"generic.svg\", Params::RenderOnly(\"Bug #1: glyph weight differs\")},'
                '{\"empty.svg\", Params::UndefinedBehavior(\" \")},'
                '{\"skip.svg\", Params::Skip(\"Bug #1: glyph weight differs\")}'
                '}',
            ),
            self.fixture.tests_root,
        )

        kinds = {case["path"]: case["render_only_kind"] for case in report["cases"]}
        self.assertEqual(kinds["painting/fill/ub.svg"], "undefined-behavior")
        self.assertEqual(kinds["painting/fill/deprecated.svg"], "deprecated-in-svg2")
        self.assertIsNone(kinds["painting/fill/skip.svg"])
        self.assertEqual(report["summary"]["render_only_cases"], 4)
        self.assertEqual(report["summary"]["render_only_undefined_behavior_cases"], 2)
        self.assertEqual(report["summary"]["render_only_deprecated_in_svg2_cases"], 1)
        self.assertEqual(
            parity_report.render_only_policy_violations(report),
            [
                "painting/fill/empty.svg: undefined-behavior case has no reason",
                "painting/fill/generic.svg: render-only without Params::UndefinedBehavior or "
                "Params::DeprecatedInSVG2",
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
            ["filters/enable-background/a.svg: render-only without Params::UndefinedBehavior or "
             "Params::DeprecatedInSVG2"],
        )

    def test_undefined_behavior_references_must_be_the_ub_marker_or_allowlisted(self) -> None:
        marker = b"ub marker bytes"
        self.fixture.add_case("painting/fill/marker.svg", marker)
        self.fixture.add_case("painting/fill/bad.svg", b"a real rendering")
        self.fixture.add_case("painting/fill/spec-ub.svg", b"an implementation's rendering")
        self.fixture.add_case("painting/fill/marker-listed.svg", marker)
        self.fixture.add_case("painting/fill/stale.svg", b"now compared")
        self.fixture.add_case("painting/fill/deprecated.svg", b"a deprecated feature")
        marker_path = self.fixture.root / "ub.png"
        marker_path.write_bytes(marker)
        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '\"painting/fill\", {'
                '{\"marker.svg\", Params::UndefinedBehavior(\"marker\")},'
                '{\"bad.svg\", Params::UndefinedBehavior(\"claimed without a marker\")},'
                '{\"spec-ub.svg\", Params::UndefinedBehavior(\"implementation-defined\")},'
                '{\"marker-listed.svg\", Params::UndefinedBehavior(\"marker\")},'
                '{\"deprecated.svg\", Params::DeprecatedInSVG2(\"not a UB case\")}'
                '}',
            ),
            self.fixture.tests_root,
        )

        violations = parity_report.undefined_behavior_reference_violations(
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
                "painting/fill/stale.svg: allowlisted, but not an active undefined-behavior case",
            ],
        )

    def test_deprecated_in_svg2_cases_must_name_a_listed_feature(self) -> None:
        for relative in ("text/tref/a.svg", "text/tref/b.svg", "painting/fill/icc.svg",
                         "painting/fill/bug.svg"):
            self.fixture.add_case(relative)
        source = "".join((
            _registration("TextTref", '\"text/tref\", {}, Params::DeprecatedInSVG2(\"<tref>\")'),
            _registration(
                "PaintingFill",
                '\"painting/fill\", {'
                '{\"icc.svg\", Params::DeprecatedInSVG2(\"icc-color() paint\")},'
                '{\"bug.svg\", Params::DeprecatedInSVG2(\"actually a bug\")}'
                '}',
            ),
        ))
        report = parity_report.build_report(source, self.fixture.tests_root)

        violations = parity_report.deprecated_in_svg2_violations(
            report,
            {"text/tref": "<tref>", "painting/fill/icc.svg": "icc-color() paint",
             "text/kerning": "kerning property"},
        )

        self.assertEqual(
            violations,
            [
                "painting/fill/bug.svg: not under a feature in DEPRECATED_IN_SVG2_FEATURES",
                "text/kerning: listed in DEPRECATED_IN_SVG2_FEATURES, but no active "
                "deprecated-in-svg2 case uses it",
            ],
        )

    def test_reference_budgets_follow_the_evidence_rule(self) -> None:
        gen = "Reference rasterizer generation: crosshair; "
        label = "Reference label glyphs: heavier labels; "
        measured = "analytic residual 398 px, supersampled 0 px"
        overrides = {
            "ok": f'Params().withMaxPixelsDifferent(410).withReason("{gen}{measured}")',
            "label-ok": f'WithMaxPixels(390, "{label}analytic residual 375 px, supersampled 302 px")',
            "full-only": f'Params().withMaxPixelsDifferent(410).onlyTextFull().withReason('
                         f'"{gen}{measured}")',
            "skip": f'Params::Skip("{gen}not measured")',
            "unbudgeted": f'Params().withReason("{gen}{measured}")',
            "widened": f'Params::WithThreshold(0.1f, 410, "{gen}{measured}")',
            "no-residual": f'Params().withMaxPixelsDifferent(410).withReason("{gen}supersampled 0 px")',
            "no-supersampled": f'Params().withMaxPixelsDifferent(410).withReason('
                               f'"{gen}analytic residual 398 px")',
            "not-generation": f'Params().withMaxPixelsDifferent(410).withReason('
                              f'"{gen}analytic residual 398 px, supersampled 297 px")',
            "label-no-supersampled": f'WithMaxPixels(390, "{label}analytic residual 375 px")',
            "too-large": f'Params().withMaxPixelsDifferent(415).withReason("{gen}{measured}")',
            "simple-text": f'Params().withMaxPixelsDifferent(410).withSimpleTextMaxPixels(900)'
                           f'.withReason("{gen}{measured}")',
            "geode-budget": f'Params().withMaxPixelsDifferent(410).withGeodeMaxPixelsDifferent(900)'
                            f'.withReason("{gen}{measured}")',
            "shared-golden": f'Params::WithGoldenOverride("g.png").withMaxPixelsDifferent(410)'
                             f'.withReason("{gen}{measured}")',
            "geode-golden": f'Params().withMaxPixelsDifferent(410).withGeodeGoldenOverride('
                            f'"g.png", "{gen}{measured}")',
            "other": 'Params().withMaxPixelsDifferent(900).withReason("glyph AA")',
        }
        for name in overrides:
            self.fixture.add_case(f"text/text/{name}.svg")
        self.fixture.add_case("text/tspan/category.svg")
        entries = ",".join(f'{{"{name}.svg", {expression}}}' for name, expression in overrides.items())
        source = "".join((
            _registration("TextText", f'"text/text", {{{entries}}}'),
            _registration(
                "TextTspan",
                f'"text/tspan", {{}}, Params().withMaxPixelsDifferent(410).withReason('
                f'"{gen}{measured}")',
            ),
        ))
        report = parity_report.build_report(source, self.fixture.tests_root)

        self.assertEqual(
            parity_report.reference_budget_violations(report),
            [
                "text/text/geode-budget.svg: combines the budget with geode_threshold",
                "text/text/geode-golden.svg: combines the budget with geode_golden",
                "text/text/label-no-supersampled.svg: reason does not state the supersampled "
                "residual",
                "text/text/no-residual.svg: reason does not state the analytic residual",
                "text/text/no-supersampled.svg: reason does not state the supersampled residual",
                "text/text/not-generation.svg: supersampled residual 297 px is above the default "
                "allowance, so the difference is not only the rasterizer generation",
                "text/text/shared-golden.svg: combines the budget with shared_golden",
                "text/text/simple-text.svg: combines the budget with simple_text_threshold",
                "text/text/too-large.svg: budget 415 exceeds the analytic residual 398 plus 3% "
                "(410)",
                "text/text/unbudgeted.svg: compared without a pixel budget",
                "text/text/widened.svg: compared without a pixel budget",
                "text/text/widened.svg: widens the color threshold instead of counting pixels",
                "text/tspan/category.svg: comes from a category default, not a per-case override",
            ],
        )
        self.assertEqual(report["summary"]["reference_label_glyph_budget_cases"], 2)

    def test_live_reference_budgets_follow_the_evidence_rule(self) -> None:
        report = parity_report.generate_report(parity_report._default_repo_root())

        violations = parity_report.reference_budget_violations(report)

        self.assertEqual(
            violations,
            [],
            "A reference rasterizer generation or label glyph budget is a per-case pixel count at "
            "the default color threshold, within 3% of the analytic residual its reason states, "
            "with the supersampled residual stated too:\n" + "\n".join(violations),
        )

    def test_state_fields_cannot_bypass_the_named_factories(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        for params in ("Params{.renderOnly = true}", "Params{.skip = true}",
                       "Params{.renderOnlyBucket = \"undefined behavior\"}",
                       "Params{.skipSimpleText = true}"):
            with self.subTest(params=params):
                with self.assertRaisesRegex(parity_report.ParityReportError,
                                            "through Params::Skip, Params::UndefinedBehavior"):
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

    def test_unknown_params_calls_cannot_bypass_the_named_factories(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        registrations = {
            "unknown Params factory": '\"painting/fill\", {{\"a.svg\", Params::NoCompare(\"x\")}}',
            "helper named like Params": '\"painting/fill\", {{\"a.svg\", ParamsUb(\"x\")}}',
            "unknown free helper": '\"painting/fill\", {{\"a.svg\", RenderWithoutCompare(\"x\")}}',
            "unknown chained call": '\"painting/fill\", {{\"a.svg\", Params().noCompare()}}',
            "unknown category default": '\"painting/fill\", {}, Params::NoCompare(\"x\")',
        }
        for label, category_call in registrations.items():
            with self.subTest(label):
                with self.assertRaisesRegex(parity_report.ParityReportError,
                                            "unrecognized .*params"):
                    parity_report.build_report(_registration("PaintingFill", category_call),
                                               self.fixture.tests_root)

    def test_known_params_calls_are_accepted(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        chained = (
            "Params::WithThreshold(0.1f, 10, \"edge AA\")"
            ".withGeodeGoldenOverride(\"g.png\", \"geode\")"
            ".requireFeature(RendererBackendFeature::Text, \"text\")"
            ".disableBackend(RendererBackend::Geode, \"no\")"
            ".setCanvasSize(10, 10).onlyTextFull().withSimpleTextMaxPixels(3)"
            ".withMaxPixelsDifferent(4).withGeodeMaxPixelsDifferent(5)"
            ".includeAntiAliasingDifferences().enableGoldenUpdateFromEnv()"
        )
        for params in (chained, "WithMaxPixels(10, \"AA\")", "Params{}", "Params()",
                       "Params::WithGoldenOverride(\"g.png\", 0.1f, \"why\")", "{}"):
            with self.subTest(params=params):
                report = parity_report.build_report(
                    _registration("PaintingFill",
                                  f'\"painting/fill\", {{{{\"a.svg\", {params}}}}}'),
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
        kinds = Counter(case["render_only_kind"] for case in cases
                        if case["primary_state"] == "render-only")
        self.assertEqual(summary["render_only_undefined_behavior_cases"],
                         kinds["undefined-behavior"])
        self.assertEqual(summary["render_only_deprecated_in_svg2_cases"],
                         kinds["deprecated-in-svg2"])

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

    def test_live_render_only_rows_use_a_named_bucket(self) -> None:
        report = parity_report.generate_report(parity_report._default_repo_root())

        violations = parity_report.render_only_policy_violations(report)

        self.assertEqual(
            violations,
            [],
            "A resvg case that renders without a pixel comparison must use "
            "Params::UndefinedBehavior or Params::DeprecatedInSVG2 with a reason; use "
            "Params::Skip with a bug number or reference reason otherwise:\n"
            + "\n".join(violations),
        )

    def test_live_undefined_behavior_references_are_the_ub_marker_or_spec_ub(self) -> None:
        repo_root = parity_report._default_repo_root()
        report = parity_report.generate_report(repo_root)

        violations = parity_report.undefined_behavior_reference_violations(
            report,
            repo_root / parity_report.TESTS_ROOT,
            repo_root / parity_report.UB_MARKER_REFERENCE,
        )

        self.assertEqual(
            violations,
            [],
            "Params::UndefinedBehavior cases need the resvg UB marker reference "
            f"({parity_report.UB_MARKER_REFERENCE}) or an entry in SPEC_UB_ALLOWLIST explaining "
            "the specification-level UB:\n" + "\n".join(violations),
        )

    def test_live_deprecated_in_svg2_cases_name_a_removed_feature(self) -> None:
        report = parity_report.generate_report(parity_report._default_repo_root())

        violations = parity_report.deprecated_in_svg2_violations(report)

        self.assertEqual(
            violations,
            [],
            "Params::DeprecatedInSVG2 is limited to the SVG 2 removals and deprecations listed in "
            "DEPRECATED_IN_SVG2_FEATURES:\n" + "\n".join(violations),
        )


if __name__ == "__main__":
    unittest.main()
