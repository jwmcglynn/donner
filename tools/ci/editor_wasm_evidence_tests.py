"""Hosted browser evidence is tied to its test label, package and declared output."""
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from zipfile import ZipFile

from editor_wasm_evidence import HOSTED, QUARANTINE, RESIDENCY, stage


class HostedEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.destination = self.root / "evidence"

    def event(self, label, status="PASSED"):
        package, name = label[2:].split(":")
        logs = self.root / "config/testlogs" / package / name
        archives = logs / "test.outputs"
        archives.mkdir(parents=True, exist_ok=True)
        (logs / "test.log").write_text("Editor browser cases passed: 3\n")
        archive = archives / "outputs.zip"
        with ZipFile(archive, "w") as output:
            output.writestr("playwright/fixture/gpu-residency-accounting.json", b"{}")
            output.writestr("quarantine-report.json", b'{"suites":[]}')
        return {"id": {"testResult": {"label": label}}, "testResult": {
            "status": status, "testActionOutput": [
                {"name": "test.log", "uri": (logs / "test.log").as_uri()},
                {"name": "test.outputs__outputs.zip", "uri": archive.as_uri()},
            ]}}

    def test_all_projects_have_separate_outputs_and_case_counts(self):
        stream = (json.dumps(self.event(label)) for label in HOSTED)
        stage(stream, self.root, self.destination)
        self.assertEqual(len(list(self.destination.glob("*/outputs.zip"))), 7)
        self.assertEqual([p.read_text() for p in self.destination.glob("*/case-count.txt")], ["3\n"] * 7)
        with self.assertRaisesRegex(ValueError, "Missing hosted browser evidence"):
            stage([], self.root, self.destination)

    def test_failed_quarantine_still_retains_its_json_report_and_archive(self):
        stage([json.dumps(self.event(QUARANTINE, "FAILED"))], self.root, self.destination,
              quarantine=True, complete=False)
        directory = self.destination / "firefox-quarantine-report"
        self.assertEqual((directory / "quarantine-report.json").read_text(), '{"suites":[]}')
        self.assertTrue((directory / "outputs.zip").is_file())

    def test_failed_browser_without_attachments_retains_its_declared_log(self):
        label = sorted(HOSTED)[0]
        event = self.event(label, "FAILED")
        event["testResult"]["testActionOutput"] = event["testResult"]["testActionOutput"][:1]
        stage([json.dumps(event)], self.root, self.destination, complete=False)
        self.assertEqual(len(list(self.destination.glob("*/test.log"))), 1)

    def test_webgpu_accounting_cannot_be_replaced_by_a_case_count(self):
        label = sorted(RESIDENCY)[0]
        event = self.event(label)
        event["testResult"]["testActionOutput"] = event["testResult"]["testActionOutput"][:1]
        with self.assertRaisesRegex(ValueError, "Missing declared browser output archive"):
            stage([json.dumps(event)], self.root, self.destination)

    def test_missing_quarantine_output_reports_failure_after_retaining_log(self):
        event = self.event(QUARANTINE, "FAILED")
        event["testResult"]["testActionOutput"] = event["testResult"]["testActionOutput"][:1]
        with self.assertRaisesRegex(ValueError, "Missing declared browser output archive"):
            stage([json.dumps(event)], self.root, self.destination, quarantine=True, complete=False)
        self.assertTrue((self.destination / "firefox-quarantine-report/test.log").is_file())
        with self.assertRaisesRegex(ValueError, "Missing hosted browser evidence"):
            stage([], self.root, self.destination, quarantine=True, complete=False)

    def test_rejects_another_targets_local_output(self):
        first, other = sorted(HOSTED)[:2]
        event = self.event(first)
        event["testResult"]["testActionOutput"] = self.event(other)["testResult"]["testActionOutput"]
        with self.assertRaisesRegex(ValueError, "Unexpected Bazel output"):
            stage([json.dumps(event)], self.root, self.destination, complete=False)


if __name__ == "__main__":
    unittest.main()
