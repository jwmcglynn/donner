#!/usr/bin/env python3
"""Verify registration parsing and corpus accounting without rendering SVGs.

Live inventory totals are report metrics; controlled fixtures test their meaning.
"""

from __future__ import annotations

from collections import Counter

import base64
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import tempfile
import textwrap
import unittest
from urllib.parse import unquote, unquote_to_bytes, urlsplit
from unittest.mock import patch
from xml.parsers import expat

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


def _png_header(width: int, height: int) -> bytes:
    """Return the PNG signature and IHDR header, which is all the canvas rule reads."""

    return (b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR" + struct.pack(">II", width, height)
            + b"\x08\x06\x00\x00\x00")


class Fixture:
    def __init__(self) -> None:
        self._temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary_directory.name)
        self.tests_root = self.root / parity_report.TESTS_ROOT
        # The text corpus partition: tests/text/ only.
        self.tests_root.mkdir(parents=True)
        (self.tests_root.parent / "BUILD.bazel").write_text(
            "_TEXT_CASES_OUTSIDE_TEXT_DIRECTORY = [\n]\n", encoding="utf-8")

    def add_case(self, relative: str, reference: bytes | None = _png_header(500, 500)) -> None:
        path = self.tests_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("<svg/>\n", encoding="utf-8")
        if reference is not None:
            path.with_suffix(".png").write_bytes(reference)

    def close(self) -> None:
        self._temporary_directory.cleanup()


# The compile-time form the text capabilities used before the text macros became private.
_MACRO_GATED_TEXT = (
    "#ifdef DONNER_TEXT_ENABLED\n    case RendererBackendFeature::Text: return true;\n#else\n"
    "    case RendererBackendFeature::Text: return false;\n#endif"
)


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

    def test_marker_classification_maps_reject_stale_and_conflicting_entries(self) -> None:
        marker = b"ub marker bytes"
        self.fixture.add_case("painting/fill/marker.svg", marker)
        self.fixture.add_case("painting/fill/real.svg", b"a real rendering")
        self.fixture.add_case("painting/fill/compared.svg", marker)
        self.fixture.add_case("structure/svg/other.svg", marker)
        marker_path = self.fixture.root / "ub.png"
        marker_path.write_bytes(marker)
        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '\"painting/fill\", {'
                '{\"marker.svg\", Params::UndefinedBehavior(\"marker\")},'
                '{\"real.svg\", Params::UndefinedBehavior(\"spec UB\")}'
                '}',
            ) + _registration(
                "StructureSvg",
                '\"structure/svg\", {{\"other.svg\", Params::UndefinedBehavior(\"marker\")}}',
            ),
            self.fixture.tests_root,
        )

        violations = parity_report.marker_case_violations(
            report,
            self.fixture.tests_root,
            marker_path,
            {
                "painting/fill/marker.svg": ("known-renderer-divergence", "spec"),
                "painting/fill/real.svg": ("defined-input-no-reference", "spec"),
                "painting/fill/compared.svg": ("defined-input-no-reference", "spec"),
                "structure/svg/other.svg": ("known-renderer-divergence", "spec"),
            },
            {"painting/fill/marker.svg": "spec"},
        )

        self.assertEqual(
            violations,
            [
                "painting/fill/compared.svg: listed, but not an active undefined-behavior case",
                "painting/fill/marker.svg: listed as both defined and undefined",
                "painting/fill/real.svg: listed, but its reference is not the UB marker",
                "structure/svg/other.svg: known renderer divergence without a spec audit ledger",
            ],
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

    def test_golden_overrides_need_a_reason(self) -> None:
        for name in ("bare.svg", "geode.svg", "reasoned.svg", "builder.svg"):
            self.fixture.add_case(f"painting/fill/{name}")
        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '"painting/fill", {'
                '{"bare.svg", Params::WithGoldenOverride("a.png")},'
                '{"geode.svg", Params().withGeodeGoldenOverride("g.png")},'
                '{"reasoned.svg", Params::WithGoldenOverride("a.png", 0.02f, "why")},'
                '{"builder.svg", Params().withGeodeGoldenOverride("g.png", "why")}'
                '}',
            ),
            self.fixture.tests_root,
        )

        self.assertEqual(
            parity_report.golden_reason_violations(report),
            [
                "painting/fill/bare.svg: golden override has no reason; record why the vendored "
                "reference is replaced",
                "painting/fill/geode.svg: golden override has no reason; record why the vendored "
                "reference is replaced",
            ],
        )

    def test_explicit_capability_gate_needs_a_source_reason(self) -> None:
        # The arabic row once had a hard-coded exemption; it must fail like any other row.
        for category, filename in (("text/sample", "gated.svg"),
                                   ("text/text-anchor", "on-tspan-with-arabic.svg")):
            fixture = Fixture()
            self.addCleanup(fixture.close)
            fixture.add_case(f"{category}/{filename}")
            report = parity_report.build_report(
                _registration(
                    "TextGated",
                    f'"{category}", {{'
                    f'{{"{filename}", Params().requireFeature(RendererBackendFeature::TextFull)}}}}',
                ),
                fixture.tests_root,
            )

            with self.subTest(path=f"{category}/{filename}"), self.assertRaisesRegex(
                parity_report.ParityReportError,
                f"{category}/{filename}: explicit capability gate needs a source reason",
            ):
                parity_report.add_exception_policies(report, {"rules": []})

    def _ratchet_report(self) -> dict[str, object]:
        for name in ("new.svg", "grown.svg", "shrunk.svg", "kept.svg", "fixed.svg", "geode.svg"):
            self.fixture.add_case(f"painting/fill/{name}")
        return parity_report.build_report(
            _registration(
                "PaintingFill",
                '"painting/fill", {'
                '{"new.svg", Params::WithThreshold(0.08f, 150, "new")},'
                '{"grown.svg", Params::WithThreshold(0.2f, 300, "grown")},'
                '{"shrunk.svg", Params::WithThreshold(0.06f, 120, "shrunk")},'
                '{"kept.svg", Params::WithThreshold(0.1f, 200, "kept")},'
                '{"fixed.svg", Params::WithThreshold(0.05f, 100, "fixed")},'
                '{"geode.svg", Params().withGeodeMaxPixelsDifferent(500).withReason("geode")}'
                '}',
            ),
            self.fixture.tests_root,
        )

    def test_pixel_budget_ratchet_rejects_new_grown_shrunk_and_stale_entries(self) -> None:
        report = self._ratchet_report()
        every_lane = ("default_text", "max", "geode")
        allowlist = {
            "painting/fill/grown.svg": dict.fromkeys(every_lane, 250),
            "painting/fill/shrunk.svg": dict.fromkeys(every_lane, 150),
            "painting/fill/kept.svg": dict.fromkeys(every_lane, 200),
            "painting/fill/fixed.svg": {"max": 400},
            "painting/fill/geode.svg": {"geode": 500, "max": 500},
        }

        violations = parity_report.pixel_budget_ratchet_violations(report, allowlist)

        def lanes(path: str, message: str) -> list[str]:
            return [f"painting/fill/{path} ({lane}): {message}" for lane in every_lane]

        self.assertEqual(violations, sorted([
            *lanes("new.svg", "max_pixels 150 is above 100 and not in PIXEL_BUDGET_ALLOWLIST; new "
                   "entries need maintainer approval, so fix the engine instead"),
            *lanes("grown.svg", "max_pixels 300 is above its recorded 250 in "
                   "PIXEL_BUDGET_ALLOWLIST, which may only shrink"),
            *lanes("shrunk.svg", "max_pixels 120 is below its recorded 150; lower the "
                   "PIXEL_BUDGET_ALLOWLIST entry"),
            "painting/fill/fixed.svg (max): stale PIXEL_BUDGET_ALLOWLIST entry; the lane no "
            "longer compares with max_pixels above 100, so remove it",
            "painting/fill/geode.svg (max): stale PIXEL_BUDGET_ALLOWLIST entry; the lane no "
            "longer compares with max_pixels above 100, so remove it",
        ]))
        self.assertEqual(parity_report.pixel_budget_ratchet_violations(report, {
            "painting/fill/new.svg": dict.fromkeys(every_lane, 150),
            "painting/fill/grown.svg": dict.fromkeys(every_lane, 300),
            "painting/fill/shrunk.svg": dict.fromkeys(every_lane, 120),
            "painting/fill/kept.svg": dict.fromkeys(every_lane, 200),
            "painting/fill/geode.svg": {"geode": 500},
        }), [])

    def test_pixel_budget_ratchet_ignores_lanes_that_do_not_compare(self) -> None:
        self.fixture.add_case("text/sample/full.svg")
        report = parity_report.build_report(
            _registration("TextSample", '"text/sample", {'
                          '{"full.svg", WithMaxPixels(300, "text-full budget").onlyTextFull()}}'),
            self.fixture.tests_root,
        )

        self.assertEqual(
            parity_report.pixel_budget_ratchet_violations(
                report, {"text/sample/full.svg": {"max": 300, "geode": 300, "geode_max_text": 300}}
            ),
            ["text/sample/full.svg (geode): stale PIXEL_BUDGET_ALLOWLIST entry; the lane no "
             "longer compares with max_pixels above 100, so remove it"],
        )

    def test_threshold_ratchet_rejects_new_grown_shrunk_and_stale_entries(self) -> None:
        report = self._ratchet_report()
        every_lane = ("default_text", "max", "geode")
        allowlist = {
            "painting/fill/grown.svg": dict.fromkeys(every_lane, 0.15),
            "painting/fill/shrunk.svg": dict.fromkeys(every_lane, 0.07),
            "painting/fill/kept.svg": dict.fromkeys(every_lane, 0.1),
            "painting/fill/fixed.svg": dict.fromkeys(every_lane, 0.05),
        }

        violations = parity_report.threshold_ratchet_violations(report, allowlist)

        self.assertEqual(len(violations), 12, violations)
        self.assertIn("painting/fill/new.svg (geode): threshold 0.08 is above 0.05 and not in "
                      "THRESHOLD_ALLOWLIST; new entries need maintainer approval, so fix the engine "
                      "instead", violations)
        self.assertIn("painting/fill/grown.svg (max): threshold 0.2 is above its recorded 0.15 in "
                      "THRESHOLD_ALLOWLIST, which may only shrink", violations)
        self.assertIn("painting/fill/shrunk.svg (default_text): threshold 0.06 is below its "
                      "recorded 0.07; lower the THRESHOLD_ALLOWLIST entry", violations)
        self.assertIn("painting/fill/fixed.svg (max): stale THRESHOLD_ALLOWLIST entry; the lane no "
                      "longer compares with threshold above 0.05, so remove it", violations)
        self.assertFalse([v for v in violations if v.startswith("painting/fill/kept.svg")])

    def test_skip_ratchet_rejects_new_and_stale_skips(self) -> None:
        for name in ("listed.svg", "new.svg", "fixed.svg"):
            self.fixture.add_case(f"painting/fill/{name}")
        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '"painting/fill", {'
                '{"listed.svg", Params::Skip("Not impl: feature (#1)")},'
                '{"new.svg", Params::Skip("Reference disagreement: the PNG is wrong")}'
                '}',
            ),
            self.fixture.tests_root,
        )

        violations = parity_report.skip_ratchet_violations(
            report, {"painting/fill/listed.svg", "painting/fill/fixed.svg"}
        )

        self.assertEqual(
            violations,
            [
                "painting/fill/new.svg: Params::Skip is not in SKIP_ALLOWLIST; "
                + parity_report.SKIP_RULE,
                "painting/fill/fixed.svg: stale SKIP_ALLOWLIST entry; the case is no longer an "
                "active skip, so remove it",
            ],
        )
        self.assertIn("Donner-rendered golden", parity_report.SKIP_RULE)
        self.assertIn("README_resvg_test_suite.md", parity_report.SKIP_RULE)

    def _edge_coverage_report(self) -> dict[str, object]:
        for name in ("listed.svg", "new.svg", "golden.svg", "shared.svg", "quantized.svg", "aa.svg",
                     "cpu.svg", "plain.svg"):
            self.fixture.add_case(f"painting/fill/{name}")
        return parity_report.build_report(
            _registration(
                "PaintingFill",
                '"painting/fill", {'
                '{"listed.svg", Params().withGeodeQuarterPixelEdgeCoverage()},'
                '{"new.svg", Params().withGeodeQuarterPixelEdgeCoverage()},'
                '{"golden.svg", Params().withGeodeGoldenOverride("gpu.png", "Geode golden")'
                '.withGeodeQuarterPixelEdgeCoverage()},'
                '{"shared.svg", Params::WithGoldenOverride("cpu.png", 0.02f, "Donner golden")'
                '.withGeodeQuarterPixelEdgeCoverage()},'
                '{"quantized.svg", Params::WithGoldenOverride("quantized.png", 0.02f, "Donner")'
                '.withGeodeQuarterPixelEdgeCoverage()},'
                '{"aa.svg", Params().includeAntiAliasingDifferences()'
                '.withGeodeQuarterPixelEdgeCoverage()},'
                '{"cpu.svg", Params().disableBackend(RendererBackend::Geode, "no Geode")'
                '.withGeodeQuarterPixelEdgeCoverage()}'
                '}',
            ),
            self.fixture.tests_root,
        )

    def test_quarter_pixel_edge_coverage_applies_to_the_geode_lane_only(self) -> None:
        report = self._edge_coverage_report()
        by_path = {case["path"]: case for case in report["cases"]}

        listed = by_path["painting/fill/listed.svg"]
        self.assertEqual(
            {lane: entry["settings"]["quarter_pixel_edge_coverage"]
             for lane, entry in listed["lanes"].items()},
            {"default_text": False, "max": False, "geode": True},
        )
        self.assertIn("geode_edge_coverage", listed["exception_types"])
        # The fixture applies the rule against a shared golden, never against a Geode golden;
        # quarter_pixel_edge_violations limits shared goldens to quantized ones.
        self.assertFalse(by_path["painting/fill/golden.svg"]["lanes"]["geode"]["settings"][
            "quarter_pixel_edge_coverage"])
        self.assertTrue(by_path["painting/fill/shared.svg"]["lanes"]["geode"]["settings"][
            "quarter_pixel_edge_coverage"])
        plain = by_path["painting/fill/plain.svg"]
        self.assertEqual(plain["exception_types"], [])
        self.assertFalse(plain["lanes"]["geode"]["settings"]["quarter_pixel_edge_coverage"])
        self.assertEqual(report["summary"]["geode_quarter_pixel_edge_cases"], 7)
        self.assertIn("geode_edge_coverage", parity_report._LOOSENING_EXCEPTIONS)

    def test_quarter_pixel_edge_allowlist_rejects_unlisted_inert_and_stale_rows(self) -> None:
        report = self._edge_coverage_report()

        violations = parity_report.quarter_pixel_edge_violations(report, {"quantized.png"}, {
            "painting/fill/listed.svg", "painting/fill/golden.svg", "painting/fill/shared.svg",
            "painting/fill/quantized.svg", "painting/fill/aa.svg", "painting/fill/cpu.svg",
            "painting/fill/fixed.svg",
        })

        self.assertEqual(violations, sorted([
            "painting/fill/new.svg: not in QUARTER_PIXEL_EDGE_ALLOWLIST; "
            + parity_report.QUARTER_PIXEL_EDGE_RULE,
            "painting/fill/golden.svg: the Geode lane compares against a Geode golden, which is "
            "not quarter-pixel quantized; remove the quarter-pixel edge coverage rule",
            "painting/fill/shared.svg: the Geode lane compares against cpu.png, which the "
            "'## Quarter-pixel quantized goldens' table in "
            "donner/svg/renderer/testdata/golden/README.md does not record as a quarter-pixel "
            "quantized Donner software rendering; remove the quarter-pixel edge coverage rule",
            "painting/fill/aa.svg: the quarter-pixel edge coverage rule needs anti-aliasing "
            "detection, which includeAntiAliasingDifferences() turns off",
            "painting/fill/cpu.svg: no Geode lane compares, so the quarter-pixel edge coverage "
            "rule has no effect; remove it",
            "painting/fill/fixed.svg: stale QUARTER_PIXEL_EDGE_ALLOWLIST entry; the row no longer "
            "opts into the quarter-pixel edge coverage rule, so remove it",
        ]))
        self.assertIn("README_resvg_test_suite.md", parity_report.QUARTER_PIXEL_EDGE_RULE)

    def test_quantized_goldens_come_from_the_golden_readme_table(self) -> None:
        readme = "\n".join([
            "## Earlier section",
            "| `firefox-elsewhere.png` | Firefox 153 | x |",
            parity_report.QUANTIZED_GOLDEN_HEADING,
            "",
            "| Golden | Renderer | Measured |",
            "| --- | --- | --- |",
            "| `donner-a.png` | Donner software renderer | 0.55 |",
            "| `sub/donner-b.png` | Donner software renderer | 0.66 |",
            "",
            "## Later section",
            "| `donner-c.png` | Donner software renderer | x |",
        ])
        self.assertEqual(parity_report.quantized_goldens_from_readme(readme), {
            "donner/svg/renderer/testdata/golden/donner-a.png",
            "donner/svg/renderer/testdata/golden/sub/donner-b.png",
        })
        with self.assertRaisesRegex(parity_report.ParityReportError, "only 'Donner software"):
            parity_report.quantized_goldens_from_readme(
                readme.replace("`sub/donner-b.png` | Donner software renderer",
                               "`sub/donner-b.png` | Firefox 153"))
        with self.assertRaisesRegex(parity_report.ParityReportError, "needs one"):
            parity_report.quantized_goldens_from_readme(
                readme.replace(parity_report.QUANTIZED_GOLDEN_HEADING, "## Renamed"))

    def test_quarter_pixel_edge_rows_carry_a_named_policy(self) -> None:
        self.fixture.add_case("painting/fill/listed.svg")
        report = parity_report.build_report(
            _registration("PaintingFill", '"painting/fill", {'
                          '{"listed.svg", Params().withGeodeQuarterPixelEdgeCoverage()}}'),
            self.fixture.tests_root,
        )

        parity_report.add_exception_policies(report, {"rules": []})

        self.assertEqual([policy["classification"] for policy in report["cases"][0]["policies"]],
                         ["reference-edge-quantization"])

    def test_geode_gate_allowlist_rejects_unlisted_cpu_unexplained_and_stale_gates(self) -> None:
        for name in ("listed.svg", "new.svg", "cpu.svg", "silent.svg"):
            self.fixture.add_case(f"painting/fill/{name}")
        report = parity_report.build_report(
            _registration(
                "PaintingFill",
                '"painting/fill", {'
                '{"listed.svg", Params().disableBackend(RendererBackend::Geode, "Bug: listed")},'
                '{"new.svg", Params().disableBackend(RendererBackend::Geode, "Bug: new")},'
                '{"cpu.svg", Params().disableBackend(RendererBackend::TinySkia, "no CPU")},'
                '{"silent.svg", Params().disableBackend(RendererBackend::Geode)}'
                '}',
            ),
            self.fixture.tests_root,
        )

        both = parity_report.BOTH_TIERS
        violations = parity_report.geode_gate_violations(report, {
            "painting/fill/listed.svg": both, "painting/fill/cpu.svg": both,
            "painting/fill/silent.svg": both, "painting/fill/fixed.svg": both,
        })

        self.assertEqual(violations, sorted([
            "painting/fill/new.svg: not in GEODE_GATE_ALLOWLIST; " + parity_report.GEODE_GATE_RULE,
            "painting/fill/cpu.svg: disables ['tiny-skia']; " + parity_report.GEODE_GATE_RULE,
            "painting/fill/silent.svg: disables the Geode comparison without a reason",
            "painting/fill/fixed.svg: stale GEODE_GATE_ALLOWLIST entry; the row no longer disables "
            "its Geode comparison, so remove it",
        ]))

    def test_gate_helper_around_another_helper_expands_both(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source = textwrap.dedent(
            """
            ImageComparisonParams Golden(std::string_view path) {
              return Params::WithGoldenOverride(path, kDefaultThreshold, "Reference disagreement");
            }
            ImageComparisonParams Gate(ImageComparisonParams params) {
              return params.disableBackend(RendererBackend::Geode, "Bug: gated");
            }
            """
        ) + _registration("PaintingFill", '"painting/fill", {{"a.svg", Gate(Golden("g.png"))}}')

        case = parity_report.build_report(source, self.fixture.tests_root)["cases"][0]

        self.assertEqual(case["disabled_backends"], ["geode"])
        self.assertIn("shared_golden", case["exception_types"])
        self.assertEqual(parity_report._geode_disable_reason(case["raw_params_expression"]),
                         "Bug: gated")

    def test_goldens_must_be_listed_in_the_digest_table_with_their_digest(self) -> None:
        golden_root = self.fixture.root / parity_report.GOLDEN_ROOT
        (golden_root / "geode").mkdir(parents=True)
        files = {"listed.png": b"listed", "geode/listed.png": b"geode listed",
                 "elsewhere.png": b"elsewhere", "stale.png": b"regenerated",
                 "unlisted.png": b"unlisted"}
        for name, data in files.items():
            (golden_root / name).write_bytes(data)
        (self.fixture.root / parity_report.GOLDEN_ROOT.parent / "escape.png").write_bytes(b"x")

        def digest(name: str) -> str:
            return hashlib.sha256(files[name]).hexdigest()

        (golden_root / "README.md").write_text(
            "Prose naming `unlisted.png` with " + digest("unlisted.png") + " is not a table row.\n\n"
            "## Another section\n\n| Golden | SHA-256 |\n| --- | --- |\n"
            f"| `elsewhere.png` | `{digest('elsewhere.png')}` |\n"
            f"| `stale.png` | `{hashlib.sha256(b'original').hexdigest()}` |\n\n"
            f"{parity_report.GOLDEN_DIGEST_HEADING}\n\n| Golden | SHA-256 |\n| --- | --- |\n"
            f"| `listed.png` | `{digest('listed.png')}` |\n"
            f"| `geode/listed.png` | `{digest('geode/listed.png')}` |\n"
            f"| `stale.png` | `{digest('stale.png')}` |\n\n"
            "## A later section\n\n"
            f"| `unlisted.png` | `{digest('unlisted.png')}` |\n",
            encoding="utf-8",
        )
        names = ("a.svg", "b.svg", "c.svg", "d.svg", "e.svg", "f.svg")
        for name in names:
            self.fixture.add_case(f"painting/fill/{name}")
        prefix = parity_report.GOLDEN_ROOT.as_posix()
        goldens = ("listed.png", "stale.png", "unlisted.png", "missing.png", "elsewhere.png",
                   "../escape.png")
        overrides = "".join(
            f'{{"{name}", Params::WithGoldenOverride("{prefix}/{golden}", 0.02f, "r")'
            + ('.withGeodeGoldenOverride("' + prefix + '/geode/listed.png")' if name == "a.svg"
               else "")
            + "},"
            for name, golden in zip(names, goldens)
        )
        report = parity_report.build_report(
            _registration("PaintingFill", '"painting/fill", {' + overrides + "}"),
            self.fixture.tests_root,
        )
        readme = parity_report.GOLDEN_README
        section = repr(parity_report.GOLDEN_DIGEST_HEADING)

        self.assertEqual(
            parity_report.golden_documentation_violations(report, self.fixture.root),
            [
                f"{prefix}/../escape.png: golden path is not a normalized repository path",
                f"{prefix}/elsewhere.png: no row in the {readme} {section} table names it with "
                "its SHA-256 " + digest("elsewhere.png"),
                f"{prefix}/missing.png: referenced golden file is missing",
                f"{prefix}/stale.png: a {readme} row names it without its current SHA-256 "
                + digest("stale.png"),
                f"{prefix}/unlisted.png: no row in the {readme} {section} table names it with "
                "its SHA-256 " + digest("unlisted.png"),
            ],
        )

    def test_golden_readme_needs_one_digest_section(self) -> None:
        golden_root = self.fixture.root / parity_report.GOLDEN_ROOT
        golden_root.mkdir(parents=True)
        (golden_root / "README.md").write_text("| `a.png` | `x` |\n", encoding="utf-8")
        self.fixture.add_case("painting/fill/a.svg")
        report = parity_report.build_report(
            _registration("PaintingFill", '"painting/fill"'), self.fixture.tests_root
        )

        with self.assertRaisesRegex(parity_report.ParityReportError, "needs one"):
            parity_report.golden_documentation_violations(report, self.fixture.root)

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

    def test_params_helpers_are_classified_by_the_expression_they_return(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        self.fixture.add_case("painting/fill/b.svg")
        helpers = textwrap.dedent(
            """
            ImageComparisonParams SharedGolden() {
              return Params::WithGoldenOverride("shared.png", kDefaultThreshold, "shared why");
            }
            ImageComparisonParams NamedGolden(std::string_view path) {
              return Params::WithGoldenOverride(path, kDefaultThreshold, "path is kept");
            }
            """
        )
        report = parity_report.build_report(
            helpers
            + _registration(
                "PaintingFill",
                '\"painting/fill\", {{\"a.svg\", SharedGolden()}, '
                '{\"b.svg\", NamedGolden(\"named.png\").setCanvasSize(10, 10)}}',
            ),
            self.fixture.tests_root,
        )

        cases = {case["path"]: case for case in report["cases"]}
        self.assertEqual(cases["painting/fill/a.svg"]["exception_types"], ["shared_golden"])
        self.assertEqual(cases["painting/fill/a.svg"]["reason"], "shared why")
        self.assertEqual(cases["painting/fill/b.svg"]["exception_types"], ["shared_golden"])
        self.assertEqual(cases["painting/fill/b.svg"]["reason"], "path is kept")
        self.assertIn('"named.png"', cases["painting/fill/b.svg"]["raw_params_expression"])

    def test_params_helper_arity_is_checked(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source = (
            "ImageComparisonParams Golden(std::string_view path) {\n"
            "  return Params::WithGoldenOverride(path);\n}\n"
            + _registration("PaintingFill", '\"painting/fill\", {{\"a.svg\", Golden()}}')
        )
        with self.assertRaisesRegex(parity_report.ParityReportError, "takes 1 arguments"):
            parity_report.build_report(source, self.fixture.tests_root)

    def test_free_helpers_are_not_expanded(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source = (
            "ImageComparisonParams WithMaxPixels(int pixels, std::string_view reason) {\n"
            "  return Params::WithGoldenOverride(\"g.png\").setCanvasSize(300, 300);\n}\n"
            + _registration("PaintingFill",
                            '\"painting/fill\", {{\"a.svg\", WithMaxPixels(120, \"AA\")}}')
        )
        case = parity_report.build_report(source, self.fixture.tests_root)["cases"][0]
        self.assertEqual(case["raw_params_expression"], 'WithMaxPixels(120, "AA")')
        self.assertNotIn("shared_golden", case["exception_types"])
        settings = case["lanes"]["max"]["settings"]
        self.assertEqual(settings["max_pixels"], 120)
        self.assertEqual(settings["canvas_size"], [500, 500])

    def test_category_default_params_helper_is_expanded(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        self.fixture.add_case("painting/fill/b.svg")
        source = (
            "ImageComparisonParams SharedGolden() {\n"
            '  return Params::WithGoldenOverride("shared.png", kDefaultThreshold, "why");\n}\n'
            + _registration("PaintingFill", '\"painting/fill\", {}, SharedGolden()')
        )
        report = parity_report.build_report(source, self.fixture.tests_root)
        self.assertEqual([case["exception_types"] for case in report["cases"]],
                         [["shared_golden"], ["shared_golden"]])

    def test_params_helper_calling_a_helper_is_rejected(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source = (
            "ImageComparisonParams Inner() {\n"
            '  return Params::WithGoldenOverride("g.png");\n}\n'
            "ImageComparisonParams Outer() {\n  return Inner().setCanvasSize(10, 10);\n}\n"
            + _registration("PaintingFill", '\"painting/fill\", {{\"a.svg\", Outer()}}')
        )
        with self.assertRaisesRegex(parity_report.ParityReportError, "calls params helper Inner"):
            parity_report.build_report(source, self.fixture.tests_root)

    def test_params_helper_defined_twice_is_rejected(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source = (
            "ImageComparisonParams Golden() {\n"
            '  return Params::WithGoldenOverride("a.png");\n}\n'
            "ImageComparisonParams Golden(std::string_view path) {\n"
            "  return Params::WithGoldenOverride(path);\n}\n"
            + _registration("PaintingFill", '\"painting/fill\", {{\"a.svg\", Golden()}}')
        )
        with self.assertRaisesRegex(parity_report.ParityReportError, "defined more than once"):
            parity_report.build_report(source, self.fixture.tests_root)

    def test_cli_resolves_explicit_repo_root_independent_of_cwd(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        source_path = self.fixture.root / parity_report.REGISTRATION_SOURCE
        source_path.parent.mkdir(parents=True, exist_ok=True)
        source_path.write_text(_registration("PaintingFill", '"painting/fill"'), encoding="utf-8")
        stdout = io.StringIO()

        result = parity_report.main(["--repo-root", str(self.fixture.root)], stdout=stdout)

        self.assertEqual(result, 0)
        self.assertEqual(json.loads(stdout.getvalue())["summary"]["total_cases"], 1)

    def test_effective_lanes_apply_backend_and_text_budget_precedence(self) -> None:
        self.fixture.add_case("painting/stroke/example.svg")
        self.fixture.add_case("text/sample/a.svg")
        source = _registration(
            "PaintingStroke", '"painting/stroke", {'
            '{"example.svg", Params::WithGoldenOverride("cpu.png", 0.1f)'
            '.withGeodeGoldenOverride("gpu.png").withMaxPixelsDifferent(20)'
            '.withSimpleTextMaxPixels(30).withGeodeMaxPixelsDifferent(80)'
            '.setCanvasSize(800, 600).includeAntiAliasingDifferences()}}'
        ) + _registration("TextSample", '"text/sample"')
        report = parity_report.build_report(source, self.fixture.tests_root)
        paint, text = report["cases"]
        expected = [("default_text", 30, "cpu.png"), ("max", 20, "cpu.png"),
                    ("geode", 80, "gpu.png")]
        for lane, budget, golden in expected:
            setting = paint["lanes"][lane]["settings"]
            self.assertEqual(setting["max_pixels"], budget)
            self.assertEqual(setting["reference"]["path"], golden)
            self.assertEqual(setting["threshold"], 0.1)
            self.assertEqual(setting["canvas_size"], [800, 600])
            self.assertTrue(setting["include_antialiasing"])
        self.assertEqual(text["lanes"]["geode"]["configured_state"], "compare")
        self.assertEqual(text["lanes"]["geode"]["missing_features"], [])
        self.assertEqual(text["lanes"]["geode"]["observed"], "unobserved")

    def test_geode_full_text_lane_runs_text_cases_with_full_shaping(self) -> None:
        for row in ("text/sample/plain.svg", "text/sample/arabic.svg", "painting/fill/a.svg",
                    "painting/fill/on-text.svg"):
            self.fixture.add_case(row)
        (self.fixture.tests_root.parent / "BUILD.bazel").write_text(
            '_TEXT_CASES_OUTSIDE_TEXT_DIRECTORY = [\n    "tests/painting/fill/on-text.svg",\n]\n',
            encoding="utf-8")
        source = _registration(
            "TextSample", '"text/sample", {{"arabic.svg", Params().onlyTextFull()'
            '.withReason("Arabic shaping")}}') + _registration("PaintingFill", '"painting/fill"')
        report = parity_report.build_report(source, self.fixture.tests_root)
        by_path = {case["path"]: case for case in report["cases"]}

        self.assertEqual(set(by_path["painting/fill/a.svg"]["lanes"]),
                         {"default_text", "max", "geode"})
        self.assertEqual(set(by_path["painting/fill/on-text.svg"]["lanes"]),
                         {"default_text", "max", "geode", "geode_max_text"})
        arabic = by_path["text/sample/arabic.svg"]["lanes"]
        self.assertEqual({lane: entry["configured_state"] for lane, entry in arabic.items()}, {
            "default_text": "capability-gated", "max": "compare", "geode": "capability-gated",
            "geode_max_text": "compare"})
        self.assertEqual(arabic["geode_max_text"]["text_configuration"], "full")
        self.assertEqual(
            parity_report._runtime_name(by_path["text/sample/arabic.svg"], "geode_max_text"),
            ("TextSample/ImageComparisonTestFixture", "ResvgTest/arabic_GeodeGolden"))
        self.assertEqual(
            parity_report._runtime_name(by_path["text/sample/arabic.svg"], "geode"),
            ("TextSample/ImageComparisonTestFixture", "ResvgTest/DISABLED_arabic_GeodeGolden"))
        self.assertEqual(
            {state: sum(counts.values())
             for state, counts in report["lane_summary"]["geode_max_text"].items()},
            {"compare": 3})
        self.assertEqual(report["source"]["normal_lanes"]["geode_max_text"]["corpus"], "text")

    def test_text_partition_fails_without_a_well_formed_list(self) -> None:
        self.fixture.add_case("text/sample/a.svg")
        build = self.fixture.tests_root.parent / "BUILD.bazel"
        unterminated = ('_TEXT_CASES_OUTSIDE_TEXT_DIRECTORY = [\n    "tests/painting/a.svg",\n'
                        '_OTHER = [\n    "tests/painting/b.svg",\n]\n')
        for contents, message in (("", "_TEXT_CASES_OUTSIDE"),
                                  (unterminated, "_TEXT_CASES_OUTSIDE"),
                                  (None, "cannot read the text corpus partition")):
            with self.subTest(contents=contents):
                if contents is None:
                    build.unlink()
                else:
                    build.write_text(contents, encoding="utf-8")
                with self.assertRaisesRegex(parity_report.ParityReportError, message):
                    parity_report.build_report(_registration("TextSample", '"text/sample"'),
                                               self.fixture.tests_root)

    def test_simple_tier_geode_gate_compares_on_the_full_tier(self) -> None:
        for name in ("simple.svg", "both.svg"):
            self.fixture.add_case(f"text/sample/{name}")
        report = parity_report.build_report(
            _registration("TextSample", '"text/sample", {'
                          '{"simple.svg", Params().disableGeodeForSimpleText("Bug: simple")},'
                          '{"both.svg", Params().disableBackend(RendererBackend::Geode, "Bug")}}'),
            self.fixture.tests_root,
        )
        states = {case["path"]: {lane: entry["configured_state"]
                                 for lane, entry in case["lanes"].items()}
                  for case in report["cases"]}
        self.assertEqual(states["text/sample/simple.svg"], {
            "default_text": "compare", "max": "compare", "geode": "backend-disabled",
            "geode_max_text": "compare"})
        self.assertEqual(states["text/sample/both.svg"]["geode_max_text"], "backend-disabled")
        self.assertEqual(report["summary"]["backend_disabled_cases"], 2)

        exact = {"text/sample/simple.svg": parity_report.SIMPLE_TIER,
                 "text/sample/both.svg": parity_report.BOTH_TIERS}
        self.assertEqual(parity_report.geode_gate_violations(report, exact), [])
        swapped = {"text/sample/simple.svg": parity_report.BOTH_TIERS,
                   "text/sample/both.svg": parity_report.SIMPLE_TIER}
        self.assertEqual(parity_report.geode_gate_violations(report, swapped), sorted([
            "text/sample/simple.svg: disables its Geode comparison in the simple text tier(s), but "
            "GEODE_GATE_ALLOWLIST records full, simple; " + parity_report.GEODE_GATE_RULE,
            "text/sample/both.svg: disables its Geode comparison in the full, simple text tier(s), "
            "but GEODE_GATE_ALLOWLIST records simple; " + parity_report.GEODE_GATE_RULE,
        ]))

    def test_simple_tier_gate_needs_a_full_tier_geode_comparison(self) -> None:
        self.fixture.add_case("painting/fill/simple.svg")
        self.fixture.add_case("text/sample/text-row.svg")
        report = parity_report.build_report(
            _registration("PaintingFill", '"painting/fill", {'
                          '{"simple.svg", Params().disableGeodeForSimpleText("Bug: simple")}}')
            + _registration("TextSample", '"text/sample", {'
                            '{"text-row.svg", Params().disableGeodeForSimpleText("Bug: simple")}}'),
            self.fixture.tests_root,
        )
        simple = parity_report.SIMPLE_TIER
        violations = parity_report.geode_gate_violations(
            report, {"painting/fill/simple.svg": simple, "text/sample/text-row.svg": simple})

        self.assertEqual(violations, [
            "painting/fill/simple.svg: disables its Geode comparison in the simple text tier "
            "only, but the Geode full-shaping lane does not compare it (not in the text corpus), "
            "so the row has no Geode comparison; disable it in both tiers"])

    def test_google_test_receipt_distinguishes_pass_failure_suppression_and_unobserved(self) -> None:
        rows = ("a.svg", "b.svg", "c.svg", "d.svg")
        for row in rows:
            self.fixture.add_case("text/sample/" + row)
        report = parity_report.build_report(
            _registration("TextSample", '"text/sample", {'
                          '{"c.svg", Params::Skip("unsupported")}}'), self.fixture.tests_root
        )
        receipt = self.fixture.root / "shard.xml"
        receipt.write_text(
            "<testsuites><testsuite>"
            '<testcase classname="TextSample/ImageComparisonTestFixture" '
            'name="ResvgTest/a" status="run" result="completed"/>'
            '<testcase classname="TextSample/ImageComparisonTestFixture" '
            'name="ResvgTest/b" status="run" result="completed"><failure/></testcase>'
            '<testcase classname="TextSample/ImageComparisonTestFixture" '
            'name="ResvgTest/DISABLED_c" status="notrun" result="suppressed"/>'
            "</testsuite></testsuites>", encoding="utf-8",
        )
        revision = "a" * 40
        report["source"]["revision"] = revision
        with self.assertRaisesRegex(parity_report.ParityReportError, "receipt revision"):
            parity_report.add_execution(report, "max", "b" * 40, receipt)
        parity_report.add_execution(report, "max", revision, receipt)
        self.assertEqual(
            [row["lanes"]["max"]["observed"] for row in report["cases"]],
            ["passed", "failed", "suppressed", "unobserved"],
        )
        self.assertNotIn("shard.xml", json.dumps(report["execution_receipts"]))
        self.assertEqual(report["execution_receipts"]["max"]["binding"], "caller-attested")
        self.assertEqual(report["execution_receipts"]["max"]["kind"], "google-test-status")

    def test_unknown_or_duplicate_executions_cannot_be_reported_as_passes(self) -> None:
        self.fixture.add_case("text/sample/a.svg")
        # The Geode lane renders simple text only, so a full-shaping requirement gates it.
        report = parity_report.build_report(
            _registration("TextSample", '"text/sample", {{"a.svg", Params().requireFeature('
                          'RendererBackendFeature::TextFull, "full text shaping")}}'),
            self.fixture.tests_root,
        )
        report["source"]["revision"] = "a" * 40
        receipt = self.fixture.root / "shard.xml"
        for rows, match in [
            ('<testcase classname="Foreign" name="Other" status="run" result="completed"/>',
             "unexpected runtime testcase"),
            ('<testcase classname="TextSample/ImageComparisonTestFixture" '
             'name="ResvgTest/a_GeodeGolden" status="run" result="completed"/>',
             "contradicts the static gate"),
            ('<testcase classname="TextSample/ImageComparisonTestFixture" '
             'name="ResvgTest/a_GeodeGolden" status="run" result="skipped"><skipped/></testcase>' * 2,
             "duplicate runtime testcase"),
        ]:
            receipt.write_text("<testsuites>" + rows + "</testsuites>", encoding="utf-8")
            with self.assertRaisesRegex(parity_report.ParityReportError, match):
                parity_report.add_execution(report, "geode", "a" * 40, receipt)

    def test_strict_exception_policy_rejects_unlisted_or_widened_settings(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        report = parity_report.build_report(
            _registration("PaintingFill", '"painting/fill", {'
                          '{"a.svg", Params::WithThreshold(0.3f, 100, "edge sweep")}}'),
            self.fixture.tests_root,
        )
        with self.assertRaisesRegex(parity_report.ParityReportError, "unexplained exceptions"):
            parity_report.add_exception_policies(report, {"rules": []})
        rule = {
            "id": "edge-sweep",
            "paths": ["painting/fill/a.svg"],
            "kinds": ["threshold"],
            "classification": "reference-rasterization",
            "disposition": "Retain limited coverage difference.",
            "evidence": ["public-source"],
            "removal_criteria": "Qualify a matched coverage reference.",
            "reference_provenance": "Adjacent original.",
            "max_threshold": 0.2,
            "max_pixels": 100,
        }
        with self.assertRaisesRegex(parity_report.ParityReportError, "exceeds the reviewed bounds"):
            parity_report.add_exception_policies(report, {"rules": [rule]})
        rule["max_threshold"] = float("nan")
        with self.assertRaisesRegex(parity_report.ParityReportError, "invalid comparison bounds"):
            parity_report.add_exception_policies(report, {"rules": [rule]})
        rule["max_threshold"] = 0.3
        parity_report.add_exception_policies(report, {"rules": [rule]})
        self.assertEqual(report["cases"][0]["policies"][0]["id"], "edge-sweep")

    def test_negative_tier_budget_uses_native_fallback(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        report = parity_report.build_report(
            _registration("PaintingFill", '"painting/fill", {'
                          '{"a.svg", Params().withMaxPixelsDifferent(120)'
                          '.withSimpleTextMaxPixels(-2).withGeodeMaxPixelsDifferent(-3)}}'),
            self.fixture.tests_root,
        )
        for mode in report["cases"][0]["lanes"].values():
            self.assertEqual(mode["settings"]["max_pixels"], 120)

    def test_scoped_policy_rejects_swapped_cpu_and_geode_references(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        report = parity_report.build_report(
            _registration("PaintingFill", '"painting/fill", {'
                          '{"a.svg", Params::WithGoldenOverride("geode.png")'
                          '.withGeodeGoldenOverride("cpu.png")}}'), self.fixture.tests_root,
        )
        rule = {
            "id": "outline",
            "paths": ["painting/fill/a.svg"],
            "kinds": ["shared_golden", "geode_golden"],
            "classification": "reference-disagreement",
            "disposition": "Preserve qualified backend coverage.",
            "evidence": ["public-source"],
            "removal_criteria": "Qualify one common comparison image.",
            "reference_provenance": "Independent geometry with per-backend coverage.",
            "references": {"shared_golden": "cpu.png", "geode_golden": "geode.png"},
            "max_threshold": 0.02,
            "max_pixels": 100,
        }
        with self.assertRaisesRegex(parity_report.ParityReportError, "unreviewed CPU or Geode"):
            parity_report.add_exception_policies(report, {"rules": [rule]})

    def test_xml_dtd_is_rejected_before_reading_receipt_status(self) -> None:
        receipt = self.fixture.root / "unsafe.xml"
        receipt.write_bytes(
            ('<?xml version="1.0" encoding="UTF-16"?>'
             '<!DOCTYPE testsuites [<!ENTITY completion "completed">]>'
             '<testsuites><testcase classname="TextSample/ImageComparisonTestFixture" '
             'name="ResvgTest/a" status="run" result="&completion;"/></testsuites>').encode("utf-16")
        )
        with self.assertRaisesRegex(parity_report.ParityReportError, "DTD and entity declarations"):
            parity_report._read_test_xml(receipt)

    def test_empty_xml_does_not_register_an_observation(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        report = parity_report.build_report(
            _registration("PaintingFill", '"painting/fill"'), self.fixture.tests_root
        )
        report["source"]["revision"] = "a" * 40
        receipt = self.fixture.root / "empty.xml"
        receipt.write_text("<testsuites/>", encoding="utf-8")
        with self.assertRaisesRegex(parity_report.ParityReportError, "zero runtime test records"):
            parity_report.add_execution(report, "max", "a" * 40, receipt)
        self.assertEqual(report["execution_receipts"], {})
        self.assertEqual(report["cases"][0]["lanes"]["max"]["observed"], "unobserved")

    def test_renderer_contract_detects_a_canvas_rule_change(self) -> None:
        root = parity_report._default_repo_root()
        dst = self.fixture.root / parity_report.RENDERER_TEST_ROOT
        dst.mkdir(parents=True)
        for name in (
            "BUILD.bazel", "ImageComparisonTestFixture.h", "ImageComparisonTestFixture.cc",
            "RendererTestBackendGeode.cc", "RendererTestBackendTinySkia.cc", "resvg_test_suite.cc",
        ):
            original = (root / parity_report.RENDERER_TEST_ROOT / name).read_text(encoding="utf-8")
            (dst / name).write_text(original, encoding="utf-8")
        parity_report.check_renderer_contract(self.fixture.root)
        edits = {
            "resvg_test_suite.cc": [
                ("test.params.setCanvasSize(500, 500);", "test.params.setCanvasSize(400, 400);",
                 "corpus canvas rule differs"),
                ("test.params.enableCanvasSizeFromReference();", "",
                 "corpus canvas rule differs"),
            ],
            "BUILD.bazel": [
                ('dep = ":resvg_test_suite_max_text_impl",\n    opens_gpu_device = True,',
                 'dep = ":resvg_test_suite_impl",\n    opens_gpu_device = True,',
                 "text corpus wrapper differs"),
            ],
            "ImageComparisonTestFixture.h": [
                ("allowGeodeSimpleText = false;\n    backendRequirementReason",
                 "allowGeodeSimpleText = false;\n    allowGeode = false;\n"
                 "    backendRequirementReason",
                 "simple-tier Geode gate differs"),
                ("return allowGeode && (allowGeodeSimpleText || fullText);",
                 "return allowGeodeSimpleText || fullText;", "simple-tier Geode gate differs"),
            ],
            "RendererTestBackendGeode.cc": [
                ("return IsTextBuiltIn();", "return true;", "Geode capability Text differs"),
                ("return IsFullTextBuiltIn();", "return IsTextBuiltIn();",
                 "Geode capability TextFull differs"),
                # The macro form no longer follows the build: the macros are private.
                ("    case RendererBackendFeature::Text: return IsTextBuiltIn();",
                 _MACRO_GATED_TEXT,
                 "Geode capability Text differs"),
                ("    case RendererBackendFeature::FilterEffects: return true;",
                 "    case RendererBackendFeature::FilterEffects: return false;",
                 "Geode capability FilterEffects differs"),
            ],
            "ImageComparisonTestFixture.cc": [
                ("mode == ComparisonMode::GeodeGolden &&\n",
                 "mode != ComparisonMode::TinyGolden &&\n",
                 "quarter-pixel edge coverage rule differs"),
                ("params.geodeOverrideGoldenFilename.empty();", "true;",
                 "quarter-pixel edge coverage rule differs"),
                ("params.geodeOverrideGoldenFilename.empty();",
                 "params.overrideGoldenFilename.empty() && "
                 "params.geodeOverrideGoldenFilename.empty();",
                 "quarter-pixel edge coverage rule differs"),
                (".acceptQuarterPixelEdgeCoverage = acceptsQuarterPixelEdgeCoverage(params, mode)}",
                 ".acceptQuarterPixelEdgeCoverage = true}",
                 "quarter-pixel edge coverage rule differs"),
                ("params.allowsBackend(backend, IsFullTextBuiltIn())",
                 "params.allowsBackend(backend, true)",
                 "simple-tier Geode gate differs"),
                ("static_cast<int>(std::max(width, height))",
                 "static_cast<int>(std::min(width, height))", "reference canvas rule differs"),
                ("kMaxDimension = 16384;", "kMaxDimension = 8192;",
                 "reference canvas rule differs"),
                ("CanvasSizeResult CanvasSizeFromReferenceImage(", "CanvasSizeResult Renamed(",
                 "reference canvas rule differs"),
                ("params, effectiveGoldenFilename, MissingGoldenCanvasFallback",
                 "params, goldenImageFilename, MissingGoldenCanvasFallback",
                 "reference canvas rule differs"),
                ("\\0\\0\\0\\x0dIHDR", "\\0\\0\\0\\x0cIHDR", "reference canvas rule differs"),
                ("width == 0 || height == 0 || ", "", "reference canvas rule differs"),
                ("constexpr size_t kHeaderSize = 24;", "constexpr size_t kHeaderSize = 16;",
                 "reference canvas rule differs"),
                ('if (getenv("UPDATE_GOLDEN_IMAGES_DIR") == nullptr) {', "if (false) {",
                 "reference canvas rule differs"),
            ],
            "RendererTestBackendTinySkia.cc": [
                ("return IsTextBuiltIn();", "return true;", "TinySkia capability Text differs"),
                ("return IsFullTextBuiltIn();", "return IsTextBuiltIn();",
                 "TinySkia capability TextFull differs"),
                ("    case RendererBackendFeature::Text: return IsTextBuiltIn();",
                 _MACRO_GATED_TEXT,
                 "TinySkia capability Text differs"),
            ],
        }
        for name, replacements in edits.items():
            source = dst / name
            original = source.read_text(encoding="utf-8")
            for before, after, message in replacements:
                with self.subTest(file=name, edit=before):
                    self.assertEqual(original.count(before), 1)
                    source.write_text(original.replace(before, after), encoding="utf-8")
                    with self.assertRaisesRegex(parity_report.ParityReportError, message):
                        parity_report.check_renderer_contract(self.fixture.root)
            source.write_text(original, encoding="utf-8")
        parity_report.check_renderer_contract(self.fixture.root)

    def test_reference_canvas_is_the_square_of_the_longer_side(self) -> None:
        cases = {
            "square": (_png_header(300, 300), [300, 300]),
            "landscape": (_png_header(500, 250), [500, 500]),
            "portrait": (_png_header(250, 400), [400, 400]),
            "largest": (_png_header(16384, 1), [16384, 16384]),
            "missing": (None, [None, None]),
            "empty": (b"", [None, None]),
            "truncated": (_png_header(300, 300)[:20], [None, None]),
            "not a PNG": (b"GIF89a" + bytes(30), [None, None]),
            "IHDR not first": (_png_header(300, 300).replace(b"IHDR", b"tEXt"), [None, None]),
            "wrong IHDR length": (
                _png_header(300, 300).replace(b"\x00\x00\x00\x0dIHDR", b"\x00\x00\x00\x0cIHDR"),
                [None, None]),
            "zero width": (_png_header(0, 300), [None, None]),
            "oversized": (_png_header(16385, 300), [None, None]),
        }
        for label, (data, expected) in cases.items():
            with self.subTest(label):
                path = self.fixture.root / (label + ".png")
                if data is not None:
                    path.write_bytes(data)
                self.assertEqual(parity_report.reference_canvas_size(path), expected)

    def test_lane_canvas_follows_the_lane_reference(self) -> None:
        self.fixture.add_case("painting/fill/explicit.svg", _png_header(300, 300))
        self.fixture.add_case("painting/fill/golden.svg", _png_header(300, 300))
        self.fixture.add_case("painting/fill/missing.svg", None)
        self.fixture.add_case("painting/fill/render-only.svg", _png_header(300, 300))
        self.fixture.add_case("painting/fill/upstream.svg", _png_header(300, 300))
        self.fixture.add_case("painting/fill/wide.svg", _png_header(500, 250))
        golden_root = self.fixture.root / parity_report.GOLDEN_ROOT
        golden_root.mkdir(parents=True)
        (golden_root / "cpu.png").write_bytes(_png_header(800, 800))
        (golden_root / "gpu.png").write_bytes(_png_header(400, 400))
        prefix = parity_report.GOLDEN_ROOT.as_posix()
        report = parity_report.build_report(
            _registration("PaintingFill", '"painting/fill", {'
                          '{"explicit.svg", Params().setCanvasSize(10, 20)},'
                          f'{{"golden.svg", Params::WithGoldenOverride("{prefix}/cpu.png")'
                          f'.withGeodeGoldenOverride("{prefix}/gpu.png", "why")}},'
                          '{"render-only.svg", Params::UndefinedBehavior("why")}}'),
            self.fixture.tests_root,
        )
        canvases = {
            case["path"].removeprefix("painting/fill/"):
                {lane: entry["settings"]["canvas_size"] for lane, entry in case["lanes"].items()}
            for case in report["cases"]
        }
        def same(size: list[int | None]) -> dict[str, list[int | None]]:
            return {"default_text": size, "max": size, "geode": size}

        self.assertEqual(canvases, {
            "explicit.svg": same([10, 20]),
            "golden.svg": {"default_text": [800, 800], "max": [800, 800], "geode": [400, 400]},
            "missing.svg": same([None, None]),
            "render-only.svg": same([500, 500]),
            "upstream.svg": same([300, 300]),
            "wide.svg": same([500, 500]),
        })
        self.assertEqual(parity_report.unresolved_canvas_violations(report), [
            f"painting/fill/missing.svg ({lane}): no canvas from reference "
            "third_party/resvg-test-suite/tests/painting/fill/missing.png"
            for lane in ("default_text", "geode", "max")
        ])
        report["source"]["revision"] = "a" * 40
        receipt = self.fixture.root / "shard.xml"
        receipt.write_text(
            '<testsuites><testcase classname="PaintingFill/ImageComparisonTestFixture" '
            'name="ResvgTest/missing" status="run" result="completed"/></testsuites>',
            encoding="utf-8",
        )
        with self.assertRaisesRegex(parity_report.ParityReportError, "unresolved comparison"):
            parity_report.add_execution(report, "max", "a" * 40, receipt)

    def test_upstream_release_manifest_rejects_drifted_entries(self) -> None:
        suite = self.fixture.root / "suite"
        (suite / "tests/a").mkdir(parents=True)
        (suite / "tests/a/kept.png").write_bytes(b"kept")
        (suite / "tests/a/changed.png").write_bytes(b"regenerated")
        def digest(data: bytes) -> str:
            return hashlib.sha256(data).hexdigest()

        good = f"{digest(b'kept')}  tests/a/kept.png"
        self.assertEqual(parity_report.upstream_release_manifest_violations(good + "\n", suite), [])

        manifest = "\n".join([
            good,
            f"{digest(b'original')}  tests/a/changed.png",
            f"{digest(b'gone')}  tests/a/missing.png",
            good,
            f"{digest(b'kept')}  tests/a/../a/kept.png",
            f"{digest(b'kept')} tests/a/kept.png",
            f"{digest(b'kept')}  resources/kept.png",
        ])
        self.assertEqual(parity_report.upstream_release_manifest_violations(manifest, suite), [
            f"tests/a/changed.png: SHA-256 is {digest(b'regenerated')}, not the recorded "
            + digest(b"original"),
            "tests/a/missing.png: listed file is missing",
            "tests/a/kept.png: listed more than once",
            "tests/a/../a/kept.png: not a normalized path under tests/",
            f"malformed line '{digest(b'kept')} tests/a/kept.png'",
            f"malformed line '{digest(b'kept')}  resources/kept.png'",
        ])
        self.assertEqual(parity_report.upstream_release_manifest_violations("", suite),
                         ["the manifest lists no files"])

    def test_upstream_release_notes_state_the_manifest_counts(self) -> None:
        suite = self.fixture.root / "suite"
        (suite / "tests/a").mkdir(parents=True)
        (suite / "tests/a/square.png").write_bytes(_png_header(300, 300))
        (suite / "tests/a/wide.png").write_bytes(_png_header(300, 150))
        (suite / "tests/a/case.svg").write_bytes(b"<svg/>")
        manifest = "\n".join(f"{'0' * 64}  tests/a/{name}"
                              for name in ("square.png", "wide.png", "case.svg"))
        notes = ("The 3 files listed in resvg-v0.50.0.sha256 (2 PNGs, each\n"
                 "300x300, and 1 SVGs) are byte-identical.")
        self.assertEqual(
            parity_report.upstream_release_note_violations(manifest, notes, suite),
            ["tests/a/wide.png: not the 300x300 PNG the notes state"])
        (suite / "tests/a/wide.png").write_bytes(_png_header(300, 300))
        self.assertEqual(parity_report.upstream_release_note_violations(manifest, notes, suite), [])
        self.assertEqual(
            parity_report.upstream_release_note_violations(
                manifest, notes.replace("2 PNGs", "3 PNGs"), suite),
            ["the notes state 3 files (3 PNGs, 1 SVGs); the manifest lists 3 (2 PNGs, 1 SVGs)"])
        self.assertEqual(
            parity_report.upstream_release_note_violations(manifest, "no counts", suite),
            ["the provenance notes need one sentence stating the manifest's file counts"])

    def test_untracked_report_inputs_keep_the_checkout_revision_unbound(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        root = self.fixture.root
        (root / ".git").mkdir()
        registry = root / parity_report.REGISTRATION_SOURCE
        registry.parent.mkdir(parents=True)
        registry.write_text(_registration("PaintingFill", '"painting/fill"'), encoding="utf-8")

        revision = "a" * 40
        for listing in ("", "third_party/resvg-test-suite/tests/painting/fill/a.svg\0"):
            stdout = io.StringIO()
            with patch.object(
                parity_report.subprocess, "check_output", side_effect=[revision, "", listing]
            ) as git:
                self.assertEqual(parity_report.main(["--repo-root", str(root)], stdout=stdout), 0)
            command = git.call_args_list[-1].args[0]
            self.assertIn("--others", command)
            self.assertNotIn("--exclude-standard", command)
            self.assertIn(f":(glob){parity_report.TESTS_ROOT.as_posix()}/**/*.svg", command)
            self.assertIn(parity_report.EXCEPTION_POLICIES.as_posix(), command)
            source = json.loads(stdout.getvalue())["source"]
            self.assertEqual(source["checkout_head"], revision)
            self.assertFalse(source["tracked_tree_dirty"])
            self.assertEqual(source["untracked_report_inputs"], bool(listing))
            self.assertEqual(source["revision"], None if listing else revision)

    def _head_to_head_fixture(self) -> dict[str, object]:
        for filename in ("both.svg", "budget.svg", "golden.svg", "gated.svg", "unknown.svg",
                         "skip.svg", "ub.svg", "neither.svg"):
            self.fixture.add_case("painting/fill/" + filename)
        self.fixture.add_case("painting/stroke/off.svg")
        source = _registration(
            "PaintingFill",
            '"painting/fill", {'
            '{"budget.svg", Params::WithThreshold(0.1f, 200, "edge coverage")},'
            '{"golden.svg", Params::WithGoldenOverride("g.png", 0.1f, "own golden")},'
            '{"gated.svg", Params().requireFeature(RendererBackendFeature::Text, "text")},'
            '{"skip.svg", Params::Skip("Bug: pending fix")},'
            '{"ub.svg", Params::UndefinedBehavior("UB: undefined")},'
            '{"neither.svg", Params::Skip("Not impl: feature")}'
            '}',
        ) + textwrap.dedent(
            """
            // INSTANTIATE_TEST_SUITE_P(
            //     PaintingStroke, ImageComparisonTestFixture,
            //     Combine(ValuesIn(getTestsInCategory("painting/stroke")),
            //             ValuesIn(ActiveComparisonModes())),
            //     TestNameFromFilename);
            """
        )
        return parity_report.build_report(source, self.fixture.tests_root)

    def test_head_to_head_counts_passes_and_lists_both_differences(self) -> None:
        report = self._head_to_head_fixture()
        results = parity_report.parse_engine_results(textwrap.dedent(
            """\
            title,chrome,resvg
            painting/fill/both.svg,2,1
            painting/fill/budget.svg,1,2
            painting/fill/golden.svg,1,1
            painting/fill/gated.svg,1,1
            painting/fill/unknown.svg,1,0
            painting/fill/skip.svg,2,1
            painting/fill/ub.svg,2,1
            painting/fill/neither.svg,1,3
            painting/stroke/off.svg,2,1
            """
        ))

        head_to_head = parity_report.build_head_to_head(report, results)

        # Budgets and golden overrides leave strict; a feature gate does not.
        self.assertEqual(head_to_head["summary"], {
            "both_pass": 3,
            "donner_only": 2,
            "donner_only_by_engine_result": {"failed": 1, "unknown": 1},
            "donner_pass": 5,
            "donner_pixel_pass": 5,
            "donner_render_pass": 0,
            "donner_strict_pass": 3,
            "engine_pass": 6,
            "gap": 3,
            "gap_by_state": {"disabled": 1, "skip": 1, "undefined-behavior": 1},
            "margin": -1,
            "total_cases": 9,
            "unscored_release_additions": 0,
        })
        self.assertEqual(head_to_head["donner_only"], [
            {"engine_result": "failed", "path": "painting/fill/budget.svg"},
            {"engine_result": "unknown", "path": "painting/fill/unknown.svg"},
        ])
        self.assertEqual(head_to_head["gap"], [
            {"path": "painting/fill/skip.svg", "reason": "Bug: pending fix", "state": "skip"},
            {"path": "painting/fill/ub.svg", "reason": "UB: undefined",
             "state": "undefined-behavior"},
            {"path": "painting/stroke/off.svg", "reason": None, "state": "disabled"},
        ])

    def test_render_credit_requires_an_active_reviewed_contract_and_backend(self) -> None:
        base = {"registration": "active", "primary_state": "render-only",
                "render_only_kind": "undefined-behavior", "comparison_modes": ["tiny-golden"],
                "exception_types": [], "reason": "upstream UB marker"}
        path = "structure/svg/funcIRI-with-invalid-characters.svg"
        other = "painting/fill/rgb-int-int-int.svg"
        results = {path: 0, other: 1}
        for overrides, expected in [
            ({}, 1), ({"registration": "disabled"}, 0), ({"comparison_modes": []}, 0),
            ({"render_only_kind": "deprecated-in-svg2"}, 0), ({"primary_state": "skip"}, 0),
        ]:
            with self.subTest(overrides=overrides):
                report = {"cases": [dict(base, path=path) | overrides,
                                    dict(base, path=other)]}
                score = parity_report.build_head_to_head(report, results)
                self.assertEqual(score["summary"]["donner_pass"], expected)
                self.assertEqual(score["summary"]["donner_strict_pass"], 0)
                self.assertEqual([row["path"] for row in score["gap"]], [other])

    def test_head_to_head_fails_closed_when_results_and_corpus_disagree(self) -> None:
        report = self._head_to_head_fixture()
        results = {"painting/fill/both.svg": 1, "painting/fill/renamed.svg": 1}

        with self.assertRaisesRegex(parity_report.ParityReportError,
                                    r"8 cases without a result .*1 results without a case"):
            parity_report.build_head_to_head(report, results)

    def test_head_to_head_leaves_release_additions_without_a_result_unscored(self) -> None:
        report = self._head_to_head_fixture()
        results = {path: 1 for path in (
            "painting/fill/both.svg", "painting/fill/budget.svg", "painting/fill/golden.svg",
            "painting/fill/gated.svg", "painting/fill/skip.svg", "painting/fill/ub.svg",
            "painting/fill/neither.svg")}
        additions = frozenset({"painting/fill/unknown.svg", "painting/stroke/off.svg",
                               "painting/fill/both.svg"})

        head_to_head = parity_report.build_head_to_head(report, results,
                                                        release_additions=additions)

        # A recorded result keeps a release-recorded case scored; the other two are left out of
        # both scores and listed.
        self.assertEqual(head_to_head["unscored_release_additions"], [
            {"path": "painting/fill/unknown.svg", "state": "compare"},
            {"path": "painting/stroke/off.svg", "state": "disabled"},
        ])
        self.assertEqual(
            {key: head_to_head["summary"][key] for key in (
                "donner_pixel_pass", "engine_pass", "total_cases", "unscored_release_additions")},
            {"donner_pixel_pass": 4, "engine_pass": 7, "total_cases": 9,
             "unscored_release_additions": 2},
        )
        with self.assertRaisesRegex(parity_report.ParityReportError,
                                    r"1 cases without a result \['painting/stroke/off.svg'\]"):
            parity_report.build_head_to_head(
                report, results, release_additions=frozenset({"painting/fill/unknown.svg"}))

    def test_release_additions_need_both_files_in_the_manifest(self) -> None:
        manifest = self.fixture.root / parity_report.UPSTREAM_RELEASE_MANIFEST
        manifest.parent.mkdir(parents=True, exist_ok=True)
        digest = "0" * 64
        manifest.write_text("".join(f"{digest}  tests/{path}\n" for path in (
            "a/b/both.png", "a/b/both.svg", "a/b/png-only.png", "a/b/svg-only.svg")),
            encoding="utf-8")

        self.assertEqual(parity_report.release_addition_cases(self.fixture.root),
                         frozenset({"a/b/both.svg"}))
        manifest.unlink()
        self.assertEqual(parity_report.release_addition_cases(self.fixture.root), frozenset())

    def test_engine_results_reject_malformed_rows(self) -> None:
        malformed = {
            "no engine column": "title,chrome\npainting/fill/a.svg,1\n",
            "no title column": "name,resvg\npainting/fill/a.svg,1\n",
            "unknown code": "title,resvg\npainting/fill/a.svg,4\n",
            "empty code": "title,resvg\npainting/fill/a.svg,\n",
            "duplicate row": "title,resvg\npainting/fill/a.svg,1\npainting/fill/a.svg,2\n",
            "non-SVG path": "title,resvg\npainting/fill/a.png,1\n",
            "non-ASCII digit": "title,resvg\npainting/fill/a.svg,\u0661\n",
            "superscript digit": "title,resvg\npainting/fill/a.svg,\u00b2\n",
            "leading zero": "title,resvg\npainting/fill/a.svg,01\n",
            "extra column": "title,resvg\npainting/fill/a.svg,1,9\n",
            "unterminated quote": 'title,resvg\n"painting/fill/a.svg,1\n',
            "oversized field": "title,resvg\n" + "x" * 200_000 + ".svg,1\n",
            "no rows": "title,resvg\n",
        }
        for label, text in malformed.items():
            with self.subTest(label), self.assertRaises(parity_report.ParityReportError):
                parity_report.parse_engine_results(text)
        with self.assertRaisesRegex(parity_report.ParityReportError, "malformed results.csv"):
            parity_report.parse_engine_results(malformed["oversized field"])

    def test_cli_head_to_head_reads_the_vendored_results(self) -> None:
        self.fixture.add_case("painting/fill/a.svg")
        self.fixture.add_case("painting/fill/b.svg")
        source_path = self.fixture.root / parity_report.REGISTRATION_SOURCE
        source_path.parent.mkdir(parents=True, exist_ok=True)
        source_path.write_text(
            _registration("PaintingFill",
                          '"painting/fill", {{"b.svg", Params::Skip("Bug: pending")}}'),
            encoding="utf-8",
        )
        (self.fixture.root / parity_report.RESULTS_CSV).write_text(
            "title,resvg\npainting/fill/a.svg,2\npainting/fill/b.svg,1\n", encoding="utf-8"
        )
        stdout = io.StringIO()

        result = parity_report.main(
            ["--repo-root", str(self.fixture.root), "--head-to-head"], stdout=stdout
        )

        self.assertEqual(result, 0)
        head_to_head = json.loads(stdout.getvalue())
        self.assertEqual(
            (head_to_head["summary"]["donner_pass"], head_to_head["summary"]["engine_pass"]),
            (1, 1),
        )
        self.assertEqual([row["path"] for row in head_to_head["gap"]], ["painting/fill/b.svg"])


_SVG_INPUT_LIMIT = 128 * 1024
_SVG_TEXT_ELEMENTS = {"text", "tspan", "tref", "textPath"}


def _read_svg_xml(data: bytes) -> bytes:
    if data.startswith(b"\x1f\x8b"):
        with gzip.GzipFile(fileobj=io.BytesIO(data)) as archive:
            data = archive.read(_SVG_INPUT_LIMIT + 1)
    if len(data) > _SVG_INPUT_LIMIT:
        raise ValueError("SVG fixture exceeds the text inventory input limit")
    return data


def _check_svg_entity(name, is_parameter_entity, value, *_unused) -> None:
    # The four DTD fixtures use short, literal internal entities. Reject declarations
    # involving external fetches, parameter entities or recursive expansion.
    if is_parameter_entity or value is None or "&" in value or len(value) > 4096:
        raise ValueError(f"Unsupported entity in SVG fixture: {name}")


def _svg_text_elements_and_hrefs(data: bytes) -> tuple[bool, set[str]]:
    has_text = False
    hrefs: set[str] = set()

    def inspect_element(name, attributes):
        nonlocal has_text
        has_text |= name.rsplit("}", 1)[-1] in _SVG_TEXT_ELEMENTS
        hrefs.update(value for key, value in attributes.items() if key.rsplit("}", 1)[-1] == "href")

    parser = expat.ParserCreate(namespace_separator="}")
    parser.SetParamEntityParsing(expat.XML_PARAM_ENTITY_PARSING_NEVER)
    parser.EntityDeclHandler = _check_svg_entity
    parser.ExternalEntityRefHandler = lambda *_unused: 0
    parser.StartElementHandler = inspect_element
    parser.Parse(_read_svg_xml(data), True)
    return has_text, hrefs


def _embedded_svg(href: str) -> bytes | None:
    header, _, payload = href.partition(",")
    if header[5:].split(";", 1)[0] not in ("image/svg+xml", ""):
        return None
    data = unquote_to_bytes(payload)
    if ";base64" in header:
        data = base64.b64decode(data)
    if data.lstrip().startswith(b"<") or data.startswith(b"\x1f\x8b"):
        return data
    return None


def _linked_svg(href: str, source: Path, resources: Path) -> Path | None:
    url = urlsplit(href)
    if url.scheme or url.netloc or not url.path:
        return None
    # Keep runfile symlinks: resolving them would escape the test's logical tree.
    linked = Path(os.path.normpath(source.parent / unquote(url.path)))
    if linked.is_relative_to(resources) and linked.suffix in (".svg", ".svgz") and linked.is_file():
        return linked
    return None


def _svg_href_contains_text(href: str, source: Path, resources: Path,
                            visited: frozenset[Path]) -> bool:
    if href.startswith("data:"):
        embedded = _embedded_svg(href)
        return embedded is not None and _svg_contains_text(embedded, source, resources, visited)
    linked = _linked_svg(href, source, resources)
    if linked is None or linked in visited:
        return False
    return _svg_contains_text(linked.read_bytes(), linked, resources, visited | {linked})


def _svg_contains_text(data: bytes, source: Path, resources: Path,
                       visited: frozenset[Path]) -> bool:
    has_text, hrefs = _svg_text_elements_and_hrefs(data)
    return has_text or any(_svg_href_contains_text(href, source, resources, visited) for href in hrefs)


class RealTreeParityReportTests(unittest.TestCase):
    def test_text_inventory_rejects_nonliteral_xml_entities(self) -> None:
        for entity in (b'<!ENTITY % value "text">',
                       b'<!ENTITY value SYSTEM "https://example.invalid/text">',
                       b'<!ENTITY value "&other;">'):
            with self.subTest(entity=entity):
                with self.assertRaisesRegex(ValueError, "Unsupported entity"):
                    _svg_text_elements_and_hrefs(b"<!DOCTYPE svg [" + entity + b"]><svg/>")

    def test_max_text_fixture_list_matches_direct_and_linked_svg_text(self) -> None:
        root = parity_report._default_repo_root() / parity_report.TESTS_ROOT
        build = (root.parent / "BUILD.bazel").read_text(encoding="utf-8")
        extras = re.search(r"_TEXT_CASES_OUTSIDE_TEXT_DIRECTORY = \[(.*?)\]", build, re.S)
        self.assertIsNotNone(extras)
        declared = re.findall(r'"([^"]+)"', extras.group(1))
        self.assertEqual(declared, sorted(set(declared)))
        expected = {
            path.relative_to(root.parent).as_posix()
            for path in root.rglob("*.svg")
            if path.relative_to(root).parts[0] != "text"
            and _svg_contains_text(path.read_bytes(), path, root.parent, frozenset({path}))
        }
        self.assertEqual(set(declared), expected)
        self.assertIn("tests/structure/image/embedded-svg-with-text.svg", expected)
        self.assertIn("tests/structure/use/xlink-to-an-external-file.svg", expected)
        self.assertNotIn("tests/structure/image/embedded-png.svg", expected)

    def test_exception_catalog_and_feature_gap_doc_match_live_registrations(self) -> None:
        root = parity_report._default_repo_root()
        self.assertTrue((root / parity_report.EXCEPTION_POLICIES).is_file())
        self.assertTrue((root / parity_report.FEATURE_GAP_DOC).is_file())
        report = parity_report.generate_report(root)
        text = (root / parity_report.FEATURE_GAP_DOC).read_text(encoding="utf-8")
        parity_report.check_inventory_doc(report, text)
        parity_report.check_renderer_contract(root)
        by_path = {c["path"]: c for c in report["cases"]}
        tile = by_path["filters/feTile/complex-transform.svg"]
        self.assertEqual(tile["render_only_kind"], "undefined-behavior")
        self.assertEqual(tile["policies"][0]["classification"], "known-renderer-divergence")
        expected_marker_classes = {
            "filters/feConvolveMatrix/edgeMode=wrap-with-matrix-larger-than-target.svg":
                "specification-undefined",
            "filters/feTile/complex-transform.svg": "known-renderer-divergence",
            "filters/feTurbulence/stitchTiles=stitch.svg": "known-renderer-divergence",
            "painting/stroke-width/negative.svg": "known-renderer-divergence",
            "structure/svg/invalid-id-attribute-1.svg": "upstream-no-reference",
        }
        self.assertEqual(
            {path: by_path[path]["policies"][0]["classification"]
             for path in expected_marker_classes},
            expected_marker_classes,
        )
        self.assertEqual(
            sorted(path for path, (cls, _) in parity_report.DEFINED_MARKER_CASES.items()
                   if cls == "known-renderer-divergence") + sorted(
                       parity_report.UNDEFINED_MARKER_CASES),
            ["filters/feTile/complex-transform.svg", "filters/feTurbulence/stitchTiles=stitch.svg",
             "painting/stroke-width/negative.svg",
             "filters/feConvolveMatrix/edgeMode=wrap-with-matrix-larger-than-target.svg"],
        )
        self.assertIn("docs/spec_audit/painting_properties.md",
                      by_path["painting/stroke-width/negative.svg"]["policies"][0]["evidence"])
        self.assertEqual(parity_report.marker_case_violations(
            report, root / parity_report.TESTS_ROOT, root / parity_report.UB_MARKER_REFERENCE), [])
        blur = by_path["filters/feGaussianBlur/huge-stdDeviation.svg"]
        self.assertEqual(blur["policies"][0]["classification"], "implementation-resource-limit")
        with self.assertRaisesRegex(parity_report.ParityReportError, "stale"):
            parity_report.check_inventory_doc(report, text.replace("| Total |", "| Fixtures |"))
        self.assertEqual(sum(
            row["lanes"]["geode"]["configured_state"] == "capability-gated"
            for row in report["cases"]
        ), report["lane_summary"]["geode"]["capability-gated"]["unobserved"])

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
                         sum(bool(case["disabled_backends"]
                                  or case["simple_text_disabled_backends"]) for case in cases))

    def test_live_head_to_head_partitions_both_engines_passes(self) -> None:
        repo_root = parity_report._default_repo_root()
        report = parity_report.generate_report(repo_root)

        head_to_head = parity_report.generate_head_to_head(repo_root)

        summary = head_to_head["summary"]
        unscored = {row["path"] for row in head_to_head["unscored_release_additions"]}
        self.assertEqual(summary["total_cases"], report["summary"]["total_cases"])
        self.assertEqual(summary["unscored_release_additions"], len(unscored))
        self.assertLessEqual(unscored, parity_report.release_addition_cases(repo_root))
        self.assertEqual(summary["donner_pixel_pass"],
                         report["summary"]["compare_cases"]
                         - sum(row["state"] == "compare"
                               for row in head_to_head["unscored_release_additions"]))
        self.assertEqual(summary["donner_pass"],
                         summary["donner_pixel_pass"] + summary["donner_render_pass"])
        self.assertEqual(summary["both_pass"] + summary["donner_only"], summary["donner_pass"])
        self.assertEqual(summary["both_pass"] + summary["gap"], summary["engine_pass"])
        self.assertEqual(summary["margin"], summary["donner_pass"] - summary["engine_pass"])
        self.assertEqual(sum(summary["gap_by_state"].values()), summary["gap"])
        self.assertNotIn("passed", summary["donner_only_by_engine_result"])
        self.assertNotIn("compare", summary["gap_by_state"])
        # Known rows pin the column that is read: resvg failed the first and passed the second.
        self.assertIn(
            {"engine_result": "failed", "path": "structure/style/important.svg"},
            head_to_head["donner_only"],
        )
        self.assertEqual(
            {row["path"] for row in head_to_head["render_passes"]},
            {"filters/feGaussianBlur/huge-stdDeviation.svg",
             "structure/svg/funcIRI-with-invalid-characters.svg"},
        )
        self.assertEqual(summary["donner_render_pass"], 2)
        self.assertGreater(summary["engine_pass"], 1000)

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

    def test_live_registrations_follow_the_ratchets_and_document_goldens(self) -> None:
        repo_root = parity_report._default_repo_root()
        report = parity_report.generate_report(repo_root)
        guards = {
            "Every golden override needs a reason (WithGoldenOverride's third argument, "
            ".withReason or .withGeodeGoldenOverride's second argument)":
                parity_report.golden_reason_violations(report),
            "PIXEL_BUDGET_ALLOWLIST records every compared budget above 100 pixels at its current "
            "value and may only shrink":
                parity_report.pixel_budget_ratchet_violations(report),
            "THRESHOLD_ALLOWLIST records every compared color threshold above 0.05 at its current "
            "value and may only shrink":
                parity_report.threshold_ratchet_violations(report),
            "SKIP_ALLOWLIST equals the active Params::Skip rows and may only shrink":
                parity_report.skip_ratchet_violations(report),
            "QUARTER_PIXEL_EDGE_ALLOWLIST equals the rows whose Geode comparison accepts "
            "quarter-pixel edge coverage differences":
                parity_report.quarter_pixel_edge_violations(
                    report, parity_report.quantized_goldens(repo_root)),
            "GEODE_GATE_ALLOWLIST equals the rows that disable their Geode comparison, each with a "
            "reason":
                parity_report.geode_gate_violations(report),
            "Every registered golden needs a golden README table row with its SHA-256":
                parity_report.golden_documentation_violations(report, repo_root),
            "Every case's canvas comes from a readable reference PNG or an explicit "
            "setCanvasSize(); the harness fails a comparison whose reference it cannot read":
                parity_report.unresolved_canvas_violations(report),
            "Every file in the resvg v0.50.0 manifest has the SHA-256 its provenance records "
            f"({parity_report.UPSTREAM_RELEASE_MANIFEST})":
                parity_report.upstream_release_manifest_violations(
                    (repo_root / parity_report.UPSTREAM_RELEASE_MANIFEST).read_text(
                        encoding="utf-8"),
                    repo_root / parity_report.UPSTREAM_RELEASE_MANIFEST.parent),
            "The provenance notes state the manifest's file counts and PNG size":
                parity_report.upstream_release_note_violations(
                    (repo_root / parity_report.UPSTREAM_RELEASE_MANIFEST).read_text(
                        encoding="utf-8"),
                    (repo_root / parity_report.UPSTREAM_RELEASE_NOTES).read_text(encoding="utf-8"),
                    repo_root / parity_report.UPSTREAM_RELEASE_MANIFEST.parent),
        }
        for rule, violations in guards.items():
            with self.subTest(rule=rule):
                self.assertEqual(violations, [], rule + ":\n" + "\n".join(violations))
        self.assertEqual(len(parity_report.SKIP_ALLOWLIST), report["summary"]["skip_cases"])
        self.assertEqual(len(parity_report.QUARTER_PIXEL_EDGE_ALLOWLIST),
                         report["summary"]["geode_quarter_pixel_edge_cases"])
        self.assertGreater(len(parity_report.referenced_goldens(report)), 0)


if __name__ == "__main__":
    unittest.main()
