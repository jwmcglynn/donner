"""Regression tests for brace enforcement in the contributor lint command."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parent.parent


class LintBracesTests(unittest.TestCase):
    def run_lint(self, source: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "brace_sample.cc"
            path.write_text(source)
            return subprocess.run(
                [str(REPO_ROOT / "tools" / "lint.sh"), str(path)],
                cwd=REPO_ROOT,
                env=os.environ.copy(),
                capture_output=True,
                text=True,
                check=False,
            )

    def test_braceless_if_fails(self):
        result = self.run_lint(
            "int f(bool enabled) {\n"
            "  if (enabled)\n"
            "    return 1;\n"
            "  return 0;\n"
            "}\n"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("clang-format-violations", result.stderr)

    def test_braced_if_passes(self):
        result = self.run_lint(
            "int f(bool enabled) {\n"
            "  if (enabled) {\n"
            "    return 1;\n"
            "  }\n"
            "  return 0;\n"
            "}\n"
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
