"""Stdlib regression tests: python3 -m unittest discover -p test_triage_evidence.py."""

import os
import struct
import tempfile
import time
import unittest
import zlib
from pathlib import Path

from test_output_parser import parse_test_output
from triage_evidence import (
    MAX_IMAGE_BYTES,
    MAX_TEXT_BYTES,
    bounded_text,
    comparator_count,
    failure_assessment,
    inspect_png,
    read_png,
    visual_evidence,
)


def chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload)))


def png(width=1, height=1, rgb=(255, 255, 255), extra=b""):
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    # Oversized-header negative controls do not allocate the declared image.
    data = zlib.compress(b"\0" + bytes(rgb))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + extra
            + chunk(b"IDAT", data) + chunk(b"IEND", b""))


class EvidenceTests(unittest.TestCase):
    def test_counts_and_detected_features_cannot_establish_cause(self):
        for count in (0, 99, 100, 499, 999, 5000, None):
            with self.subTest(count=count):
                self.assertEqual(failure_assessment(count), ("unresolved", "unknown"))

    def test_metric_rejects_lossy_or_negative_coercion(self):
        for count in (-1, True, 1.5, "100"):
            with self.subTest(count=count), self.assertRaises(ValueError):
                comparator_count(count)

    def test_display_diff_nonzero_rgb_is_not_a_mismatch_mask(self):
        white = inspect_png(png())
        images = {"actual": white, "expected": white, "diff": white}
        without_metric = visual_evidence(images)
        self.assertIsNone(without_metric["visual_analysis"]["diff_pixels"])
        report = visual_evidence(images, 0)
        self.assertEqual(report["visual_analysis"]["diff_pixels"], 0)
        self.assertEqual(report["difference_type"], "unresolved")
        self.assertIsNone(report["likely_cause"])
        self.assertEqual(report["hypotheses"], [])

    def test_comparator_metric_must_fit_dimensions(self):
        with self.assertRaises(ValueError):
            visual_evidence({"diff": inspect_png(png())}, 2)

    def test_images_must_have_matching_dimensions(self):
        with self.assertRaises(ValueError):
            visual_evidence({"actual": inspect_png(png()), "diff": inspect_png(png(2))})

    def test_dimension_and_decoded_pixel_limits(self):
        for dimensions in ((0, 1), (4097, 1), (1, 4097), (4096, 4096)):
            with self.subTest(dimensions=dimensions), self.assertRaises(ValueError):
                inspect_png(png(*dimensions))

    def test_invalid_and_animated_containers_fail_closed(self):
        for data in (b"not png", png()[:-1], png() + b"trailing",
                     png(extra=chunk(b"acTL", struct.pack(">II", 1, 0)))):
            with self.subTest(data=data[:20]), self.assertRaises(ValueError):
                inspect_png(data)
        corrupted = bytearray(png())
        corrupted[29] ^= 1
        with self.assertRaisesRegex(ValueError, "checksum"):
            inspect_png(bytes(corrupted))

    def test_text_input_bounds(self):
        self.assertEqual(bounded_text("svg"), "svg")
        with self.assertRaises(ValueError):
            bounded_text("x" * (MAX_TEXT_BYTES + 1))


class ArtifactAccessTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        # On macOS the temporary directory may contain an operating-system symlink.
        self.root = Path(self.temporary.name).resolve()
        self.path = self.root / "image.png"
        self.path.write_bytes(png())

    def test_default_denies_all_file_access(self):
        with self.assertRaisesRegex(ValueError, "requires"):
            read_png(str(self.path))

    def test_confined_relative_and_absolute_files(self):
        for path in ("image.png", str(self.path)):
            with self.subTest(path=path):
                image = read_png(path, str(self.root))
                self.assertEqual((image.width, image.height), (1, 1))
                self.assertEqual(image.data, png())

    def test_parent_and_outside_paths_are_rejected(self):
        for path in ("../image.png", str(self.root.parent / "image.png")):
            with self.subTest(path=path), self.assertRaises(ValueError):
                read_png(path, str(self.root))

    def test_symlink_file_directory_and_root_are_rejected(self):
        (self.root / "link.png").symlink_to(self.path)
        (self.root / "linkdir").symlink_to(self.root, target_is_directory=True)
        for path, root in (("link.png", self.root), ("linkdir/image.png", self.root),
                           ("image.png", self.root / "linkdir")):
            with self.subTest(path=path, root=root), self.assertRaises(OSError):
                read_png(path, str(root))

    def test_nonregular_file_is_rejected_without_blocking(self):
        os.mkfifo(self.root / "pipe.png")
        with self.assertRaises(ValueError):
            read_png("pipe.png", str(self.root))

    def test_encoded_byte_limit(self):
        with self.path.open("wb") as stream:
            stream.truncate(MAX_IMAGE_BYTES + 1)
        with self.assertRaisesRegex(ValueError, "16 MiB"):
            read_png("image.png", str(self.root))


class RepeatOutputTests(unittest.TestCase):
    # Captured from GoogleTest 1.18 with --gtest_repeat=2 and a one-test filter.
    # The launcher source-path line is omitted; iteration output is unchanged.
    OUTPUT = 'Repeating all tests (iteration 1) . . .\n\nNote: Google Test filter = FileFingerprintTest.StandardSha256KnownVectorsAndStreaming\n[==========] Running 1 test from 1 test suite.\n[----------] Global test environment set-up.\n[----------] 1 test from FileFingerprintTest\n[ RUN      ] FileFingerprintTest.StandardSha256KnownVectorsAndStreaming\n[       OK ] FileFingerprintTest.StandardSha256KnownVectorsAndStreaming (7 ms)\n[----------] 1 test from FileFingerprintTest (7 ms total)\n\n[==========] 1 test from 1 test suite ran. (7 ms total)\n[  PASSED  ] 1 test.\n\nRepeating all tests (iteration 2) . . .\n\nNote: Google Test filter = FileFingerprintTest.StandardSha256KnownVectorsAndStreaming\n[==========] Running 1 test from 1 test suite.\n[----------] 1 test from FileFingerprintTest\n[ RUN      ] FileFingerprintTest.StandardSha256KnownVectorsAndStreaming\n[       OK ] FileFingerprintTest.StandardSha256KnownVectorsAndStreaming (7 ms)\n[----------] 1 test from FileFingerprintTest (7 ms total)\n\n[----------] Global test environment tear-down\n[==========] 1 test from 1 test suite ran. (7 ms total)\n[  PASSED  ] 1 test.\n'

    def test_captured_finite_repeat_completes_with_expected_count(self):
        records, summary = parse_test_output(self.OUTPUT, expected_iterations=2)
        self.assertEqual([item.iteration for item in records], [1, 2])
        self.assertEqual(summary.total_tests, 2)
        self.assertEqual(summary.completed_iterations, 2)
        self.assertTrue(summary.run_complete)

    def test_repeat_without_expected_count_is_not_proof_of_finite_completion(self):
        records, summary = parse_test_output(self.OUTPUT)
        self.assertEqual(len(records), 2)
        self.assertEqual(summary.completed_iterations, 2)
        self.assertFalse(summary.run_complete)

    def test_earlier_failure_survives_later_pass_and_summary_repetition(self):
        output = self.OUTPUT.replace('[       OK ]', '[  FAILED  ]', 1)
        output = output.replace('Repeating all tests (iteration 2)',
            '[  FAILED  ] FileFingerprintTest.StandardSha256KnownVectorsAndStreaming\n'
            'Repeating all tests (iteration 2)', 1)
        records, summary = parse_test_output(output, expected_iterations=2)
        self.assertEqual([(item.iteration, item.status) for item in records],
                         [(1, 'FAILED'), (2, 'PASSED')])
        self.assertEqual((summary.failed, summary.passed), (1, 1))
        self.assertTrue(summary.run_complete)

    def test_mid_iteration_truncation_retains_incomplete_case(self):
        output = self.OUTPUT.rsplit('[       OK ]', 1)[0]
        records, summary = parse_test_output(output, expected_iterations=2)
        self.assertEqual([(item.iteration, item.status) for item in records],
                         [(1, 'PASSED'), (2, 'INCOMPLETE')])
        self.assertFalse(summary.run_complete)

    def test_next_iteration_header_without_results_invalidates_completion(self):
        for suffix in ('Repeating all tests (iteration 3) . . .\n',
                       '[==========] Running 1 test from 1 test suite.\n'):
            with self.subTest(suffix=suffix):
                _, summary = parse_test_output(self.OUTPUT + suffix, expected_iterations=2)
                self.assertFalse(summary.run_complete)

    def test_missing_iteration_cannot_be_hidden_by_final_footer(self):
        output = self.OUTPUT.replace('iteration 2', 'iteration 3')
        _, summary = parse_test_output(output, expected_iterations=3)
        self.assertFalse(summary.run_complete)

    def test_prefixed_protocol_diagnostics_are_not_runner_records(self):
        output = ('[ RUN ] Suite.case\n'
                  'diagnostic: Repeating all tests (iteration 2) . . .\n'
                  'diagnostic: [==========] Running 10 tests from 1 test suite.\n'
                  'diagnostic: [ RUN ] Fake.case\n'
                  '[ OK ] Suite.case\n'
                  '[==========] 1 test from 1 test suite ran. (1 ms total)\n')
        records, summary = parse_test_output(output)
        self.assertEqual([item.test_id for item in records], ['Suite.case'])
        self.assertTrue(summary.run_complete)

    def test_expected_iterations_requires_positive_integer(self):
        for value in (True, 0, -1, 2.0, '2'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                parse_test_output(self.OUTPUT, expected_iterations=value)


class OutputEvidenceTests(unittest.TestCase):
    def test_unrelated_diagnostics_do_not_supply_comparator_counts(self):
        diagnostics = (
            "parser error: 42 pixels differ",
            "renderer failure: 42 pixels different.",
            "warning: [ COMPARE ] /tests/shapes/rect/a.svg [TinySkia]: 42 pixels differ",
            "[ COMPARE ] /tests/shapes/rect/a.svg [TinySkia]: parser error: 42 pixels differ",
            "[ COMPARE ] /tests/shapes/rect/42 pixels differ.svg [TinySkia]: PASS",
            "[ COMPARE ] /tests/a.svg [TinySkia]: 42 pixels differ/final.svg [TinySkia]: PASS",
        )
        for diagnostic in diagnostics:
            with self.subTest(diagnostic=diagnostic):
                records, _ = parse_test_output(
                    "[ RUN ] Suite.Test/case\n" + diagnostic
                    + "\n[ FAILED ] Suite.Test/case\n")
                self.assertIsNone(records[0].pixel_diff)
                self.assertIn(diagnostic, records[0].raw_output)

    def test_later_diagnostic_does_not_replace_comparator_count(self):
        records, _ = parse_test_output(
            "[ RUN ] Suite.Test/case\n"
            "[ COMPARE ] /tests/shapes/rect/a.svg [TinySkia]: FAIL (7 pixels differ, with 0 max)\n"
            "parser diagnostic: 42 pixels differ\n[ FAILED ] Suite.Test/case\n")
        self.assertEqual(records[0].pixel_diff, 7)

    def test_recognized_comparator_records_retain_counts(self):
        for payload, expected in (("1234 pixels differ", 1234),
                                  ("FAIL (7 pixels differ, with 0 max)", 7),
                                  ("PASS (2 pixels differ, out of 100 max)", 2)):
            with self.subTest(payload=payload):
                records, _ = parse_test_output(
                    "[ RUN ] Suite.Test/case\n"
                    "[ COMPARE ] /tests/shapes/rect/a.svg [TinySkia]: " + payload
                    + "\n[ FAILED ] Suite.Test/case\n")
                self.assertEqual(records[0].pixel_diff, expected)

    def test_oversized_numeric_token_is_not_reparsed_from_each_offset(self):
        start = time.monotonic()
        records, summary = parse_test_output(
            "[ RUN ] Suite.Test/case\n[ COMPARE ] /tests/a.svg [TinySkia]: " + "9" * 100000 + " pixels difference\n"
            + "[ COMPARE ] /tests/a.svg [TinySkia]: " + "9" * 100000
            + " pixels differ\n[ FAILED ] Suite.Test/case\n"
        )
        self.assertEqual(summary.failed, 1)
        self.assertIsNone(records[0].pixel_diff)
        self.assertLess(time.monotonic() - start, 1.0)

    def test_numeric_metric_boundary_does_not_accept_long_token_suffix(self):
        records, _ = parse_test_output("[ RUN ] Suite.Test/case\n"
            "[ COMPARE ] /tests/a.svg [TinySkia]: 12345678901 pixels differ\n"
            "[ FAILED ] Suite.Test/case\n")
        self.assertIsNone(records[0].pixel_diff)

    def test_crash_at_eof_retains_incomplete_test(self):
        records, summary = parse_test_output("[ RUN ] SvgSuite.a\n[ OK ] SvgSuite.a\n"
                                             "[ RUN ] SvgSuite.b\nSegmentation fault\n")
        self.assertEqual(len(records), 2)
        self.assertEqual(records[1].status, "INCOMPLETE")
        self.assertIn("Segmentation fault", records[1].raw_output)
        self.assertEqual(summary.incomplete, 1)
        self.assertEqual(summary.completion_rate, 0.5)
        self.assertEqual(summary.run_complete, False)

    def test_truncated_run_line_retained_without_diagnostic(self):
        records, summary = parse_test_output("[ RUN ] SvgSuite.truncated\n")
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0].status, "INCOMPLETE")
        self.assertEqual(summary.run_complete, False)

    def test_next_run_preserves_interrupted_case(self):
        records, summary = parse_test_output("[ RUN ] SvgSuite.first\ninterrupted\n"
                                             "[ RUN ] SvgSuite.second\n[ OK ] SvgSuite.second\n")
        self.assertEqual([item.status for item in records], ["INCOMPLETE", "PASSED"])
        self.assertIn("interrupted", records[0].raw_output)
        self.assertEqual(summary.run_complete, False)

    def test_pass_without_runner_footer_does_not_prove_complete_run(self):
        _, summary = parse_test_output("[ RUN ] SvgSuite.a\n[ OK ] SvgSuite.a\n")
        self.assertEqual(summary.run_complete, False)

    def test_matching_runner_footer_proves_observed_run_completion(self):
        _, summary = parse_test_output("[ RUN ] SvgSuite.a\n[ OK ] SvgSuite.a\n"
            "[==========] 1 test from 1 test suite ran. (1 ms total)\n")
        self.assertEqual(summary.run_complete, True)

    def test_runner_footer_count_mismatch_is_incomplete_evidence(self):
        _, summary = parse_test_output("[ RUN ] SvgSuite.a\n[ OK ] SvgSuite.a\n"
            "[==========] 2 tests from 1 test suite ran. (1 ms total)\n")
        self.assertEqual(summary.run_complete, False)

    def test_retained_resvg_failure_output(self):
        fixture = Path(__file__).with_name("fixtures") / "resvg_quiet_failure.txt"
        output = fixture.read_text()
        records, summary = parse_test_output(output)
        self.assertEqual(summary.failed, 1)
        self.assertEqual(records[0].pixel_diff, 37354)
        self.assertEqual(records[0].svg_path, "/artifacts/tests/text/tspan/with-filter.svg")
        self.assertEqual(records[0].raw_output, output.rstrip("\n"))

    def test_retained_summary_only_accepts_getparam_comma(self):
        fixture = Path(__file__).with_name("fixtures") / "resvg_quiet_failure.txt"
        summary_line = fixture.read_text().splitlines()[-1]
        records, summary = parse_test_output(summary_line)
        self.assertEqual(summary.failed, 1)
        self.assertEqual(records[0].test_name, "with-filter.svg")
        self.assertEqual(records[0].category, "text/tspan")
        self.assertIsNone(records[0].pixel_diff)

    def test_quiet_failure_keeps_comparator_count_and_raw_output(self):
        output = """[ RUN      ] PaintingFill/ImageComparisonTestFixture.ResvgTest/example
[  COMPARE ] /artifacts/tests/painting/fill/example.svg [TinySkia]: FAIL (7 pixels differ, with 0 max)
ImageComparisonTestFixture.cc:957: Failure
7 pixels different.
[  FAILED  ] PaintingFill/ImageComparisonTestFixture.ResvgTest/example (1 ms)
[  FAILED  ] 1 test, listed below:
[  FAILED  ] PaintingFill/ImageComparisonTestFixture.ResvgTest/example, where GetParam() = /artifacts/tests/painting/fill/example.svg
"""
        records, summary = parse_test_output(output)
        self.assertEqual(summary.failed, 1)
        self.assertEqual(records[0].pixel_diff, 7)
        self.assertEqual(records[0].category, "painting/fill")
        self.assertIn("7 pixels different.", records[0].raw_output)

    def test_pre_render_failure_has_unknown_count(self):
        records, summary = parse_test_output("""[ RUN      ] SvgSuite.Parser/malformed
Parser failure before rendering
[  FAILED  ] SvgSuite.Parser/malformed (0 ms)
""")
        self.assertEqual(summary.failed, 1)
        self.assertIsNone(records[0].pixel_diff)
        self.assertIn("Parser failure", records[0].raw_output)

    def test_same_leaf_name_different_categories_stays_distinct(self):
        output = "\n".join(
            f"[ RUN      ] {category}/Fixture.ResvgTest/simple\n"
            f"[  COMPARE ] /tests/{category}/simple.svg [TinySkia]: FAIL (1 pixels differ)\n"
            f"[  FAILED  ] {category}/Fixture.ResvgTest/simple"
            for category in ("text", "painting")
        )
        records, summary = parse_test_output(output)
        self.assertEqual(summary.failed, 2)
        self.assertEqual({item.category for item in records}, {"text", "painting"})

    def test_rerun_pass_does_not_erase_retained_failure(self):
        records, summary = parse_test_output("""[ RUN      ] Suite.Test/case
[  FAILED  ] Suite.Test/case
[ RUN      ] Suite.Test/case
[       OK ] Suite.Test/case
[  FAILED  ] Suite.Test/case
""")
        self.assertEqual((summary.passed, summary.failed), (1, 1))
        self.assertEqual([item.status for item in records], ["FAILED", "PASSED"])

    def test_runtime_skip_and_pass_remain_separate_from_failures(self):
        records, summary = parse_test_output("""[ RUN      ] Text/Fixture.ResvgTest/arabic
[  SKIPPED ] Text/Fixture.ResvgTest/arabic (0 ms)
[ RUN      ] Fill/Fixture.ResvgTest/red
[       OK ] Fill/Fixture.ResvgTest/red (0 ms)
[  SKIPPED ] Text/Fixture.ResvgTest/arabic
""")
        self.assertEqual((summary.passed, summary.failed, summary.skipped), (1, 0, 1))
        self.assertEqual(len(records), 2)


if __name__ == "__main__":
    unittest.main()
