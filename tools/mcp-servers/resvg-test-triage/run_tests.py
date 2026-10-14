"""Run the dependency-free triage regression suite under Bazel."""

import unittest
from pathlib import Path


def main():
    tests = unittest.defaultTestLoader.discover(str(Path(__file__).parent), pattern="test_*.py")
    if tests.countTestCases() == 0:
        raise SystemExit("No triage regression tests were discovered")
    result = unittest.TextTestRunner(verbosity=2).run(tests)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
