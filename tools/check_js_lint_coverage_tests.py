"""Runs check_js_lint_coverage.py on fixture repositories."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest

from tools import check_js_lint_coverage
from tools import test_timing

_GIT_ENV = {"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CEILING_DIRECTORIES": tempfile.gettempdir()}

_BUILD = textwrap.dedent("""\
    eslint_bin.eslint_test(
        name = "js_lint_test",
        data = glob([
            "*.js",
            "*.mjs",
        ]) + [
            ":node_modules/@eslint/js",
            "//tools:treemap.js",
            "//examples:viewer.mjs",
        ],
    )
    """)


class CheckJsLintCoverageTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="js lint coverage ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self._git("init", "-q")
        self._write(f"{check_js_lint_coverage.LINT_PACKAGE}/BUILD.bazel", _BUILD)
        for path in (f"{check_js_lint_coverage.LINT_PACKAGE}/harness.mjs",
                     f"{check_js_lint_coverage.LINT_PACKAGE}/config.js",
                     "tools/treemap.js", "examples/viewer.mjs"):
            self._add(path)

    def _git(self, *args):
        subprocess.run(["git", "-C", str(self.root), *args], check=True,
                       env=dict(os.environ, **_GIT_ENV), timeout=test_timing.STALL_GUARD_SECONDS)

    def _write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
        self._git("add", "--", path)

    def _add(self, path):
        self._write(path, "export {};\n")

    def _run(self):
        result = subprocess.run(
            [sys.executable, "-B", check_js_lint_coverage.__file__, "--root", str(self.root)],
            capture_output=True, text=True, check=False, env=dict(os.environ, **_GIT_ENV),
            timeout=test_timing.STALL_GUARD_SECONDS)
        return result.returncode, result.stdout + result.stderr

    def test_every_tracked_file_declared_passes(self):
        status, output = self._run()
        self.assertEqual(status, 0, output)
        self.assertIn("4 JavaScript files, all linted", output)

    def test_an_unlisted_file_in_another_package_fails(self):
        for path in ("donner/gpu/browser/library.js", "examples/new_test.mjs"):
            with self.subTest(path=path):
                self._add(path)
                status, output = self._run()
                self.assertEqual(status, 1, output)
                self.assertIn(f"{path}: not linted", output)
                self._git("rm", "-q", "--cached", "--", path)

    def test_a_file_in_a_subdirectory_of_the_lint_package_is_not_covered_by_its_glob(self):
        path = f"{check_js_lint_coverage.LINT_PACKAGE}/nested/helper.mjs"
        self._add(path)
        status, output = self._run()
        self.assertEqual(status, 1, output)
        self.assertIn(f"{path}: not linted", output)

    def test_new_files_in_the_lint_package_are_covered_by_its_glob(self):
        self._add(f"{check_js_lint_coverage.LINT_PACKAGE}/new.spec.mjs")
        status, output = self._run()
        self.assertEqual(status, 0, output)

    def test_files_codefactor_skips_are_not_required(self):
        for path in ("third_party/lib/vendor.js", "jest.config.cjs", "tools/types.ts"):
            self._add(path)
        (self.root / "tools" / "untracked.mjs").write_text("export {};\n", encoding="utf-8")
        status, output = self._run()
        self.assertEqual(status, 0, output)

    def test_a_listed_file_that_is_not_tracked_fails(self):
        self._git("rm", "-q", "--cached", "--", "examples/viewer.mjs")
        status, output = self._run()
        self.assertEqual(status, 1, output)
        self.assertIn("examples/viewer.mjs: listed in", output)

    def test_an_unreadable_declaration_fails(self):
        package = check_js_lint_coverage.LINT_PACKAGE
        for build in ('js_test(name = "other", data = [])\n',
                      'eslint_test(name = "js_lint_test")\n',
                      'eslint_test(name = "js_lint_test", data = SOURCES)\n',
                      'eslint_test(name = "js_lint_test", data = glob(PATTERNS))\n'):
            with self.subTest(build=build):
                self._write(f"{package}/BUILD.bazel", build)
                status, output = self._run()
                self.assertEqual(status, 1, output)
                self.assertIn("check_js_lint_coverage:", output)


if __name__ == "__main__":
    unittest.main()
