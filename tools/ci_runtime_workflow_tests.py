"""Pins the self-hosted CI runtime boundaries that keep full runs viable."""

import re
import unittest

from python.runfiles import runfiles


def _workflow_text(path):
    resolver = runfiles.Create()
    resolved = resolver.Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


class CiRuntimeWorkflowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.main = _workflow_text(".github/workflows/main.yml")
        cls.coverage = _workflow_text(".github/workflows/coverage.yml")
        cls.coverage_script = _workflow_text("tools/coverage.sh")

    def _job_body(self, job):
        marker = "\n  %s:\n" % job
        self.assertIn(marker, self.main, "job %s not found" % job)
        rest = self.main.split(marker, 1)[1]
        end = re.search(r"^  [A-Za-z0-9_-]+:\s*$", rest, re.MULTILINE)
        return rest[: end.start()] if end else rest

    def test_coverage_does_not_expand_ci_config_twice(self):
        """The coverage command inherits its CI config from the runner rc."""
        flag_line = re.search(
            r'^\s*DONNER_COVERAGE_BAZEL_FLAGS: "([^"]*)"$',
            self.coverage,
            re.MULTILINE,
        )
        self.assertIsNotNone(flag_line)
        self.assertNotIn("--config=ci", flag_line.group(1))

    def test_heartbeat_process_tree_does_not_require_awk(self):
        """Failure cleanup uses procps already present on the runner."""
        job = self._job_body("linux-self-hosted")
        self.assertNotIn("awk -v p=", job)
        self.assertIn('ps -o pid= --ppid "$root"', job)

    def test_coverage_process_tree_does_not_require_awk(self):
        """Coverage failure cleanup uses procps already present on the runner."""
        self.assertNotIn("awk -v p=", self.coverage_script)
        self.assertIn(
            'ps -o pid= --ppid "$root"',
            self.coverage_script,
        )


if __name__ == "__main__":
    unittest.main()
