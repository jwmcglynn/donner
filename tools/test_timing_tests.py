"""Tests for the shared tool-test stall guard."""

from pathlib import Path
import re
import unittest

from tools import test_timing


class StallGuardTest(unittest.TestCase):
    def test_stall_guard_ends_before_the_small_test_timeout(self):
        self.assertEqual(test_timing.STALL_GUARD_SECONDS, 30)
        self.assertLess(test_timing.STALL_GUARD_SECONDS, 60)


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
