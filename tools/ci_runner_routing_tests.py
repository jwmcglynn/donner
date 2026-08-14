"""Pins trusted CI routing to the configured self-hosted runner for each OS."""

import re
import unittest

from python.runfiles import runfiles


def _workflow_text(path):
    resolver = runfiles.Create()
    resolved = resolver.Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


class CiRunnerRoutingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = _workflow_text(".github/workflows/main.yml")
        cls.coverage_text = _workflow_text(".github/workflows/coverage.yml")

    def _job_body(self, job):
        marker = "\n  %s:\n" % job
        self.assertIn(marker, self.text, "job %s not found" % job)
        rest = self.text.split(marker, 1)[1]
        end = re.search(r"^  [A-Za-z0-9_-]+:\s*$", rest, re.MULTILINE)
        return rest[: end.start()] if end else rest

    def test_change_size_does_not_override_trusted_runner_routing(self):
        """A trusted run follows the runner gate regardless of changed-file count."""
        self.assertNotIn("SELF_HOSTED_MAX_CHANGED_FILES", self.text)
        self.assertNotIn("outputs.large_change", self.text)
        self.assertNotIn("changed_file_count", self.text)
        self.assertNotIn("SELF_HOSTED_MAX_CHANGED_FILES", self.coverage_text)
        self.assertNotIn("outputs.large_change", self.coverage_text)
        self.assertNotIn("changed_file_count", self.coverage_text)

    def test_linux_jobs_are_selected_only_by_the_trusted_runner_gate(self):
        hosted = self._job_body("linux")
        turnstile = self._job_body("linux-self-hosted-turnstile")
        self_hosted = self._job_body("linux-self-hosted")

        self.assertIn("outputs.use_self_hosted_linux != 'true'", hosted)
        self.assertIn("outputs.use_self_hosted_linux == 'true'", turnstile)
        self.assertIn("outputs.use_self_hosted_linux == 'true'", self_hosted)

    def test_macos_jobs_are_selected_only_by_the_trusted_runner_gate(self):
        hosted = self._job_body("macos")
        self_hosted = self._job_body("macos-self-hosted")

        self.assertIn("outputs.use_self_hosted_macos != 'true'", hosted)
        self.assertIn("outputs.use_self_hosted_macos == 'true'", self_hosted)


if __name__ == "__main__":
    unittest.main()
