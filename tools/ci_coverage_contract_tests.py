"""Tests for tools/ci_coverage_contract.py and the checked-in coverage manifest."""

import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

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
        "buildbuddy_verify": _ran(), "build": _SKIPPED, "test": _SKIPPED, "local_xvfb": _ran(),
        "deterministic_soak_probe": _ran(1),
        "hosted_parity_test": _ran(1),
        "linker_canary": _ran(),
        "cmake_consumer": _ran()}),
    "linux-self-hosted": _receipt("linux-self-hosted", {"build": _ran(), "test": _ran()}),
    "macos-self-hosted": _receipt("macos-self-hosted", {
        "build": _ran(), "test": _ran(), "geode_test": _ran(6),
        "browser_preflight": _ran(), "browser_test": _ran(2)}),
    "macos-metal": _receipt("macos-metal", {
        "metal_profile": _ran(), "metal_preflight": _ran(), "metal_native": _ran(10)}),
    "macos-browser": _receipt("macos-browser", {
        "browser_preflight": _ran(), "browser_test": _ran(2)}),
    "macos-hosted-parity": _receipt("macos-hosted-parity", {
        "test": _ran(3), "geode_test": _ran(6)}),
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

    def test_deterministic_soak_probe_cannot_be_skipped_or_empty(self):
        for step in (_SKIPPED, _ran(0)):
            with self.subTest(step=step):
                receipts = copy.deepcopy(_COMPLETE)
                receipts["linux"]["steps"]["deterministic_soak_probe"] = step
                problems, _ = self._check(receipts)
                self.assertIn(
                    "linux / Test deterministic fuzzer soak wrapper / deterministic-wrapper",
                    "\n".join(problems))

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
        for step, entry in (("metal_profile", "shader-validation"),
                            ("metal_preflight", "shader-memory-canary")):
            with self.subTest(step=step):
                receipts = copy.deepcopy(_COMPLETE)
                receipts["macos-metal"]["steps"][step] = _SKIPPED
                problems, _ = self._check(receipts)
                self.assertEqual(problems, [
                    "macos / Test / %s is required on this pull request but did not run; "
                    "expected an executed macos-metal / %s" % (entry, step)])

    def test_native_metal_suite_cannot_be_skipped_or_empty(self):
        for result in (_SKIPPED, _ran(0)):
            with self.subTest(result=result):
                receipts = copy.deepcopy(_COMPLETE)
                receipts["macos-metal"]["steps"]["metal_native"] = result
                problems, _ = self._check(receipts)
                self.assertTrue(any("native-metal" in problem for problem in problems), problems)

    def test_replayed_skipped_hosted_linux_test_fails(self):
        # Negative control from #1889: the trusted pull request's hosted Linux job skipped
        # `Test` and ran only its Xvfb partition, while the ordinary tests ran on the self-hosted
        # ARM64 lane. Its change reached a host-tool probe test (`hosted_parity`), whose result
        # differed on the hosted x86_64 image, and main went red.
        receipts = copy.deepcopy(_COMPLETE)
        receipts["linux"]["steps"].pop("hosted_parity_test")
        problems, _ = self._check(receipts)
        self.assertEqual(problems, [
            "linux / Test / hosted-parity ran on main but not on this pull request; expected an "
            "executed linux / hosted_parity_test"])
        # Skipping the step fails the same way as omitting it.
        receipts["linux"]["steps"]["hosted_parity_test"] = _SKIPPED
        self.assertEqual(problems, self._check(receipts)[0])

    def test_skipped_cloud_build_fails(self):
        # Main's hosted Linux Build runs with the cloud execution configuration. A trusted pull
        # request that did not build the tree that way before merge cannot claim that toolchain.
        receipts = copy.deepcopy(_COMPLETE)
        receipts["linux"]["steps"]["buildbuddy_verify"] = _SKIPPED
        problems, _ = self._check(receipts)
        gap = ("ran on main but not on this pull request; expected an executed linux / "
               "buildbuddy_verify")
        self.assertEqual(problems, [
            "linux / Build / hosted-toolchain " + gap,
            "linux / Verify BuildBuddy remote execution / cloud-build " + gap])

    def test_each_browser_partition_requires_its_own_receipt(self):
        for job in ("macos-self-hosted", "macos-browser"):
            for step in ("browser_preflight", "browser_test"):
                with self.subTest(job=job, step=step):
                    receipts = copy.deepcopy(_COMPLETE)
                    receipts[job]["steps"][step] = _SKIPPED
                    problems, _ = self._check(receipts)
                    self.assertEqual(1, len(problems), problems)
                    self.assertIn(job + " / " + step, problems[0])

    def test_removing_a_parity_partition_fails(self):
        for step in ("test", "geode_test"):
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
        receipts["macos-self-hosted"]["steps"]["geode_test"] = _ran(0)
        problems, _ = self._check(receipts)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("macos / Test (Geode editor lane) / geode", problems[0])

    def test_a_failed_counterpart_still_counts_as_executed(self):
        # The failing job already fails the run; the contract asks only whether it ran.
        receipts = copy.deepcopy(_COMPLETE)
        receipts["linux-self-hosted"]["steps"]["test"] = {"outcome": "failure"}
        self.assertEqual([], self._check(receipts)[0])

    def test_unselected_classes_need_no_counterpart(self):
        receipts = copy.deepcopy(_COMPLETE)
        del receipts["macos-hosted-parity"]
        del receipts["macos-browser"]
        receipts["linux"]["steps"].pop("hosted_parity_test")
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
        receipts["linux"]["steps"]["hosted_parity_test"] = _SKIPPED
        self.assertEqual(1, len(self._check(receipts, selection)[0]))
        # A fallback does not change routing.
        self.assertEqual(1, len(self._check(receipts, dict(selection, fallback=True))[0]))

    def test_a_parity_step_that_ran_no_tests_does_not_count(self):
        receipts = copy.deepcopy(_COMPLETE)
        receipts["linux"]["steps"]["hosted_parity_test"] = _ran(0)
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
            path = Path(directory) / "coverage-test.bep.json"
            path.write_text("\n".join(json.dumps(event) for event in events) + "\n",
                            encoding="utf-8")
            self.assertEqual({"tests": 3, "executed": 1}, contract.bep_counts(path))
            # A step's build event file is found by its id; a skipped step reports no counts.
            (Path(directory) / "coverage-skipped.bep.json").write_text("", encoding="utf-8")
            receipt = contract.make_receipt(
                "job", {"test": {"outcome": "success", "conclusion": "success"},
                        "skipped": {"outcome": "skipped"}, "other": {"outcome": "success"}},
                directory)
        self.assertEqual(receipt["steps"], {
            "test": {"outcome": "success", "tests": 3, "executed": 1},
            "skipped": {"outcome": "skipped"}, "other": {"outcome": "success"}})

    def test_selection_comes_from_the_needs_context(self):
        needs = {"determine-targets": {"result": "success", "outputs": {
                     "fallback": "false", "browser_affected": "true",
                     "hosted_parity_affected": "false"}},
                 "gatekeeper": {"outputs": {"use_self_hosted_macos": "true"}}}
        self.assertEqual(contract.selection_from_needs(needs), {
            "always": True, "fallback": False, "browser_affected": True,
            "hosted_parity_affected": False, "hosted_parity_tests_affected": False,
            "self_hosted_macos": True})
        # A skipped job sets no outputs, which read as false everywhere but `always`.
        skipped = {job: {"result": "skipped", "outputs": {}}
                   for job in ("determine-targets", "gatekeeper")}
        self.assertEqual({"always"}, {name for name, value in
                                      contract.selection_from_needs(skipped).items() if value})
        # A source job missing from `needs` is an error, never a silent false.
        for job in ("determine-targets", "gatekeeper"):
            with self.subTest(missing=job):
                partial = {other: value for other, value in skipped.items() if other != job}
                with self.assertRaisesRegex(ValueError, job):
                    contract.selection_from_needs(partial)

    def test_every_selection_has_one_source(self):
        # Each condition and route the manifest can name, apart from `always`, is read from
        # exactly one job output, plus the //... fallback.
        self.assertEqual(set(contract.SELECTION_SOURCES),
                         (set(contract.CONDITIONS) | set(contract.ROUTES) | {"fallback"})
                         - {"always"})

    def test_command_line_round_trip(self):
        steps = {"build": {"outcome": "success"}, "test": {"outcome": "success"},
                 "geode_test": {"outcome": "skipped"}}
        needs = {"determine-targets": {"outputs": {"fallback": "true"}},
                 "gatekeeper": {"outputs": {"use_self_hosted_macos": "true"}}}
        with tempfile.TemporaryDirectory() as directory, mock.patch.dict(
                os.environ, {"RUNNER_TEMP": directory, "STEPS_JSON": json.dumps(steps),
                             "NEEDS_JSON": json.dumps(needs)}):
            self.assertEqual(0, contract.main(["receipt", "macos-self-hosted"]))
            written = json.loads((Path(directory) / "coverage-receipt" /
                                  "coverage-receipt.json").read_text(encoding="utf-8"))
            self.assertEqual("macos-self-hosted", written["job"])
            self.assertEqual("skipped", written["steps"]["geode_test"]["outcome"])
            receipts = Path(directory) / "coverage-receipts"
            for job, receipt in _COMPLETE.items():
                path = receipts / ("coverage-receipt-%s" % job) / "coverage-receipt.json"
                path.parent.mkdir(parents=True)
                path.write_text(json.dumps(receipt), encoding="utf-8")
            manifest = runfiles.Create().Rlocation("donner/tools/ci/coverage_contract.json")
            self.assertEqual(0, contract.main(["check", "--manifest", manifest]))
            (receipts / "coverage-receipt-linux" / "coverage-receipt.json").unlink()
            self.assertEqual(1, contract.main(["check", "--manifest", manifest]))


if __name__ == "__main__":
    unittest.main()
