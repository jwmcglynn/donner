"""Pins the shape of the Coverage workflow and the Codecov configuration it feeds.

Coverage runs nightly and on manual dispatch, measures the complete product tree
on main, and uploads it as the project baseline. The upload is a separate job
that consumes the report artifact, so an upload failure is re-run on its own
instead of repeating the instrumented build, and it still fails the run.
Pull requests get no coverage run and Codecov must not report on them.
"""

from pathlib import Path
import re
import unittest

from python.runfiles import runfiles


def _runfile(path):
    resolver = runfiles.Create()
    located = resolver.Rlocation("donner/%s" % path)
    assert located is not None, path
    return Path(located)


def _crons(text):
    return re.findall(r'^\s*- cron: "([^"]+)"', text, re.MULTILINE)


def _minute_hour(cron):
    return tuple(cron.split()[:2])


class CoverageWorkflowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = _runfile(".github/workflows/coverage.yml").read_text(encoding="utf-8")
        cls.codecov = _runfile("codecov.yml").read_text(encoding="utf-8")

    def _job_body(self, job):
        """The YAML block for one job, up to the next job key at the same indent."""
        marker = "\n  %s:\n" % job
        self.assertIn(marker, self.text, "job %s not found" % job)
        rest = self.text.split(marker, 1)[1]
        end = re.search(r"^  [A-Za-z0-9_-]+:\s*$", rest, re.MULTILINE)
        return rest[: end.start()] if end else rest

    def _steps(self, job_body):
        """Every step body in a job, in order, whatever key the step starts with."""
        return re.split(r"^      - ", job_body, flags=re.MULTILINE)[1:]

    def _step_names(self, job_body):
        names = []
        for step in self._steps(job_body):
            match = re.search(r"(?m)^(?:        )?(?:name|uses): (.+)$", step)
            names.append(match.group(1) if match else None)
        return names

    def _step_body(self, job_body, step_name):
        for step, name in zip(self._steps(job_body), self._step_names(job_body)):
            if name == step_name:
                return step
        self.fail("step %s not found" % step_name)

    def test_runs_only_nightly_and_on_manual_dispatch(self):
        triggers = self.text.split("\non:\n", 1)[1].split("\npermissions:\n", 1)[0]
        self.assertEqual(
            ["schedule", "workflow_dispatch"],
            re.findall(r"^  ([a-z_]+):", triggers, re.MULTILINE),
        )
        self.assertEqual(1, len(_crons(triggers)))

    def test_nightly_slot_does_not_collide_with_another_scheduled_workflow(self):
        (slot,) = _crons(self.text)
        directory = _runfile(".github/workflows/main.yml").parent
        others = {}
        for path in sorted(directory.glob("*.y*ml")):
            if path.name != "coverage.yml":
                for cron in _crons(path.read_text(encoding="utf-8")):
                    others[cron] = path.name
        self.assertGreaterEqual(len(others), 5, "scheduled workflow discovery is stale")
        clashes = {
            cron: name
            for cron, name in others.items()
            if _minute_hour(cron) == _minute_hour(slot)
        }
        self.assertEqual({}, clashes)

    def test_every_run_measures_the_complete_main_tree(self):
        build = self._job_body("build")
        self.assertRegex(build, r'(?m)^      COVERAGE_TARGETS: "//donner/\.\.\."$')
        self.assertEqual("Require main", self._step_names(build)[0])
        guard = self._step_body(build, "Require main")
        self.assertIn("if: github.ref != 'refs/heads/main'", guard)
        self.assertIn("exit 1", guard)
        generate = self._step_body(build, "Generate coverage")
        self.assertIn("if [[ -f coverage-report/coverage_skipped ]]; then", generate)
        self.assertIn("cache-save: true\n", build)
        proof = self._step_body(build, "Generate coverage proof")
        self.assertIn("python3 tools/coverage_run_proof.py", proof)
        self.assertIn('--patterns "$COVERAGE_TARGETS"', proof)
        self.assertIn('--event "$GITHUB_EVENT_NAME" --ref "$GITHUB_REF"', proof)

    def test_pull_request_coverage_machinery_is_gone(self):
        for remnant in (
            "pull_request",
            "pr-incremental",
            "self-hosted",
            "re-turnstile",
            "bazel-diff",
            "determine-targets",
            "ci:full-test",
            "concurrency:",
        ):
            with self.subTest(remnant=remnant):
                self.assertNotIn(remnant, self.text)

    def test_build_hands_the_report_to_the_upload_job_as_its_last_step(self):
        build = self._job_body("build")
        self.assertNotIn("codecov/codecov-action", build)
        self.assertNotIn("CODECOV_TOKEN", build)
        self.assertEqual("Upload coverage report", self._step_names(build)[-1])
        upload = self._step_body(build, "Upload coverage report")
        self.assertTrue(upload.startswith("id: upload_report\n"), upload)
        self.assertNotRegex(upload, r"(?m)^        if: ")
        self.assertIn("uses: actions/upload-artifact@v7", upload)
        self.assertIn("name: coverage-report\n", upload)
        self.assertIn("path: coverage-report/filtered_report.dat", upload)
        self.assertIn("if-no-files-found: error", upload)
        self.assertIn(
            "report_artifact_id: ${{ steps.upload_report.outputs.artifact-id }}", build
        )
        self.assertNotIn("report_written", self.text)

    def test_codecov_upload_is_a_separate_job_that_fails_the_run(self):
        self.assertEqual(1, self.text.count("uses: codecov/codecov-action@"))
        job = self._job_body("upload-coverage")
        self.assertRegex(job, r"(?m)^    needs: build$")
        # No condition at all: the default success() skips the upload after a
        # failed or cancelled build, and nothing else may skip it.
        self.assertNotRegex(job, r"(?m)^    if: ")
        download = self._step_body(job, "Download coverage report")
        self.assertIn("uses: actions/download-artifact@v8", download)
        # By the id the build attempt produced, never by name.
        self.assertIn("artifact-ids: ${{ needs.build.outputs.report_artifact_id }}", download)
        self.assertNotRegex(download, r"(?m)^\s+name: ")
        self.assertIn("path: coverage-report\n", download)
        # The report must land directly in that path, where the upload reads it.
        self.assertIn("merge-multiple: true\n", download)
        upload = self._step_body(job, "Upload coverage to Codecov")
        self.assertIn("uses: codecov/codecov-action@", upload)
        for setting in (
            "token: ${{ secrets.CODECOV_TOKEN }}",
            "files: ./coverage-report/filtered_report.dat",
            "disable_search: true",
            "flags: unittests\n",
            "fail_ci_if_error: true",
            "verbose: false",
        ):
            with self.subTest(setting=setting):
                self.assertIn(setting, upload)
        self.assertLess(
            job.index("Download coverage report"), job.index("Upload coverage to Codecov")
        )

    def test_an_upload_outage_is_not_hidden(self):
        for masking in ("continue-on-error", "skip_validation", "nick-fields/retry", "binary:"):
            with self.subTest(masking=masking):
                self.assertNotIn(masking, self.text)

    def test_permissions_are_read_only(self):
        self.assertRegex(self.text, r"\npermissions:\n  contents: read\n\njobs:\n")
        self.assertEqual(1, self.text.count("permissions:"))

    def test_codecov_reports_only_the_main_baseline(self):
        project = self.codecov.split("    project:", 1)[1].split("    patch:", 1)[0]
        self.assertIn("- main", project)
        self.assertIn("- unittests", project)
        self.assertRegex(self.codecov, r"(?m)^    patch: false$")
        self.assertRegex(self.codecov, r"(?m)^comment: false$")
        self.assertRegex(self.codecov, r"unittests:\s*\n\s*carryforward: true")
        self.assertNotIn("pr-incremental", self.codecov)


if __name__ == "__main__":
    unittest.main()
