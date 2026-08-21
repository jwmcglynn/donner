#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


_CHECKER_PATH = Path(__file__).with_name("check_banned_patterns.py")
_CHECKER_SPEC = importlib.util.spec_from_file_location("check_banned_patterns", _CHECKER_PATH)
assert _CHECKER_SPEC is not None
assert _CHECKER_SPEC.loader is not None
check_banned_patterns = importlib.util.module_from_spec(_CHECKER_SPEC)
_CHECKER_SPEC.loader.exec_module(check_banned_patterns)


class CheckBannedPatternsTests(unittest.TestCase):
    def _write_source(self, source: str, filename: str = "Example.cc") -> Path:
        temp_dir = Path(tempfile.mkdtemp())
        source_path = temp_dir / "donner" / "base" / filename
        source_path.parent.mkdir(parents=True)
        source_path.write_text(source, encoding="utf-8")
        return source_path

    def _descriptions_for(
        self,
        source: str,
        filename: str = "Example.cc",
        *,
        check_method_complexity: bool = False,
    ) -> list[str]:
        return [
            error[1]
            for error in check_banned_patterns.check_file(
                self._write_source(source, filename),
                check_method_complexity=check_method_complexity,
            )
        ]

    def test_blocks_typographic_hyphens_and_dashes_in_comments_and_strings(self):
        descriptions = self._descriptions_for(
            '// A comment with a non-breaking hyphen: \u2011\n'
            '// A comment with an em dash: \u2014\n'
            'const char* range = "10\u201320";\n'
        )

        self.assertTrue(any("non-breaking hyphen (U+2011" in desc for desc in descriptions))
        self.assertTrue(any("em dash (U+2014 EM DASH)" in desc for desc in descriptions))
        self.assertTrue(any("en dash (U+2013 EN DASH)" in desc for desc in descriptions))

    def test_blocks_smart_quotes(self):
        descriptions = self._descriptions_for(
            '// Smart single quotes: \u2018value\u2019\n'
            'const char* text = "\u201cvalue\u201d";\n'
        )

        self.assertEqual(4, len([desc for desc in descriptions if "smart quote" in desc]))

    def test_blocks_hidden_unicode_whitespace(self):
        descriptions = self._descriptions_for(
            "int before\u00a0= 1;\n"
            "int after\u200b= 2;\n"
        )

        self.assertTrue(any("U+00A0 NO-BREAK SPACE" in desc for desc in descriptions))
        self.assertTrue(any("U+200B ZERO WIDTH SPACE" in desc for desc in descriptions))

    def test_blocks_raw_unicode_in_script_sources_without_cpp_rules(self):
        descriptions = self._descriptions_for(
            "# Python comment with a smart quote: \u2019\n"
            "text = 'long long in a Python string should not trigger the C++ rule'\n",
            filename="formatter.py",
        )

        self.assertEqual(1, len(descriptions))
        self.assertIn("smart quote", descriptions[0])

    def test_allows_other_intentional_unicode_literals(self):
        descriptions = self._descriptions_for(
            'const char* arrow = "\u2192";\n'
            'const char* star = "\u2731";\n'
            'const char* multiply = "\u00d7";\n'
        )

        self.assertEqual([], descriptions)

    def test_directory_scan_excludes_dependencies_and_generated_trees(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            source_root = Path(temp_dir) / "donner"
            source_root.mkdir()
            checked_source = source_root / "Example.cc"
            checked_source.write_text("int example;\n", encoding="utf-8")

            for excluded_directory in ("node_modules", "third_party", "bazel-generated", ".git"):
                dependency_source = source_root / excluded_directory / "dependency.js"
                dependency_source.parent.mkdir()
                dependency_source.write_text("// dependency\n", encoding="utf-8")

            self.assertEqual(
                [checked_source],
                check_banned_patterns._iter_source_files([source_root]),
            )

    def test_blocks_methods_above_the_local_complexity_limit(self):
        branches = "\n".join(
            f"  if (value == {index}) {{ value += {index}; }}" for index in range(11)
        )
        descriptions = self._descriptions_for(
            "void Example::run(int value) {\n" + branches + "\n}\n",
            check_method_complexity=True,
        )

        self.assertIn("complex method (11 decision points; limit 10)", descriptions)

    def test_allows_methods_at_the_local_complexity_limit(self):
        branches = "\n".join(
            f"  if (value == {index}) {{ value += {index}; }}" for index in range(10)
        )
        descriptions = self._descriptions_for(
            "void Example::run(int value) {\n" + branches + "\n}\n",
            check_method_complexity=True,
        )

        self.assertNotIn("complex method", "\n".join(descriptions))

    def test_allows_preexisting_complex_method_from_baseline(self):
        branches = "\n".join(
            f"  if (value == {index}) {{ value += {index}; }}" for index in range(11)
        )
        source = "void Example::run(int value) {\n" + branches + "\n}\n"
        source_path = self._write_source(source)
        descriptions = [
            error[1]
            for error in check_banned_patterns.check_file(
                source_path,
                check_method_complexity=True,
                method_complexity_baseline=check_banned_patterns._strip_comments_and_strings(
                    source
                ),
            )
        ]

        self.assertNotIn("complex method", "\n".join(descriptions))

    def test_blocks_new_complex_method_against_simple_baseline(self):
        current_branches = "\n".join(
            f"  if (value == {index}) {{ value += {index}; }}" for index in range(11)
        )
        baseline_branches = "\n".join(
            f"  if (value == {index}) {{ value += {index}; }}" for index in range(10)
        )
        source_path = self._write_source(
            "void Example::run(int value) {\n" + current_branches + "\n}\n"
        )
        descriptions = [
            error[1]
            for error in check_banned_patterns.check_file(
                source_path,
                check_method_complexity=True,
                method_complexity_baseline=check_banned_patterns._strip_comments_and_strings(
                    "void Example::run(int value) {\n" + baseline_branches + "\n}\n"
                ),
            )
        ]

        self.assertIn("complex method (11 decision points; limit 10)", descriptions)

    def test_blocks_preexisting_complex_method_that_gets_worse(self):
        baseline_branches = "\n".join(
            f"  if (value == {index}) {{ value += {index}; }}" for index in range(11)
        )
        current_branches = "\n".join(
            f"  if (value == {index}) {{ value += {index}; }}" for index in range(12)
        )
        source_path = self._write_source(
            "void Example::run(int value) {\n" + current_branches + "\n}\n"
        )
        descriptions = [
            error[1]
            for error in check_banned_patterns.check_file(
                source_path,
                check_method_complexity=True,
                method_complexity_baseline=check_banned_patterns._strip_comments_and_strings(
                    "void Example::run(int value) {\n" + baseline_branches + "\n}\n"
                ),
            )
        ]

        self.assertIn("complex method (12 decision points; limit 10)", descriptions)

    def test_complex_baseline_overload_does_not_exempt_new_overload(self):
        branches = "\n".join(
            f"  if (value == {index}) {{ value += {index}; }}" for index in range(11)
        )
        baseline = "void Example::run(int value) {\n" + branches + "\n}\n"
        current = baseline + "void Example::run(double value) {\n" + branches + "\n}\n"
        source_path = self._write_source(current)
        descriptions = [
            error[1]
            for error in check_banned_patterns.check_file(
                source_path,
                check_method_complexity=True,
                method_complexity_baseline=check_banned_patterns._strip_comments_and_strings(
                    baseline
                ),
            )
        ]

        self.assertEqual(1, descriptions.count("complex method (11 decision points; limit 10)"))

    def _run_lint_in_git_repo(
        self, *, current_decision_points: int, provide_origin_main: bool
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temp_dir:
            repo = Path(temp_dir)
            (repo / "build_defs").mkdir()
            (repo / "tools").mkdir()
            (repo / "donner" / "base").mkdir(parents=True)
            (repo / "examples").mkdir()
            shutil.copy2(_CHECKER_PATH, repo / "build_defs" / "check_banned_patterns.py")
            shutil.copy2(
                Path(__file__).resolve().parents[1] / "tools" / "lint.sh",
                repo / "tools" / "lint.sh",
            )

            def write_source(decision_points: int) -> None:
                branches = "\n".join(
                    f"  if (value == {index}) {{ value += {index}; }}"
                    for index in range(decision_points)
                )
                (repo / "donner" / "base" / "Example.cc").write_text(
                    "void Example::run(int value) {\n" + branches + "\n}\n",
                    encoding="utf-8",
                )

            subprocess.run(["git", "init", "-b", "main"], cwd=repo, check=True, capture_output=True)
            subprocess.run(
                ["git", "config", "user.email", "lint-test@example.invalid"],
                cwd=repo,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "Lint Test"], cwd=repo, check=True
            )
            write_source(10)
            subprocess.run(["git", "add", "."], cwd=repo, check=True)
            subprocess.run(
                ["git", "commit", "-m", "baseline"], cwd=repo, check=True, capture_output=True
            )
            if provide_origin_main:
                subprocess.run(
                    ["git", "update-ref", "refs/remotes/origin/main", "HEAD"],
                    cwd=repo,
                    check=True,
                )
            write_source(current_decision_points)
            subprocess.run(["git", "add", "."], cwd=repo, check=True)
            subprocess.run(
                ["git", "commit", "--allow-empty", "-m", "candidate"],
                cwd=repo,
                check=True,
                capture_output=True,
            )
            subprocess.run(
                ["git", "checkout", "--detach"], cwd=repo, check=True, capture_output=True
            )

            return subprocess.run(
                [str(repo / "tools" / "lint.sh")],
                cwd=repo,
                check=False,
                capture_output=True,
                text=True,
            )

    def test_lint_checks_detached_branch_diff_against_origin_main(self):
        completed = self._run_lint_in_git_repo(
            current_decision_points=11, provide_origin_main=True
        )

        self.assertNotEqual(0, completed.returncode)
        self.assertIn("complex method (11 decision points; limit 10)", completed.stdout)

    def test_lint_fails_closed_without_target_baseline(self):
        completed = self._run_lint_in_git_repo(
            current_decision_points=10, provide_origin_main=False
        )

        self.assertNotEqual(0, completed.returncode)
        self.assertIn("cannot resolve complexity baseline", completed.stderr)


if __name__ == "__main__":
    unittest.main()
