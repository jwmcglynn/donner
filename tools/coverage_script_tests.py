"""Coverage orchestration refuses partial reports and mixed build failures."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from python.runfiles import runfiles


class CoverageScriptTest(unittest.TestCase):
    def run_fixture(self, root, quiet, status, report, failed_target=False):
        binaries = root / "bin"
        binaries.mkdir()
        (root / "output").mkdir()
        (root / "tools").mkdir()
        resolver = runfiles.Create()
        shutil.copyfile(
            resolver.Rlocation("donner/tools/coverage_bep_status.py"),
            root / "tools/coverage_bep_status.py",
        )
        events = [
            {
                "id": {"targetCompleted": {"label": "//fixture:incompatible"}},
                "aborted": {"reason": "SKIPPED"},
            }
        ]
        if failed_target:
            events.append(
                {
                    "id": {"targetCompleted": {"label": "//fixture:failed"}},
                    "completed": {"success": False},
                }
            )
        (root / "fixture-bep.json").write_text(
            "\n".join(json.dumps(event) for event in events), encoding="utf-8"
        )
        fake_bazel = binaries / "bazel"
        fake_bazel.write_text(
            '#!/bin/bash\n'
            'case "$1" in\n'
            ' info) case "$2" in\n'
            '  workspace) echo "$FIXTURE_ROOT";;\n'
            '  output_path) echo "$FIXTURE_ROOT/output";;\n'
            ' esac;;\n'
            ' coverage)\n'
            '  for arg in "$@"; do\n'
            '   case "$arg" in --build_event_json_file=*)\n'
            '    cp "$FIXTURE_ROOT/fixture-bep.json" "${arg#*=}";;\n'
            '   esac\n'
            '  done\n'
            '  if [[ "$FIXTURE_REPORT" = 1 ]]; then\n'
            '   mkdir -p "$FIXTURE_ROOT/output/_coverage"\n'
            '   printf "SF:donner/example.cc\\nDA:1,1\\nend_of_record\\n" '
            '> "$FIXTURE_ROOT/output/_coverage/_coverage_report.dat"\n'
            '  fi\n'
            '  exit "$FIXTURE_STATUS";;\n'
            'esac\n',
            encoding="utf-8",
        )
        fake_bazel.chmod(0o755)
        for name, body in (
            ("java", "#!/bin/sh\nexit 0\n"),
            ("clang", "#!/bin/sh\necho /bin/true\n"),
        ):
            path = binaries / name
            path.write_text(body, encoding="utf-8")
            path.chmod(0o755)
        command = ["bash", resolver.Rlocation("donner/tools/coverage.sh"), "--no-html"]
        if quiet:
            command.append("--quiet")
        return subprocess.run(
            command,
            cwd=root,
            env={
                **os.environ,
                "PATH": str(binaries) + os.pathsep + os.environ["PATH"],
                "FIXTURE_ROOT": str(root),
                "FIXTURE_STATUS": str(status),
                "FIXTURE_REPORT": str(int(report)),
                "DONNER_BAZEL": str(fake_bazel),
                "DONNER_CI_DIAGNOSTICS_DIR": "",
            },
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )

    def test_failed_run_cannot_publish_an_existing_combined_report(self):
        for quiet in (False, True):
            with self.subTest(quiet=quiet), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                result = self.run_fixture(root, quiet, status=3, report=True)
                self.assertEqual(3, result.returncode, result.stdout + result.stderr)
                self.assertIn("refusing to publish a partial report", result.stdout)
                self.assertTrue((root / "output/_coverage/_coverage_report.dat").is_file())
                self.assertFalse((root / "coverage-report/filtered_report.dat").exists())

    def test_all_incompatible_targets_accept_success_and_no_tests_found(self):
        for quiet in (False, True):
            for status in (0, 4):
                with self.subTest(quiet=quiet, status=status):
                    with tempfile.TemporaryDirectory() as directory:
                        root = Path(directory)
                        result = self.run_fixture(root, quiet, status, report=False)
                        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                        self.assertTrue((root / "coverage-report/coverage_skipped").is_file())
                        self.assertFalse((root / "coverage-report/filtered_report.dat").exists())

    def test_skipped_target_does_not_hide_a_failed_build(self):
        for quiet in (False, True):
            with self.subTest(quiet=quiet), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                result = self.run_fixture(root, quiet, status=1, report=False, failed_target=True)
                self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertFalse((root / "coverage-report/coverage_skipped").exists())
                self.assertFalse((root / "coverage-report/filtered_report.dat").exists())


if __name__ == "__main__":
    unittest.main()
