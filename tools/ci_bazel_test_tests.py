"""Pins the exit-code contract of the CI bazel-test wrapper.

The wrapper exists so a change-based lane whose selected target set contains no
test targets reports success instead of bazel's exit 4. The property that makes
that safe is narrowness: exit 4 (and only exit 4) becomes success, so a build
break, a failing test, or a bad command line still fails the lane. These tests
hold that line by driving the wrapper with a stand-in command whose exit status
the test chooses, which needs no bazel and no workspace.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles

# Bazel's exit status for "Build successful but no tests were found even though
# testing was requested".
NO_TESTS_FOUND_EXIT_CODE = 4

NOTICE_PREFIX = "::notice::"


class CiBazelTestWrapperTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resolver = runfiles.Create()
        cls.script = resolver.Rlocation("donner/tools/ci_bazel_test.sh")
        assert cls.script is not None, "ci_bazel_test.sh not found in runfiles"
        assert os.path.exists(cls.script), cls.script

    def _fake_command(self, directory, exit_code):
        """Write a stand-in command that echoes its arguments and exits.

        Args:
            directory: Directory to write the command into.
            exit_code: Status the command exits with.

        Returns:
            Path to the executable stand-in command.
        """
        command = Path(directory) / "fake_bazel.sh"
        command.write_text(
            "#!/usr/bin/env bash\n"
            'echo "fake-bazel-stdout: $*"\n'
            'echo "fake-bazel-stderr" >&2\n'
            f"exit {exit_code}\n",
            encoding="utf-8",
        )
        command.chmod(0o755)
        return command

    def _passthrough_wrapper(self, directory):
        """Write a wrapper that runs its arguments and propagates their status.

        Mirrors the logging wrapper a self-hosted lane runs bazel under, so the
        composed invocation is covered and not just the direct one.
        """
        wrapper = Path(directory) / "passthrough.sh"
        wrapper.write_text(
            "#!/usr/bin/env bash\nset -uo pipefail\n\"$@\"\nexit $?\n",
            encoding="utf-8",
        )
        wrapper.chmod(0o755)
        return wrapper

    def _run(self, args):
        return subprocess.run(
            [self.script, *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=60,
        )

    def _run_fake(self, exit_code, args=("test", "//pkg:target"), compose=False):
        with tempfile.TemporaryDirectory() as temp_dir:
            command = self._fake_command(temp_dir, exit_code)
            argv = [str(command), *args]
            if compose:
                argv = [str(self._passthrough_wrapper(temp_dir)), *argv]
            return self._run(argv)

    def test_script_parses(self):
        """A syntax regression must not wait for a CI lane to surface."""
        result = subprocess.run(
            ["bash", "-n", self.script],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)

    def test_empty_test_selection_reports_success_with_a_notice(self):
        """Exit 4 means "nothing to test", which is not a defect in the change."""
        result = self._run_fake(NO_TESTS_FOUND_EXIT_CODE)

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn(NOTICE_PREFIX, result.stdout)
        self.assertIn("No test targets", result.stdout)

    def test_empty_test_selection_is_tolerated_through_a_logging_wrapper(self):
        """Lanes that run bazel under a wrapper get the same treatment."""
        result = self._run_fake(NO_TESTS_FOUND_EXIT_CODE, compose=True)

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn(NOTICE_PREFIX, result.stdout)

    def test_success_is_passed_through_without_a_notice(self):
        """An ordinary green run must look exactly like an unwrapped one."""
        result = self._run_fake(0)

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertNotIn(NOTICE_PREFIX, result.stdout)

    def test_build_failure_still_fails(self):
        """Exit 1 (build error) must keep failing the lane."""
        result = self._run_fake(1)

        self.assertEqual(1, result.returncode)
        self.assertNotIn(NOTICE_PREFIX, result.stdout)

    def test_command_line_error_still_fails(self):
        """Exit 2 (bad command line) must keep failing the lane."""
        result = self._run_fake(2)

        self.assertEqual(2, result.returncode)
        self.assertNotIn(NOTICE_PREFIX, result.stdout)

    def test_failing_test_still_fails(self):
        """Exit 3 (tests ran and failed) must keep failing the lane."""
        result = self._run_fake(3)

        self.assertEqual(3, result.returncode)
        self.assertNotIn(NOTICE_PREFIX, result.stdout)

    def test_failing_test_still_fails_through_a_logging_wrapper(self):
        """The composed form must not soften a real test failure either."""
        result = self._run_fake(3, compose=True)

        self.assertEqual(3, result.returncode)
        self.assertNotIn(NOTICE_PREFIX, result.stdout)

    def test_environment_failures_still_fail(self):
        """Bazel's internal/environment statuses are passed through unchanged."""
        for exit_code in (36, 37, 38):
            with self.subTest(exit_code=exit_code):
                result = self._run_fake(exit_code)
                self.assertEqual(exit_code, result.returncode)
                self.assertNotIn(NOTICE_PREFIX, result.stdout)

    def test_arguments_and_output_are_passed_through(self):
        """The wrapper must be transparent to the command it runs."""
        result = self._run_fake(0, args=("test", "--config=ci", "//pkg:target"))

        self.assertIn("fake-bazel-stdout: test --config=ci //pkg:target", result.stdout)
        self.assertIn("fake-bazel-stderr", result.stderr)

    def test_missing_command_is_a_usage_error(self):
        """An empty command line is a workflow bug, not an empty test set."""
        result = self._run([])

        self.assertEqual(2, result.returncode)
        self.assertNotIn(NOTICE_PREFIX, result.stdout)


if __name__ == "__main__":
    unittest.main()
