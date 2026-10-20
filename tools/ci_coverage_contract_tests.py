"""Tests for tools/ci_coverage_contract.py and the checked-in coverage manifest."""

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

from python.runfiles import runfiles

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ci_coverage_contract as contract  # noqa: E402


def _manifest():
    path = runfiles.Create().Rlocation("donner/tools/ci/coverage_contract.json")
    return json.loads(Path(path).read_text(encoding="utf-8"))


def _receipt(job, steps):
    return {"version": contract.RECEIPT_VERSION, "job": job, "steps": steps}


def _ran(tests=None):
    entry = {"outcome": "success"}
    if tests is not None:
        entry.update({"tests": tests, "executed": tests})
    return entry


_SKIPPED = {"outcome": "skipped"}

# The receipts of a trusted pull request whose lanes ran every phase the manifest names.
_COMPLETE = {
    "linux": _receipt("linux", {
        "Verify BuildBuddy remote execution": _ran(), "Build": _SKIPPED, "Test": _SKIPPED,
        "Test local Xvfb targets": _ran(),
        "Test (hosted parity subset)": _ran(1),
        "Linker canary (non-lld linker support)": _ran(),
        "Build and test CMake consumer": _ran()}),
    "linux-self-hosted": _receipt("linux-self-hosted", {"Build": _ran(), "Test": _ran()}),
    "macos-self-hosted": _receipt("macos-self-hosted", {
        "Require full Metal shader validation": _ran(), "Metal validation preflight": _ran(),
        "Build": _ran(), "Test": _ran(), "Test (Geode editor lane)": _ran(6)}),
    "macos-browser": _receipt("macos-browser", {
        "Qualify pinned Chromium archive": _ran(), "Test (browser targets)": _ran(2)}),
    "macos-hosted-parity": _receipt("macos-hosted-parity", {
        "Test": _ran(3), "Test (Geode editor lane)": _ran(6)}),
}

_EVERYTHING = {"always": True, "browser_affected": True, "hosted_parity_affected": True,
               "hosted_parity_tests_affected": True, "self_hosted_macos": True}


class CoverageContractTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.manifest = _manifest()

    def _check(self, receipts, selection=None):
        return contract.check(self.manifest, receipts, selection or _EVERYTHING)

    def test_checked_in_manifest_is_well_formed(self):
        self.assertEqual([], contract.manifest_problems(self.manifest))
        for value, entry in (("not a bool", {"when": "always", "pr_only": "yes", "counterparts": [
                {"job": "macos-self-hosted", "step": "Build"}]}),
                             ("on an exception", {"when": "always", "pr_only": True, "exception": {
                                 "owner": "o", "decision": "d", "reason": "r"}})):
            with self.subTest(value=value):
                problems = contract.manifest_problems(
                    {"version": 1, "routes": {"macos": "always"},
                     "lanes": {"macos": {"Test": {"x": entry}}}})
                self.assertTrue(any("pr_only" in problem for problem in problems), problems)

    def test_complete_receipts_pass_and_report_every_exception(self):
        problems, excepted = self._check(_COMPLETE)
        self.assertEqual([], problems)
        # Each gap the manifest accepts is reported on every run, with its owner.
        self.assertEqual(4, len(excepted))
        for line in excepted:
            self.assertIn("owner jwmcglynn", line)
        for exception in ("hosted-equivalent", "hosted-shader-validation"):
            self.assertTrue(any(line.startswith("macos / Test / %s: " % exception)
                                for line in excepted), exception)

    def test_skipped_self_hosted_shader_validation_fails(self):
        # Main runs no shader validation, so the self-hosted real-GPU steps are the only gate.
        for step, entry in (("Require full Metal shader validation", "shader-validation"),
                            ("Metal validation preflight", "shader-memory-canary")):
            with self.subTest(step=step):
                receipts = copy.deepcopy(_COMPLETE)
                receipts["macos-self-hosted"]["steps"][step] = _SKIPPED
                problems, _ = self._check(receipts)
                self.assertEqual(problems, [
                    "macos / Test / %s is required on this pull request but did not run; "
                    "expected an executed macos-self-hosted / %s" % (entry, step)])

    def test_replayed_skipped_hosted_linux_test_fails(self):
        # Negative control from #1889: the trusted pull request's hosted Linux job skipped
        # `Test` and ran only its Xvfb partition, while the ordinary tests ran on the self-hosted
        # ARM64 lane. Its change reached a host-tool probe test (`hosted_parity`), whose result
        # differed on the hosted x86_64 image, and main went red.
        receipts = copy.deepcopy(_COMPLETE)
        receipts["linux"]["steps"].pop("Test (hosted parity subset)")
        problems, _ = self._check(receipts)
        self.assertEqual(problems, [
            "linux / Test / hosted-parity ran on main but not on this pull request; expected an "
            "executed linux / Test (hosted parity subset)"])
        # Skipping the step fails the same way as omitting it.
        receipts["linux"]["steps"]["Test (hosted parity subset)"] = _SKIPPED
        self.assertEqual(problems, self._check(receipts)[0])

    def test_skipped_cloud_build_fails(self):
        # Main's hosted Linux Build runs with the cloud execution configuration. A trusted pull
        # request that did not build the tree that way before merge cannot claim that toolchain.
        receipts = copy.deepcopy(_COMPLETE)
        receipts["linux"]["steps"]["Verify BuildBuddy remote execution"] = _SKIPPED
        problems, _ = self._check(receipts)
        gap = ("ran on main but not on this pull request; expected an executed linux / "
               "Verify BuildBuddy remote execution")
        self.assertEqual(problems, [
            "linux / Build / hosted-toolchain " + gap,
            "linux / Verify BuildBuddy remote execution / cloud-build " + gap])

    def test_removing_a_parity_partition_fails(self):
        for step in ("Test", "Test (Geode editor lane)"):
            with self.subTest(step=step):
                receipts = copy.deepcopy(_COMPLETE)
                receipts["macos-hosted-parity"]["steps"].pop(step)
                problems, _ = self._check(receipts)
                self.assertEqual(1, len(problems), problems)
                self.assertIn("macos-hosted-parity / %s" % step, problems[0])
        # A skipped parity job (no receipt at all) fails both parity classes.
        receipts = copy.deepcopy(_COMPLETE)
        del receipts["macos-hosted-parity"]
        self.assertEqual(2, len(self._check(receipts)[0]))

    def test_removing_a_parity_counterpart_from_the_manifest_fails(self):
        manifest = copy.deepcopy(self.manifest)
        entry = manifest["lanes"]["macos"]["Test (Geode editor lane)"]["hosted-parity"]
        entry["counterparts"] = []
        self.assertEqual(
            ["macos / Test (Geode editor lane) / hosted-parity needs either counterparts or an "
             "exception, not both or neither"], contract.manifest_problems(manifest))

    def test_exceptions_must_name_an_owner(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["lanes"]["macos"]["Test"]["hosted-equivalent"]["exception"]["owner"] = " "
        self.assertEqual(["macos / Test / hosted-equivalent exception must name an owner, the "
                          "decision and a reason"], contract.manifest_problems(manifest))

    def test_a_counterpart_must_report_its_minimum_test_count(self):
        receipts = copy.deepcopy(_COMPLETE)
        receipts["macos-self-hosted"]["steps"]["Test (Geode editor lane)"] = _ran(0)
        problems, _ = self._check(receipts)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("macos / Test (Geode editor lane) / geode", problems[0])

    def test_a_failed_counterpart_still_counts_as_executed(self):
        # The failing job already fails the run; the contract asks only whether it ran.
        receipts = copy.deepcopy(_COMPLETE)
        receipts["linux-self-hosted"]["steps"]["Test"] = {"outcome": "failure"}
        self.assertEqual([], self._check(receipts)[0])

    def test_unselected_classes_need_no_counterpart(self):
        receipts = copy.deepcopy(_COMPLETE)
        del receipts["macos-hosted-parity"]
        del receipts["macos-browser"]
        receipts["linux"]["steps"].pop("Test (hosted parity subset)")
        self.assertEqual([], self._check(receipts, {"always": True, "self_hosted_macos": True})[0])
        # A //... fallback selects every class.
        self.assertEqual(5, len(self._check(receipts, {"always": True, "fallback": True,
                                                       "self_hosted_macos": True})[0]))

    def test_hosted_macos_routing_still_checks_linux(self):
        # With the ci:hosted-macos label (or the macOS runner disabled), main's hosted macOS job
        # ran on the pull request itself, so the macOS lane needs no counterparts. Linux still
        # ran on the self-hosted lane, and the #1889 control must still fail.
        receipts = {job: receipt for job, receipt in copy.deepcopy(_COMPLETE).items()
                    if job.startswith("linux")}
        selection = dict(_EVERYTHING, self_hosted_macos=False)
        problems, excepted = self._check(receipts, selection)
        self.assertEqual([], problems)
        self.assertFalse(any(line.startswith("macos /") for line in excepted))
        receipts["linux"]["steps"]["Test (hosted parity subset)"] = _SKIPPED
        self.assertEqual(1, len(self._check(receipts, selection)[0]))
        # A fallback does not change routing.
        self.assertEqual(1, len(self._check(receipts, dict(selection, fallback=True))[0]))

    def test_a_parity_step_that_ran_no_tests_does_not_count(self):
        receipts = copy.deepcopy(_COMPLETE)
        receipts["linux"]["steps"]["Test (hosted parity subset)"] = _ran(0)
        self.assertEqual(1, len(self._check(receipts)[0]))

    def test_bep_counts_separate_executed_from_cached_results(self):
        events = [
            {"id": {"testResult": {"label": "//a:t", "run": 1, "shard": 1, "attempt": 1}},
             "testResult": {"status": "PASSED"}},
            {"id": {"testSummary": {"label": "//a:t"}}, "testSummary": {"overallStatus": "PASSED"}},
            {"id": {"testResult": {"label": "//b:t", "run": 1, "shard": 1, "attempt": 1}},
             "testResult": {"status": "PASSED", "cachedLocally": True}},
            {"id": {"testSummary": {"label": "//b:t"}}, "testSummary": {"overallStatus": "PASSED"}},
            {"id": {"testResult": {"label": "//c:t", "run": 1, "shard": 1, "attempt": 1}},
             "testResult": {"status": "PASSED", "executionInfo": {"cachedRemotely": True}}},
            {"id": {"testSummary": {"label": "//c:t"}}, "testSummary": {"overallStatus": "PASSED"}},
            {"id": {"progress": {}}, "progress": {}},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.bep.json"
            path.write_text("\n".join(json.dumps(event) for event in events) + "\n",
                            encoding="utf-8")
            self.assertEqual({"tests": 3, "executed": 1}, contract.bep_counts(path))
            receipt = contract.make_receipt(
                "job", {"Test": "success", "Skipped": "skipped"},
                {"Test": str(path), "Skipped": str(path)})
        self.assertEqual(receipt["steps"], {
            "Test": {"outcome": "success", "tests": 3, "executed": 1},
            "Skipped": {"outcome": "skipped"}})

    def test_command_line_round_trip(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for job, receipt in _COMPLETE.items():
                path = root / ("coverage-receipt-%s" % job) / "coverage-receipt.json"
                path.parent.mkdir()
                path.write_text(json.dumps(receipt), encoding="utf-8")
            manifest = runfiles.Create().Rlocation("donner/tools/ci/coverage_contract.json")
            selections = ["fallback=true", "browser_affected=", "hosted_parity_affected=",
                          "hosted_parity_tests_affected=", "self_hosted_macos=true"]
            arguments = ["check", "--manifest", manifest, "--receipts", directory]
            for selection in selections:
                arguments += ["--selection", selection]
            self.assertEqual(0, contract.main(arguments))
            # An unknown or a missing selection is an error, never a silent false.
            self.assertEqual(2, contract.main(arguments + ["--selection", "typo=true"]))
            self.assertEqual(2, contract.main(arguments[:-2]))
            (root / "coverage-receipt-linux" / "coverage-receipt.json").unlink()
            self.assertEqual(1, contract.main(arguments))


if __name__ == "__main__":
    unittest.main()
