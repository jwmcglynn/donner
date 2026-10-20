#!/usr/bin/env python3
"""Tests for ci_runner_loss_rerun.py.

The annotation messages and step states below are copied from real hosted macOS jobs of this
repository's main CI, so the signatures are pinned to what the runner actually reports.
"""

import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import urllib.error

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ci_runner_loss_rerun as rerun

REPOSITORY = "example/donner"

LOST = ("The hosted runner lost communication with the server. Anything in your workflow that "
        "terminates the runner process, starves it for CPU/Memory, or blocks its network access "
        "can cause this error.")
CAPACITY = ("Due to capacity constraints, jobs targeting macOS arm64 runners may experience "
            "longer queue times.")
METAL = ("Excluding requires_metal_toolchain targets: the hosted macOS image has no offline Metal "
         "compiler.")
SHADER_VALIDATION = ("Excluding requires_shader_validation targets: the hosted macOS image's "
                     "virtual GPU cannot run Metal shader validation.")
DIAG_PAGE = (
    "System.IO.IOException: Illegal byte sequence : '/Users/runner/actions-runner/extracted/_diag/"
    "pages/3907064f-31bb-8f13-bafa-25873b53f7d2_17cdf819-5d70-46a9-bcad-b728863fed71_2.log'\n"
    "   at Interop.ThrowExceptionForIoErrno(ErrorInfo errorInfo, String path, Boolean isDirError)\n"
    "   at GitHub.Runner.Common.PagingLogger.NewPage()")
TEMP_SCRIPT = ("Illegal byte sequence : "
               "'/Users/runner/work/_temp/a1f9abc5-0c5e-439d-84b2-4d3fd26ee32c.sh'")


def annotation(message, level="failure"):
    return {"path": ".github", "annotation_level": level, "title": "", "message": message,
            "raw_details": ""}


def steps(*pairs):
    """Steps in order from (name, conclusion) pairs.

    "pending" is a step never reached, and "in_progress" is the step running when the runner
    died; neither has a conclusion.
    """
    result = []
    for number, (name, conclusion) in enumerate(pairs, start=1):
        unfinished = conclusion in ("pending", "in_progress")
        result.append({"number": number, "name": name,
                       "status": conclusion if unfinished else "completed",
                       "conclusion": None if unfinished else conclusion})
    return result


def job(job_id, name, conclusion, job_steps=()):
    return {"id": job_id, "name": name, "status": "completed", "conclusion": conclusion,
            "run_attempt": 1, "steps": list(job_steps)}


def run(run_id, **overrides):
    value = {"id": run_id, "name": "CI", "path": ".github/workflows/main.yml",
             "head_branch": "main", "event": "push", "status": "completed",
             "conclusion": "failure", "run_attempt": 1,
             "repository": {"full_name": REPOSITORY},
             "head_repository": {"full_name": REPOSITORY}}
    value.update(overrides)
    return value


PASSING = [job(117788505412, "gatekeeper", "success"),
           job(117788598000, "linux", "success"),
           job(117788703982, "macos-browser", "skipped")]

# Run 39165844514: the runner died during Build; later steps never ran.
LOST_IN_BUILD = job(117788702573, "macos", "failure", steps(
    ("Checkout", "success"),
    ("Configure BuildBuddy read-only cache", "skipped"),
    ("Build", "pending"),
    ("Test", "pending"),
    ("Post Restore Bazel action cache", "pending"),
))
LOST_IN_BUILD_ANNOTATIONS = [annotation(LOST), annotation(CAPACITY, "notice")]

# Run 39165844514 attempt 2: the runner died while restoring the action cache, so that step
# never got a conclusion and every later step stayed pending. The step layout is the one before
# the action cache moved its save into a final "Save Bazel action cache" step.
LOST_IN_CACHE_RESTORE = job(117866907732, "macos", "failure", steps(
    ("Set up job", "success"),
    ("Checkout", "success"),
    ("Setup Bazel", "success"),
    ("Restore Bazel action cache", "in_progress"),
    ("Build", "pending"),
    ("Test", "pending"),
    ("Post Restore Bazel action cache", "pending"),
))
LOST_IN_CACHE_RESTORE_ANNOTATIONS = [annotation(LOST), annotation(CAPACITY, "notice")]

# Run 38696609859: the runner's own log page write failed in post-job cache save.
IO_IN_POST = job(116294844879, "macos", "failure", steps(
    ("Build", "success"),
    ("Test", "success"),
    ("Post Restore Bazel action cache", "failure"),
    ("Post Setup Bazel", "pending"),
))
IO_IN_POST_ANNOTATIONS = [annotation(DIAG_PAGE), annotation(CAPACITY, "notice")]

# Run 39165828508: Test failed first, with exit code 1, because Bazel's disk cache hit
# `Illegal byte sequence` I/O errors and a target failed to build; the browser step then failed
# with exit code 2. Only after both did a step script under _temp become unwritable. The
# workload failed on its own exit codes, so this is not a runner loss to rerun; bounding the
# disk cache is what addresses it.
IO_IN_TEST = job(117788674812, "macos", "failure", steps(
    ("Build", "success"),
    ("Test", "failure"),
    ("Qualify pinned Chromium archive", "failure"),
    ("Test (browser targets)", "failure"),
))
IO_IN_TEST_ANNOTATIONS = [annotation(TEMP_SCRIPT),
                          annotation("Process completed with exit code 2."),
                          annotation("Process completed with exit code 1."),
                          annotation(METAL, "notice"), annotation(SHADER_VALIDATION, "notice"),
                          annotation(CAPACITY, "notice")]

# The same runner log page write failure once the action cache saves in its own final step
# rather than in a post step.
IO_IN_CACHE_SAVE = job(116294844880, "macos", "failure", steps(
    ("Build", "success"),
    ("Test", "success"),
    ("Save Bazel action cache", "failure"),
    ("Post Setup Bazel", "pending"),
))

# Run 38671104124: the test steps had already failed when the runner was lost.
LOST_AFTER_FAILED_TEST = job(116214414886, "macos", "failure", steps(
    ("Build", "success"),
    ("Test", "failure"),
    ("Test (browser targets)", "failure"),
    ("Test (Geode editor lane)", "failure"),
    ("Post Checkout", "success"),
    ("Complete job", "pending"),
))
LOST_AFTER_FAILED_TEST_ANNOTATIONS = [annotation(LOST), annotation(CAPACITY, "notice")]

# Run 39124107325's Linux job: an ordinary Bazel test failure.
LINUX_TEST_FAILURE = job(117665700108, "linux", "failure", steps(
    ("Build", "success"),
    ("Test", "failure"),
))
LINUX_TEST_FAILURE_ANNOTATIONS = [annotation("Process completed with exit code 3.")]


def classify(run_value, jobs, annotations):
    return rerun.classify_run(run_value, jobs, lambda job_id: annotations[job_id])


class ClassifierTests(unittest.TestCase):
    def test_signature_a_lost_runner_without_failed_step_is_eligible(self):
        decision = classify(run(39165844514), PASSING + [LOST_IN_BUILD],
                            {117788702573: LOST_IN_BUILD_ANNOTATIONS})

        self.assertTrue(decision.eligible)
        self.assertEqual([rerun.SIGNATURE_LOST], [job.signature for job in decision.jobs])

    def test_signature_a_matches_a_step_left_in_progress_by_the_lost_runner(self):
        decision = rerun.classify_job(LOST_IN_CACHE_RESTORE, LOST_IN_CACHE_RESTORE_ANNOTATIONS)

        self.assertEqual(rerun.SIGNATURE_LOST, decision.signature)
        # The same loss on a second attempt is still never rerun.
        run_decision = classify(run(39165844514, run_attempt=2), [LOST_IN_CACHE_RESTORE],
                                {117866907732: LOST_IN_CACHE_RESTORE_ANNOTATIONS})
        self.assertFalse(run_decision.eligible)
        self.assertEqual(["attempt 2 is not the first"], run_decision.gate_failures)

    def test_signature_b_runner_diag_write_failure_is_eligible(self):
        decision = classify(run(38696609859), PASSING + [IO_IN_POST],
                            {116294844879: IO_IN_POST_ANNOTATIONS})

        self.assertTrue(decision.eligible)
        self.assertEqual([rerun.SIGNATURE_IO], [job.signature for job in decision.jobs])

    def test_signature_b_runner_script_write_failure_in_cleanup_is_eligible(self):
        decision = classify(run(39000000012), PASSING + [IO_IN_POST],
                            {116294844879: [annotation(TEMP_SCRIPT)]})

        self.assertTrue(decision.eligible)
        self.assertEqual([rerun.SIGNATURE_IO], [job.signature for job in decision.jobs])

    def test_script_write_failure_after_failed_workload_steps_is_not_rerun(self):
        decision = classify(run(39165828508), PASSING + [IO_IN_TEST],
                            {117788674812: IO_IN_TEST_ANNOTATIONS})

        self.assertFalse(decision.eligible)
        self.assertEqual("runner I/O error after failed workload step(s): Test, Qualify pinned "
                         "Chromium archive, Test (browser targets)", decision.jobs[0].reason)

    def test_real_test_failure_then_a_script_write_failure_is_not_rerun(self):
        failed_test = job(117788674813, "macos", "failure", steps(
            ("Build", "success"),
            ("Test", "failure"),
            ("Post Setup Bazel", "failure"),
        ))
        decision = classify(run(39000000013), PASSING + [failed_test], {117788674813: [
            annotation("Process completed with exit code 3."), annotation(TEMP_SCRIPT)]})

        self.assertFalse(decision.eligible)
        self.assertIn("failed workload step(s): Test", decision.jobs[0].reason)

    def test_script_write_failure_beside_a_failed_step_without_exit_code_is_not_rerun(self):
        # The annotation does not say which step the write failure hit, so it cannot show that
        # the failed workload step was its casualty rather than a failure of its own.
        decision = classify(run(39000000015), PASSING + [IO_IN_TEST],
                            {117788674812: [annotation(TEMP_SCRIPT)]})

        self.assertFalse(decision.eligible)
        self.assertIn("failed workload step(s): Test", decision.jobs[0].reason)

    def test_script_write_failure_with_an_exit_code_is_not_rerun(self):
        decision = classify(run(39000000014), [IO_IN_POST], {116294844879: [
            annotation(TEMP_SCRIPT), annotation("Process completed with exit code 1.")]})

        self.assertFalse(decision.eligible)
        self.assertEqual("a step exit-code failure accompanies the runner I/O error",
                         decision.jobs[0].reason)

    def test_signature_b_runner_diag_write_failure_in_the_cache_save_is_eligible(self):
        decision = classify(run(39000000010), PASSING + [IO_IN_CACHE_SAVE],
                            {116294844880: IO_IN_POST_ANNOTATIONS})

        self.assertTrue(decision.eligible)
        self.assertEqual([rerun.SIGNATURE_IO], [job.signature for job in decision.jobs])

    def test_cache_save_failure_with_an_exit_code_is_not_rerun(self):
        # The save step exited non-zero on its own; the log write failure does not explain it.
        decision = classify(run(39000000011), [IO_IN_CACHE_SAVE], {116294844880: [
            annotation(DIAG_PAGE), annotation("Process completed with exit code 1.")]})

        self.assertFalse(decision.eligible)
        self.assertIn("Save Bazel action cache", decision.jobs[0].reason)

    def test_runner_log_write_failure_after_a_failed_test_is_not_rerun(self):
        # A test that genuinely fails before the runner's own log write breaks must stay red.
        failed_test = job(116294844879, "macos", "failure", steps(
            ("Build", "success"),
            ("Test", "failure"),
            ("Post Restore Bazel action cache", "failure"),
        ))
        decision = classify(run(38696609859), [failed_test], {116294844879: [
            annotation("Process completed with exit code 3."), annotation(DIAG_PAGE),
            annotation(CAPACITY, "notice")]})

        self.assertFalse(decision.eligible)
        self.assertEqual("runner I/O error after failed workload step(s): Test",
                         decision.jobs[0].reason)

    def test_runner_loss_beside_a_real_test_failure_is_not_rerun(self):
        decision = classify(run(39124107325), [LOST_IN_BUILD, LINUX_TEST_FAILURE], {
            117788702573: LOST_IN_BUILD_ANNOTATIONS,
            117665700108: LINUX_TEST_FAILURE_ANNOTATIONS})

        self.assertFalse(decision.eligible)
        self.assertEqual([True, False], [job.eligible for job in decision.jobs])
        self.assertEqual("no runner-loss annotation", decision.jobs[1].reason)

    def test_runner_io_failure_beside_a_real_test_failure_is_not_rerun(self):
        decision = classify(run(39124107325), [IO_IN_POST, LINUX_TEST_FAILURE], {
            116294844879: IO_IN_POST_ANNOTATIONS,
            117665700108: LINUX_TEST_FAILURE_ANNOTATIONS})

        self.assertFalse(decision.eligible)
        self.assertEqual([True, False], [job.eligible for job in decision.jobs])

    def test_runner_lost_after_a_failed_test_step_is_not_rerun(self):
        decision = classify(run(38671104124), PASSING + [LOST_AFTER_FAILED_TEST],
                            {116214414886: LOST_AFTER_FAILED_TEST_ANNOTATIONS})

        self.assertFalse(decision.eligible)
        self.assertIsNone(decision.jobs[0].signature)
        self.assertIn("Test, Test (browser targets)", decision.jobs[0].reason)

    def test_benign_notices_with_an_exit_failure_are_not_rerun(self):
        failed = job(117000000001, "macos", "failure", steps(
            ("Build", "success"),
            ("Test", "failure"),
        ))
        decision = classify(run(39000000001), PASSING + [failed], {117000000001: [
            annotation("Process completed with exit code 3."), annotation(METAL, "notice"),
            annotation(CAPACITY, "notice")]})

        self.assertFalse(decision.eligible)
        self.assertEqual("no runner-loss annotation", decision.jobs[0].reason)

    def test_benign_notices_alone_are_not_a_runner_loss(self):
        failed = job(117000000002, "macos", "failure", steps(("Build", "failure")))
        decision = classify(run(39000000002), [failed], {117000000002: [
            annotation(CAPACITY, "notice")]})

        self.assertFalse(decision.eligible)

    def test_lost_runner_with_an_exit_code_annotation_is_not_rerun(self):
        decision = classify(run(39000000003), [LOST_IN_BUILD], {117788702573: [
            annotation(LOST), annotation("Process completed with exit code 1.")]})

        self.assertFalse(decision.eligible)

    def test_unknown_annotation_beside_a_signature_is_not_rerun(self):
        for extra in (annotation("Failed to CreateArtifact: Unable to make request: ENOTFOUND"),
                      annotation("No files were found with the provided path: out.", "warning"),
                      annotation("Some other notice.", "notice"),
                      annotation("The operation was canceled.")):
            with self.subTest(extra=extra["message"]):
                decision = classify(run(39000000004), [IO_IN_POST], {
                    116294844879: IO_IN_POST_ANNOTATIONS + [extra]})

                self.assertFalse(decision.eligible)
                self.assertIn("unrecognized annotation", decision.jobs[0].reason)

    def test_io_signature_requires_a_runner_internal_path(self):
        for message in (
                "Illegal byte sequence : '/Users/runner/work/donner/donner/out.txt'",
                "Illegal byte sequence : '/Users/runner/work/_temp/../donner/out.txt'",
                "Write failed: Illegal byte sequence : '/Users/runner/work/_temp/x.sh'",
                "System.IO.IOException: Illegal byte sequence : '/tmp/_diag/x.log'"):
            with self.subTest(message=message):
                decision = classify(run(39000000005), [IO_IN_POST], {
                    116294844879: [annotation(message)]})

                self.assertFalse(decision.eligible)
                self.assertIn("unrecognized annotation", decision.jobs[0].reason)

    def test_cancelled_and_timed_out_jobs_are_not_runner_losses(self):
        for conclusion in ("cancelled", "timed_out", None):
            with self.subTest(conclusion=conclusion):
                lost = dict(LOST_IN_BUILD, conclusion=conclusion)
                decision = classify(run(39000000006), [lost], {
                    117788702573: LOST_IN_BUILD_ANNOTATIONS})

                self.assertFalse(decision.eligible)

    def test_lost_runner_after_a_cancelled_step_is_not_rerun(self):
        lost = job(117788702573, "macos", "failure", steps(
            ("Build", "cancelled"),
            ("Test", "pending"),
        ))
        decision = classify(run(39000000007), [lost], {117788702573: LOST_IN_BUILD_ANNOTATIONS})

        self.assertFalse(decision.eligible)

    def test_run_gate_rejects_reruns_branches_events_and_other_workflows(self):
        cases = {
            "attempt 2": {"run_attempt": 2},
            "pull request branch": {"head_branch": "fix/selectors-level4-conformance"},
            "pull request event": {"event": "pull_request"},
            "other workflow": {"path": ".github/workflows/perf.yml"},
            "success": {"conclusion": "success"},
            "in progress": {"status": "in_progress"},
            "fork head": {"head_repository": {"full_name": "someone/donner"}},
            "missing head": {"head_repository": None},
        }
        for label, overrides in cases.items():
            with self.subTest(label):
                decision = classify(run(39165844514, **overrides), [LOST_IN_BUILD],
                                    {117788702573: LOST_IN_BUILD_ANNOTATIONS})

                self.assertFalse(decision.eligible)
                self.assertTrue(decision.gate_failures)
                self.assertTrue(decision.jobs[0].eligible)

    def test_editor_wasm_runs_are_in_scope(self):
        decision = classify(run(39165844515, path=".github/workflows/editor_wasm.yml"),
                            [LOST_IN_BUILD], {117788702573: LOST_IN_BUILD_ANNOTATIONS})

        self.assertTrue(decision.eligible)

    def test_a_failed_run_without_a_failed_job_is_not_rerun(self):
        decision = classify(run(39000000008), PASSING, {})

        self.assertFalse(decision.eligible)
        self.assertIn("no failed job to classify", decision.gate_failures)


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class FakeGitHub:
    """Serves canned REST responses and records every request."""

    def __init__(self, responses):
        self.responses = responses
        self.requests = []

    def __call__(self, request, timeout):
        self.requests.append((request.get_method(), request.full_url))
        path = request.full_url.split("/repos/%s" % REPOSITORY, 1)[1]
        value = self.responses.get((request.get_method(), path))
        if isinstance(value, Exception):
            raise value
        if value is None:
            raise urllib.error.HTTPError(request.full_url, 404, "Not Found", {}, None)
        return FakeResponse(b"" if value == "" else json.dumps(value).encode())

    @property
    def posts(self):
        return [url for method, url in self.requests if method == "POST"]


def responses_for(run_value, jobs, annotations):
    run_id = run_value["id"]
    responses = {
        ("GET", "/actions/runs/%d" % run_id): run_value,
        ("GET", "/actions/runs/%d/attempts/1/jobs?per_page=100&page=1" % run_id):
            {"total_count": len(jobs), "jobs": jobs},
        ("POST", "/actions/runs/%d/rerun-failed-jobs" % run_id): "",
    }
    for job_id, found in annotations.items():
        responses[("GET", annotations_path(job_id, 1))] = found
        responses[("GET", annotations_path(job_id, 2))] = []
    return responses


def annotations_path(job_id, page):
    return "/check-runs/%d/annotations?per_page=100&page=%d" % (job_id, page)


def jobs_path(run_id, page):
    return "/actions/runs/%d/attempts/1/jobs?per_page=100&page=%d" % (run_id, page)


class MainTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.summary = Path(directory.name) / "summary.md"
        self.environ = {"GITHUB_REPOSITORY": REPOSITORY, "GH_TOKEN": "token",
                        "GITHUB_API_URL": "https://api.example.test",
                        "GITHUB_STEP_SUMMARY": str(self.summary)}

    def invoke(self, fake, run_id, mode="rerun"):
        stdout = io.StringIO()
        original, sys.stdout = sys.stdout, stdout
        try:
            status = rerun.main(["--run-id", str(run_id), "--mode", mode], self.environ, fake)
        finally:
            sys.stdout = original
        return status

    def test_eligible_run_is_rerun_once_and_summarized(self):
        fake = FakeGitHub(responses_for(run(39165844514), PASSING + [LOST_IN_BUILD],
                                        {117788702573: LOST_IN_BUILD_ANNOTATIONS}))

        self.assertEqual(0, self.invoke(fake, 39165844514))

        self.assertEqual(["https://api.example.test/repos/%s/actions/runs/39165844514/"
                          "rerun-failed-jobs" % REPOSITORY], fake.posts)
        summary = self.summary.read_text(encoding="utf-8")
        self.assertIn("rerun of failed jobs requested", summary)
        self.assertIn("| macos (117788702573) | failure | A: runner lost communication | "
                      "eligible |", summary)

    def test_dry_run_reports_eligibility_without_rerunning(self):
        fake = FakeGitHub(responses_for(run(38696609859), PASSING + [IO_IN_POST],
                                        {116294844879: IO_IN_POST_ANNOTATIONS}))

        self.assertEqual(0, self.invoke(fake, 38696609859, mode="dry-run"))

        self.assertEqual([], fake.posts)
        summary = self.summary.read_text(encoding="utf-8")
        self.assertIn("eligible; dry run, no rerun requested", summary)
        self.assertIn("B: runner I/O Illegal byte sequence", summary)

    def test_mixed_run_is_not_rerun(self):
        fake = FakeGitHub(responses_for(run(39124107325), [LOST_IN_BUILD, LINUX_TEST_FAILURE], {
            117788702573: LOST_IN_BUILD_ANNOTATIONS,
            117665700108: LINUX_TEST_FAILURE_ANNOTATIONS}))

        self.assertEqual(0, self.invoke(fake, 39124107325))

        self.assertEqual([], fake.posts)
        summary = self.summary.read_text(encoding="utf-8")
        self.assertIn("| linux (117665700108) | failure | no runner-loss annotation | "
                      "not eligible |", summary)

    def test_second_attempt_is_not_rerun(self):
        fake = FakeGitHub(responses_for(run(39165844514, run_attempt=2), [LOST_IN_BUILD],
                                        {117788702573: LOST_IN_BUILD_ANNOTATIONS}))

        self.assertEqual(0, self.invoke(fake, 39165844514))

        self.assertEqual([], fake.posts)
        self.assertIn("attempt 2 is not the first", self.summary.read_text(encoding="utf-8"))

    def test_attempt_started_during_classification_is_not_rerun(self):
        self.assert_recheck_blocks_rerun(run(39165844514, run_attempt=2, status="queued"), 0)

    def test_recheck_naming_another_run_is_not_rerun(self):
        self.assert_recheck_blocks_rerun(run(1), 1)

    def assert_recheck_blocks_rerun(self, second, expected_status):
        responses = responses_for(run(39165844514), [LOST_IN_BUILD],
                                  {117788702573: LOST_IN_BUILD_ANNOTATIONS})
        first = run(39165844514)
        fake = FakeGitHub(responses)
        served = iter([first, second])
        original_call = fake.__call__

        def call(request, timeout):
            if request.full_url.endswith("/actions/runs/39165844514"):
                fake.responses[("GET", "/actions/runs/39165844514")] = next(served)
            return original_call(request, timeout)

        self.assertEqual(expected_status, self.invoke(call, 39165844514))

        self.assertEqual([], fake.posts)
        self.assertEqual(2, sum(1 for method, url in fake.requests
                                if url.endswith("/actions/runs/39165844514")))

    def test_api_errors_fail_closed_without_rerunning(self):
        failures = {
            "run": ("GET", "/actions/runs/39165844514"),
            "jobs": ("GET", "/actions/runs/39165844514/attempts/1/jobs?per_page=100&page=1"),
            "annotations": ("GET", "/check-runs/117788702573/annotations?per_page=100&page=1"),
        }
        for label, key in failures.items():
            with self.subTest(label):
                responses = responses_for(run(39165844514), [LOST_IN_BUILD],
                                          {117788702573: LOST_IN_BUILD_ANNOTATIONS})
                responses[key] = urllib.error.HTTPError("url", 502, "Bad Gateway", {}, None)
                fake = FakeGitHub(responses)

                self.assertEqual(1, self.invoke(fake, 39165844514))

                self.assertEqual([], fake.posts)
                self.assertIn("no rerun (failed closed)",
                              self.summary.read_text(encoding="utf-8"))

    def test_malformed_responses_fail_closed(self):
        cases = {
            "jobs not a list": {"total_count": 1, "jobs": "macos"},
            "truncated jobs": {"total_count": 3, "jobs": [LOST_IN_BUILD]},
            "job not an object": {"total_count": 1, "jobs": ["macos"]},
        }
        for label, jobs_response in cases.items():
            with self.subTest(label):
                responses = responses_for(run(39165844514), [LOST_IN_BUILD],
                                          {117788702573: LOST_IN_BUILD_ANNOTATIONS})
                responses[("GET", jobs_path(39165844514, 1))] = jobs_response
                responses[("GET", jobs_path(39165844514, 2))] = {"total_count": 3, "jobs": []}
                fake = FakeGitHub(responses)

                self.assertEqual(1, self.invoke(fake, 39165844514))

                self.assertEqual([], fake.posts)

    def test_malformed_nested_fields_fail_closed(self):
        cases = {
            "steps not a list": ("job", dict(LOST_IN_BUILD, steps="Build")),
            "step not an object": ("job", dict(LOST_IN_BUILD, steps=["Build"])),
            "head repository not an object": ("run", run(39165844514, head_repository="x")),
        }
        for label, (kind, value) in cases.items():
            with self.subTest(label):
                jobs = [value] if kind == "job" else [LOST_IN_BUILD]
                run_value = value if kind == "run" else run(39165844514)
                fake = FakeGitHub(responses_for(run_value, jobs,
                                                {117788702573: LOST_IN_BUILD_ANNOTATIONS}))

                self.assertEqual(1, self.invoke(fake, 39165844514))

                self.assertEqual([], fake.posts)
                self.assertIn("failed closed", self.summary.read_text(encoding="utf-8"))

    def test_jobs_are_read_across_pages_and_counted(self):
        responses = responses_for(run(39165844514), [], {
            117788702573: LOST_IN_BUILD_ANNOTATIONS, 117665700108: LINUX_TEST_FAILURE_ANNOTATIONS})
        responses[("GET", jobs_path(39165844514, 1))] = {"total_count": 2, "jobs": [LOST_IN_BUILD]}
        responses[("GET", jobs_path(39165844514, 2))] = {"total_count": 2,
                                                         "jobs": [LINUX_TEST_FAILURE]}
        fake = FakeGitHub(responses)

        self.assertEqual(0, self.invoke(fake, 39165844514))

        self.assertEqual([], fake.posts)
        self.assertIn("linux (117665700108)", self.summary.read_text(encoding="utf-8"))

    def test_more_jobs_than_the_reported_total_fails_closed(self):
        responses = responses_for(run(39165844514), [LOST_IN_BUILD],
                                  {117788702573: LOST_IN_BUILD_ANNOTATIONS})
        responses[("GET", jobs_path(39165844514, 1))] = {
            "total_count": 1, "jobs": [LOST_IN_BUILD, LINUX_TEST_FAILURE]}
        fake = FakeGitHub(responses)

        self.assertEqual(1, self.invoke(fake, 39165844514))

        self.assertEqual([], fake.posts)

    def test_an_unknown_annotation_on_a_later_page_blocks_the_rerun(self):
        responses = responses_for(run(39165844514), [LOST_IN_BUILD],
                                  {117788702573: LOST_IN_BUILD_ANNOTATIONS})
        responses[("GET", annotations_path(117788702573, 2))] = [
            annotation("Failed to CreateArtifact: Unable to make request: ENOTFOUND")]
        responses[("GET", annotations_path(117788702573, 3))] = []
        fake = FakeGitHub(responses)

        self.assertEqual(0, self.invoke(fake, 39165844514))

        self.assertEqual([], fake.posts)
        self.assertIn("unrecognized annotation", self.summary.read_text(encoding="utf-8"))

    def test_annotations_that_are_not_a_list_fail_closed(self):
        responses = responses_for(run(39165844514), [LOST_IN_BUILD],
                                  {117788702573: LOST_IN_BUILD_ANNOTATIONS})
        responses[("GET", annotations_path(117788702573, 1))] = {"message": LOST}
        fake = FakeGitHub(responses)

        self.assertEqual(1, self.invoke(fake, 39165844514))

        self.assertEqual([], fake.posts)

    def test_failed_rerun_request_is_reported_as_unknown(self):
        responses = responses_for(run(39165844514), [LOST_IN_BUILD],
                                  {117788702573: LOST_IN_BUILD_ANNOTATIONS})
        responses[("POST", "/actions/runs/39165844514/rerun-failed-jobs")] = (
            urllib.error.HTTPError("url", 502, "Bad Gateway", {}, None))
        fake = FakeGitHub(responses)

        self.assertEqual(1, self.invoke(fake, 39165844514))

        self.assertEqual(1, len(fake.posts))
        self.assertIn("whether a rerun started is unknown",
                      self.summary.read_text(encoding="utf-8"))

    def test_mismatched_run_response_fails_closed(self):
        responses = responses_for(run(39165844514), [LOST_IN_BUILD],
                                  {117788702573: LOST_IN_BUILD_ANNOTATIONS})
        responses[("GET", "/actions/runs/39165844514")] = run(1)
        fake = FakeGitHub(responses)

        self.assertEqual(1, self.invoke(fake, 39165844514))

        self.assertEqual([], fake.posts)

    def test_rejects_non_numeric_run_ids_and_missing_context(self):
        fake = FakeGitHub({})
        for run_id in ("", "12; rm -rf /", "-5", "0", "$(id)"):
            with self.subTest(run_id=run_id):
                self.assertEqual(2, self.invoke(fake, run_id))
        for key, value in (("GITHUB_REPOSITORY", "../x"), ("GH_TOKEN", ""),
                           ("GITHUB_API_URL", "http://api.example.test")):
            with self.subTest(key=key):
                self.environ[key] = value
                self.assertEqual(2, self.invoke(fake, 39165844514))
        self.assertEqual([], fake.requests)

    def test_summary_cells_cannot_break_the_table(self):
        decision = rerun.RunDecision(run_id=1, jobs=[rerun.JobDecision(
            name="macos | evil\nrow", job_id=2, conclusion="failure", reason="a|b `c`")])

        summary = rerun.render_summary(decision, "dry-run", "no rerun")

        self.assertIn("| macos \\| evil row (2) | failure | a\\|b 'c' | not eligible |", summary)

    def test_redirects_are_refused(self):
        handler = rerun._RefuseRedirects()

        self.assertIsNone(handler.redirect_request(None, None, 302, "Found", {},
                                                   "https://elsewhere.example.test/"))


if __name__ == "__main__":
    unittest.main()
