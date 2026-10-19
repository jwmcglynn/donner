"""Exercises exclusion expansion and the actual child command boundary."""

import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

from python.runfiles import runfiles
from tools import test_timing


class ExclusionSelectionTest(unittest.TestCase):
    def _run(self, query_output, query_status=0, test_status=0):
        resolver = runfiles.Create()
        helper = resolver.Rlocation("donner/tools/ci/test_with_exclusions.py")
        with tempfile.TemporaryDirectory(prefix="ci selection ") as temporary:
            root = Path(temporary)
            capture = root / "commands.jsonl"
            bazel = root / "fake bazel"
            # The fixture runs on this test's own interpreter; a `#!/usr/bin/env python3`
            # shebang would resolve a host interpreter that remote executors need not have.
            fixture = root / "fake bazel.py"
            fixture.write_text(
                "import json, os, sys\n"
                "with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as output:\n"
                "    output.write(json.dumps(sys.argv[1:]) + '\\n')\n"
                "if 'query' in sys.argv:\n"
                "    sys.stdout.write(os.environ['QUERY_OUTPUT'])\n"
                "    print('query diagnostic', file=sys.stderr)\n"
                "    sys.exit(int(os.environ['QUERY_STATUS']))\n"
                "sys.exit(int(os.environ['TEST_STATUS']))\n",
                encoding="utf-8",
            )
            bazel.write_text(
                '#!/bin/sh\nexec %s %s "$@"\n'
                % (shlex.quote(sys.executable), shlex.quote(str(fixture))),
                encoding="utf-8",
            )
            bazel.chmod(0o755)
            result = subprocess.run(
                [sys.executable, helper, "--exclude-suite", "//tools/ci:fixture", "--",
                 str(bazel), "--nohome_rc", "test", "--config=asan",
                 "--test_arg=argument with spaces", "--", "//donner/..."],
                env=dict(os.environ, CAPTURE=str(capture), QUERY_OUTPUT=query_output,
                         QUERY_STATUS=str(query_status), TEST_STATUS=str(test_status)),
                capture_output=True,
                text=True,
                check=False,
                timeout=test_timing.STALL_GUARD_SECONDS,
            )
            commands = [json.loads(line) for line in capture.read_text().splitlines()]
            return result, commands

    def test_expansion_preserves_config_startup_flags_and_argument_boundaries(self):
        result, commands = self._run("//fixture:allocator\n//fixture:allocator\n", test_status=7)
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(commands, [
            ["--nohome_rc", "query", "--output=label", "tests(//tools/ci:fixture)"],
            ["--nohome_rc", "test", "--config=asan", "--test_arg=argument with spaces",
             "--", "//donner/...", "-//fixture:allocator"],
        ])
        self.assertIn("query diagnostic", result.stderr)

    def test_query_failure_stops_before_testing_and_preserves_status(self):
        result, commands = self._run("//fixture:partial_result\n", query_status=19)
        self.assertEqual(result.returncode, 19, result.stderr)
        self.assertEqual(len(commands), 1, commands)
        self.assertIn("query diagnostic", result.stderr)

    def test_empty_or_invalid_query_results_never_invoke_tests(self):
        for output in ("", "\n", "//fixture:valid\n--test_filter=all\n", "//fixture:bad name\n"):
            with self.subTest(output=output):
                result, commands = self._run(output)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(len(commands), 1, commands)
                self.assertIn("CI test selection failed", result.stderr)


if __name__ == "__main__":
    unittest.main()
