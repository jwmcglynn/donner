"""Tests for the coverage BEP classifier.

The classifier decides whether a missing coverage report is benign, so the
interesting cases are the ones where it must REFUSE to say "benign": a real
build failure, a partial stream, and a run that mixed a skip with a result.
"""

#!/usr/bin/env python3
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import coverage_bep_status as status


def skipped_event(label):
    return json.dumps(
        {
            "id": {"targetCompleted": {"label": label}},
            "aborted": {
                "reason": "SKIPPED",
                "description": "Target %s build was skipped." % label,
            },
        }
    )


def completed_event(label, success=True):
    return json.dumps(
        {"id": {"targetCompleted": {"label": label}}, "completed": {"success": success}}
    )


def test_result_event(label):
    return json.dumps(
        {"id": {"testResult": {"label": label}}, "testResult": {"status": "PASSED"}}
    )


def failed_gpu_events(label, declare_xml=True):
    output = [{"name": "test.xml", "uri": "file:///private/runner/path"}] if declare_xml else []
    return [
        json.dumps({
            "id": {"testResult": {"label": label}},
            "testResult": {"status": "FAILED", "testActionOutput": output},
        }),
        json.dumps({
            "id": {"testSummary": {"label": label}},
            "testSummary": {"overallStatus": "FAILED"},
        }),
    ]


def write_gpu_xml(root, label, xml):
    path = root / status._GTEST_XML_BY_LABEL[label]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(xml, encoding="utf-8")
    return path


class ClassifyTest(unittest.TestCase):
    def test_allowlisted_cases_emit_only_validated_identifiers(self):
        golden = "//donner/svg/renderer/tests:renderer_geode_golden_tests"
        baseline = "//donner/gpu/baseline:baseline_pixels_tests"
        xml = (
            '<testsuites failures="2" errors="0"><testsuite name="RendererGeodeGoldenTests">'
            '<testcase classname="RendererGeodeGoldenTests" name="Lion">'
            '<failure message="/private/runner/path">secret assertion text</failure></testcase>'
            '<testcase classname="RendererGeodeGoldenTests" name="PatternSolid">'
            '<error message="/private/runner/path" /></testcase>'
            '</testsuite></testsuites>'
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bazel-testlogs"
            write_gpu_xml(root, golden, xml)
            write_gpu_xml(root, baseline, (
                '<testsuites failures="1"><testcase classname="BaselinePixelsTests" '
                'name="PixelParity"><failure message="secret" /></testcase></testsuites>'
            ))
            result = status.allowlisted_failure_cases(
                failed_gpu_events(golden) + failed_gpu_events(baseline), root)
        self.assertEqual(result["case_names_unavailable"], 0)
        self.assertEqual([item["target"] for item in result["targets"]], [baseline, golden])
        self.assertEqual(result["targets"][1]["failedCases"], [
            "RendererGeodeGoldenTests.Lion", "RendererGeodeGoldenTests.PatternSolid",
        ])
        self.assertEqual(result["targets"][1]["failedCaseCount"], 2)
        self.assertNotIn("private", json.dumps(result))
        self.assertNotIn("secret", json.dumps(result))

    def test_allowlisted_cases_fail_closed_on_missing_or_unsafe_xml(self):
        golden = "//donner/svg/renderer/tests:renderer_geode_golden_tests"
        events = failed_gpu_events(golden)
        documents = {
            "invalid_xml": "<testsuites failures='1'><testcase",
            "unsafe_case_name": (
                '<testsuites failures="1"><testcase classname="RendererGeodeGoldenTests" '
                'name="bad&#10;/private/path"><failure>secret</failure></testcase></testsuites>'
            ),
            "incomplete_xml": '<testsuites failures="1"><testcase name="Lion" /></testsuites>',
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bazel-testlogs"
            root.mkdir()
            missing = status.allowlisted_failure_cases(events, root)
            self.assertEqual(missing["targets"][0]["reason"], "missing_xml")
            self.assertEqual(missing["case_names_unavailable"], 1)
            for expected, document in documents.items():
                with self.subTest(expected=expected):
                    path = write_gpu_xml(root, golden, document)
                    result = status.allowlisted_failure_cases(events, root)["targets"][0]
                    self.assertEqual(result["status"], "unavailable")
                    self.assertEqual(result["reason"], expected)
                    self.assertNotIn("private", json.dumps(result))
                    self.assertNotIn("secret", json.dumps(result))
            path.write_bytes(b"x" * (status._MAX_GTEST_XML_BYTES + 1))
            self.assertEqual(status.allowlisted_failure_cases(events, root)["targets"][0]
                             ["reason"], "oversize_xml")
            path.write_text('<!DOCTYPE testsuites [<!ENTITY x "secret">]>'
                            '<testsuites failures="1">&x;</testsuites>', encoding="utf-8")
            self.assertEqual(status.allowlisted_failure_cases(events, root)["targets"][0]
                             ["reason"], "invalid_xml")
            path.unlink()
            external = Path(directory) / "external.xml"
            external.write_text('<testsuites failures="1" />', encoding="utf-8")
            path.symlink_to(external)
            self.assertEqual(status.allowlisted_failure_cases(events, root)["targets"][0]
                             ["reason"], "symlink_xml")
            self.assertEqual(status.allowlisted_failure_cases(
                failed_gpu_events(golden, declare_xml=False), root)["targets"][0]["reason"],
                "undeclared_xml")

    def test_allowlisted_cases_are_capped_and_final_pass_suppresses_attempt(self):
        golden = "//donner/svg/renderer/tests:renderer_geode_golden_tests"
        labels = list(status._GTEST_XML_BY_LABEL)
        passed_summary = json.dumps({
            "id": {"testSummary": {"label": golden}},
            "testSummary": {"overallStatus": "PASSED"},
        })
        cases = "".join(
            '<testcase classname="RendererGeodeGoldenTests" name="Case%02d">'
            '<failure /></testcase>' % index for index in range(23)
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "bazel-testlogs"
            for label in labels:
                write_gpu_xml(root, label, '<testsuites failures="23">' + cases + '</testsuites>')
            self.assertIsNone(status.allowlisted_failure_cases(
                failed_gpu_events(golden) + [passed_summary], root))
            events = sum((failed_gpu_events(label) for label in labels), [])
            result = status.allowlisted_failure_cases(events, root)
        self.assertEqual(len(result["targets"]), 3)
        self.assertEqual([len(item["failedCases"]) for item in result["targets"]], [20, 10, 0])
        self.assertEqual(sum(item["failedCaseCount"] for item in result["targets"]), 69)
        self.assertEqual(result["omittedCases"], 39)
        self.assertEqual(result["case_names_unavailable"], 0)
        self.assertIsNone(status.allowlisted_failure_cases(
            failed_gpu_events("//untrusted:target"), Path("/nonexistent")))

    def test_failure_summary_reports_only_safe_bounded_labels(self):
        events = [
            json.dumps({
                "id": {"testSummary": {"label": "//donner/editor:failed_test"}},
                "testSummary": {"overallStatus": "FAILED", "passed": [{"uri": "file:///private/path"}]},
            }),
            json.dumps({
                "id": {"testResult": {"label": "//donner/svg:timed_out"}},
                "testResult": {"status": "TIMEOUT"},
            }),
            completed_event("//tools:failed_build", success=False),
            json.dumps({
                "id": {"targetSummary": {"label": "@@lib+//pkg:compile_test"}},
                "targetSummary": {"overallTestStatus": "FAILED_TO_BUILD"},
            }),
            json.dumps({
                "id": {"configuredLabel": {"label": "//donner/editor:analysis_failed"}},
                "aborted": {"reason": "ANALYSIS_FAILURE", "description": "/private/path"},
            }),
            json.dumps({
                "id": {"testResult": {"label": "//donner/editor:halted"}},
                "testResult": {"status": "TOOL_HALTED_BEFORE_TESTING"},
            }),
            json.dumps({
                "id": {"testSummary": {"label": "//donner/editor:malformed"}},
                "testSummary": {"overallStatus": []},
            }),
            json.dumps({
                "id": {"testSummary": {"label": "//evil\nprivate:path"}},
                "testSummary": {"overallStatus": "FAILED"},
            }),
        ]
        result = status.failure_summary(events)
        self.assertEqual(result["failedTests"], [
            "//donner/editor:failed_test", "//donner/editor:halted",
            "//donner/svg:timed_out", "@@lib+//pkg:compile_test",
        ])
        self.assertEqual(result["failedBuilds"], [
            "//donner/editor:analysis_failed", "//tools:failed_build",
        ])
        self.assertNotIn("private", json.dumps(result))
        self.assertEqual(result["omittedTests"], 0)

    def test_failure_summary_caps_output(self):
        events = [json.dumps({
            "id": {"testSummary": {"label": f"//fixture:failed_{index:02}"}},
            "testSummary": {"overallStatus": "FAILED"},
        }) for index in range(23)]
        result = status.failure_summary(events)
        self.assertEqual(len(result["failedTests"]), 20)
        self.assertEqual(result["omittedTests"], 3)

    def test_final_pass_or_flaky_summary_supersedes_failed_attempt(self):
        events = []
        for label, final_status in (("//fixture:recovered", "PASSED"),
                                    ("//fixture:flaky", "FLAKY")):
            events.append(json.dumps({
                "id": {"testResult": {"label": label}},
                "testResult": {"status": "FAILED"},
            }))
            events.append(json.dumps({
                "id": {"testSummary": {"label": label}},
                "testSummary": {"overallStatus": final_status},
            }))
        events.append(json.dumps({
            "id": {"testResult": {"label": "//fixture:incomplete"}},
            "testResult": {"status": "TIMEOUT"},
        }))
        self.assertEqual(status.failure_summary(events)["failedTests"],
                         ["//fixture:incomplete"])

    def test_single_incompatible_target_is_all_skipped(self):
        # The real shape this was written for: a pull request whose only
        # affected target is restricted to another platform.
        result = status.classify([skipped_event("//donner/editor/wasm/tests:smoke")])
        self.assertEqual(result["status"], status.ALL_SKIPPED)
        self.assertEqual(result["skipped"], ["//donner/editor/wasm/tests:smoke"])

    def test_every_target_skipped_is_all_skipped(self):
        result = status.classify(
            [skipped_event("//a:one"), skipped_event("//b:two")]
        )
        self.assertEqual(result["status"], status.ALL_SKIPPED)
        self.assertEqual(result["skipped"], ["//a:one", "//b:two"])

    def test_one_result_alongside_a_skip_is_not_benign(self):
        # A target really ran, so a missing report means something went wrong
        # with the report, not that there was nothing to measure.
        result = status.classify([skipped_event("//a:one"), completed_event("//b:two")])
        self.assertEqual(result["status"], status.HAS_RESULTS)
        self.assertEqual(result["produced"], ["//b:two"])

    def test_test_result_counts_as_a_result(self):
        result = status.classify([test_result_event("//a:one")])
        self.assertEqual(result["status"], status.HAS_RESULTS)

    def test_failed_completion_is_not_a_result_but_is_not_benign_either(self):
        # An unsuccessful completion with no skip is the ordinary build-failure
        # case; it must stay unknown so the caller keeps failing closed.
        result = status.classify([completed_event("//a:one", success=False)])
        self.assertEqual(result["status"], status.UNKNOWN)

    def test_empty_stream_is_unknown(self):
        self.assertEqual(status.classify([])["status"], status.UNKNOWN)
        self.assertEqual(status.classify(["", "  "])["status"], status.UNKNOWN)

    def test_unparseable_stream_is_unknown(self):
        self.assertEqual(status.classify(["not json"])["status"], status.UNKNOWN)

    def test_partial_trailing_line_does_not_hide_a_skip(self):
        # A run killed mid-write leaves a truncated final line; the completed
        # events before it still classify.
        result = status.classify([skipped_event("//a:one"), '{"id": {"targetCom'])
        self.assertEqual(result["status"], status.ALL_SKIPPED)

    def test_non_object_json_lines_are_ignored(self):
        self.assertEqual(status.classify(["[1,2,3]", "null"])["status"], status.UNKNOWN)

    def test_missing_file_is_unknown(self):
        self.assertEqual(status.main(["prog", "/nonexistent/bep.json"]), 0)


if __name__ == "__main__":
    unittest.main()
