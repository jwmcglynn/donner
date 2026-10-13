"""Tests for the shared tool-test stall guard."""

from pathlib import Path
import re
import subprocess
import sys
import unittest
from unittest import mock

from tools import test_timing


class RunBoundedTest(unittest.TestCase):
    def test_stall_guard_is_a_generous_hang_bound(self):
        self.assertEqual(test_timing.STALL_GUARD_SECONDS, 120)

    def test_runs_the_command_and_returns_its_result(self):
        result = test_timing.run_bounded(
            [sys.executable, "-c", "import sys; print('ran'); sys.exit(3)"],
            capture_output=True,
            text=True,
        )
        self.assertEqual((result.returncode, result.stdout), (3, "ran\n"))

    def test_applies_the_stall_guard_and_forwards_other_arguments(self):
        with mock.patch.object(test_timing.subprocess, "run") as run:
            test_timing.run_bounded(["tool", "--flag"], check=True, cwd="/work")
        run.assert_called_once_with(
            ["tool", "--flag"], timeout=test_timing.STALL_GUARD_SECONDS, check=True, cwd="/work"
        )

    def test_rejects_a_per_call_timeout(self):
        with self.assertRaisesRegex(TypeError, "do not pass timeout"):
            test_timing.run_bounded(["tool"], timeout=5)

    def test_a_hung_command_fails_with_the_command_named(self):
        expired = subprocess.TimeoutExpired(["tool"], test_timing.STALL_GUARD_SECONDS)
        with mock.patch.object(test_timing.subprocess, "run", side_effect=expired):
            with self.assertRaises(subprocess.TimeoutExpired) as raised:
                test_timing.run_bounded(["tool"])
        self.assertEqual(raised.exception.cmd, ["tool"])


# A stall guard copied into a test drifts from the shared one; every tool test imports it instead.
DUPLICATE_DEFINITION = re.compile(r"^\s*STALL_GUARD_SECONDS\s*=", re.MULTILINE)


def duplicated_stall_guards(sources):
    return sorted(
        name for name, text in sources.items()
        if name != "test_timing.py" and DUPLICATE_DEFINITION.search(text)
    )


class StallGuardPolicyTest(unittest.TestCase):
    def test_scan_recognizes_a_copied_stall_guard(self):
        self.assertEqual(
            duplicated_stall_guards({
                "a_tests.py": "STALL_GUARD_SECONDS = 10\n",
                "b_tests.py": "from tools.test_timing import STALL_GUARD_SECONDS\n",
                "test_timing.py": "STALL_GUARD_SECONDS = 120\n",
            }),
            ["a_tests.py"],
        )

    def test_no_tool_module_defines_its_own_stall_guard(self):
        directory = Path(__file__).parent
        sources = {path.name: path.read_text(encoding="utf-8") for path in directory.glob("*.py")}
        self.assertIn("test_timing.py", sources)
        self.assertEqual(duplicated_stall_guards(sources), [])


if __name__ == "__main__":
    unittest.main()
