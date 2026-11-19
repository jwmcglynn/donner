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
_COUNTS = {"darwin-build": 42, "darwin-test": 20, "metal-profile": 1, "metal-preflight": 6,
           "metal-native": 10, "metal-compiler": 1, "browser-archive": 1, "headless-browser": 5,
           "desktop-browser": 2, "geode": 6}
_COMPLETE = {"linux-self-hosted": _receipt("linux-self-hosted", {
    "build": _SKIPPED, "test": _SKIPPED,
    "unified": dict(_ran(500), deterministic_soak_executed=True, classes=_COUNTS)})}

_EVERYTHING = {"always": True, "browser_affected": True, "metal_affected": True,
               "unified_macos": True}


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
                receipts["linux-self-hosted"]["steps"]["unified"] = step
                problems, _ = self._check(receipts)
                self.assertIn(
                    "linux / Test / deterministic-wrapper",
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
        self.assertEqual(8, len(excepted))
        for line in excepted:
            self.assertIn("owner jwmcglynn", line)
        for exception in ("Build and test / hosted-equivalent", "Build and test / hosted-shader-validation",
                          "Test (Geode editor lane) / hosted-equivalent"):
            self.assertTrue(any(line.startswith("macos / %s: " % exception)
                                for line in excepted), exception)

    def test_an_intentionally_empty_selection_is_not_a_gap(self):
        receipts = copy.deepcopy(_COMPLETE)
        receipts["linux-self-hosted"]["steps"]["unified"]["classes"] = {
            "darwin-build": 0, "darwin-test": 0, "geode": 6}
        nothing = {"always": True, "unified_macos": True}
        self.assertEqual([], self._check(receipts, nothing)[0])

    def test_missing_metal_profile_preflight_or_compiler_fails(self):
        for cls, name in (("metal-profile", "shader-validation"), ("metal-preflight", "shader-memory-canary"),
                          ("metal-compiler", "offline-metal-compiler")):
            with self.subTest(cls=cls):
                receipts = copy.deepcopy(_COMPLETE)
                del receipts["linux-self-hosted"]["steps"]["unified"]["classes"][cls]
                problems, _ = self._check(receipts)
                self.assertEqual(len(problems), 1)
                self.assertIn("macos / Build and test / " + name, problems[0])

    def test_unaffected_pull_request_expects_no_native_metal_receipt(self):
        receipts = copy.deepcopy(_COMPLETE)
        counts = receipts["linux-self-hosted"]["steps"]["unified"]["classes"]
        for name in ("metal-profile", "metal-preflight", "metal-native", "metal-compiler"):
            del counts[name]
        self.assertEqual([], self._check(receipts, dict(_EVERYTHING, metal_affected=False))[0])
        for selection in (_EVERYTHING, dict(_EVERYTHING, metal_affected=False, fallback=True)):
            self.assertEqual(4, len(self._check(receipts, selection)[0]))

    def test_native_metal_suite_cannot_be_skipped_or_empty(self):
        receipts = copy.deepcopy(_COMPLETE)
        counts = receipts["linux-self-hosted"]["steps"]["unified"]["classes"]
        counts["metal-native"] = 0
        self.assertTrue(any("native-metal" in p for p in self._check(receipts)[0]))
        del counts["metal-native"]
        self.assertTrue(any("native-metal" in p for p in self._check(receipts)[0]))

    def test_x86_gaps_are_visible_without_a_hosted_receipt(self):
        problems, excepted = self._check(_COMPLETE)
        self.assertEqual([], problems)
        for phase in ("Verify BuildBuddy remote execution / cloud-executor-probe",
                      "Build / hosted-toolchain", "Test / hosted-toolchain",
                      "Linker canary (non-lld linker support) / linker"):
            with self.subTest(phase=phase):
                self.assertTrue(any(line.startswith("linux / " + phase + ": ")
                                    and "2026-10-31" in line for line in excepted), excepted)


    def test_each_browser_partition_requires_its_own_receipt(self):
        for cls, gaps in (("headless-browser", 1), ("desktop-browser", 1), ("browser-archive", 2)):
            with self.subTest(cls=cls):
                receipts = copy.deepcopy(_COMPLETE)
                del receipts["linux-self-hosted"]["steps"]["unified"]["classes"][cls]
                self.assertEqual(gaps, len(self._check(receipts)[0]))

    def test_hosted_configuration_gap_is_a_reported_exception(self):
        # No pull request lane reruns tests under main's hosted macOS configuration: main's
        # hosted job is the only place a hosted-configuration failure shows. The manifest says so
        # for each phase that runs tests, on every run, instead of expecting a counterpart.
        problems, excepted = self._check(_COMPLETE)
        self.assertEqual([], problems)
        for phase in ("Build and test", "Test (Geode editor lane)"):
            with self.subTest(phase=phase):
                self.assertEqual(1, sum(line.startswith("macos / %s / hosted-equivalent: " % phase)
                                        and "Maintainer decision 2026-11-14" in line
                                        and "main's hosted macOS job" in line
                                        for line in excepted), excepted)
        # No class names a job that no longer exists.
        jobs = {counterpart["job"] for phases in self.manifest["lanes"].values()
                for classes in phases.values() for entry in classes.values()
                for counterpart in entry.get("counterparts", [])}
        self.assertEqual(set(_COMPLETE), jobs)
        # Dropping the exception without restoring a counterpart is a manifest error.
        manifest = copy.deepcopy(self.manifest)
        del manifest["lanes"]["macos"]["Test (Geode editor lane)"]["hosted-equivalent"][
            "exception"]
        self.assertEqual(
            ["macos / Test (Geode editor lane) / hosted-equivalent needs either counterparts or "
             "an exception, not both or neither"], contract.manifest_problems(manifest))

    def test_exceptions_must_name_an_owner(self):
        manifest = copy.deepcopy(self.manifest)
        manifest["lanes"]["macos"]["Build and test"]["hosted-equivalent"]["exception"]["owner"] = " "
        self.assertEqual(["macos / Build and test / hosted-equivalent exception must name an owner, the "
                          "decision and a reason"], contract.manifest_problems(manifest))

    def test_a_counterpart_must_report_its_minimum_test_count(self):
        receipts = copy.deepcopy(_COMPLETE)
        receipts["linux-self-hosted"]["steps"]["unified"]["classes"]["geode"] = 0
        problems, _ = self._check(receipts)
        self.assertEqual(1, len(problems), problems)
        self.assertIn("macos / Test (Geode editor lane) / geode", problems[0])

    def test_a_linux_failure_still_counts_as_executed_without_certifying_darwin(self):
        receipts = {"linux-self-hosted": _receipt("linux-self-hosted", {
            "build": _ran(), "test": {"outcome":"failure", "deterministic_soak_executed":True}})}
        self.assertEqual([], self._check(receipts, {"always":True})[0])
        self.assertTrue(self._check(receipts, _EVERYTHING)[0])

    def test_unselected_classes_need_no_counterpart(self):
        receipts = copy.deepcopy(_COMPLETE)
        del receipts["linux-self-hosted"]["steps"]["unified"]["classes"]["desktop-browser"]
        self.assertEqual([], self._check(receipts, {"always": True, "unified_macos": True})[0])
        self.assertEqual(1, len(self._check(receipts, {"always": True, "fallback": True,
                                                       "unified_macos": True})[0]))

    def test_hosted_macos_routing_still_checks_linux(self):
        receipts = {"linux-self-hosted": _receipt("linux-self-hosted", {
            "build": _ran(), "test": dict(_ran(5), deterministic_soak_executed=True)})}
        selection = dict(_EVERYTHING, unified_macos=False)
        problems, excepted = self._check(receipts, selection)
        self.assertEqual([], problems)
        self.assertFalse(any(line.startswith("macos /") for line in excepted))
        receipts["linux-self-hosted"]["steps"]["test"] = _SKIPPED
        self.assertEqual(3, len(self._check(receipts, selection)[0]))
        self.assertEqual(3, len(self._check(receipts, dict(selection, fallback=True))[0]))

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
                     "fallback": "false", "browser_affected": "true", "metal_affected": "true"}},
                 "gatekeeper": {"outputs": {"unified_macos": "true"}}}
        self.assertEqual(contract.selection_from_needs(needs), {
            "always": True, "fallback": False, "browser_affected": True,
            "metal_affected": True, "unified_macos": True})
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
                 "gatekeeper": {"outputs": {"unified_macos": "true"}}}
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
            (receipts / "coverage-receipt-linux-self-hosted" / "coverage-receipt.json").unlink()
            self.assertEqual(1, contract.main(["check", "--manifest", manifest]))


if __name__ == "__main__":
    unittest.main()
