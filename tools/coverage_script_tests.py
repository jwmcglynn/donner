"""Coverage orchestration refuses partial reports and mixed build failures."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from python.runfiles import runfiles


def prepare_stale_diagnostics(root, enabled, summary_state, timing_state):
    if not enabled or not (summary_state or timing_state):
        return
    diagnostics = root / "private-diagnostics/coverage"
    diagnostics.mkdir(parents=True)
    if summary_state == "file":
        (diagnostics / "failure-summary.json").write_text("token=secret")
    elif summary_state == "directory":
        stale = diagnostics / "failure-summary.json"
        stale.mkdir()
        (stale / "raw.log").write_text("token=secret")
    if timing_state == "file":
        (diagnostics / "timing.txt").write_text("token=secret\n")
    elif timing_state == "directory":
        stale = diagnostics / "timing.txt"
        stale.mkdir()
        (stale / "raw.log").write_text("token=secret")


def prepare_failed_mktemp(binaries):
    mktemp = binaries / "mktemp"
    mktemp.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
    mktemp.chmod(0o755)


class CoverageScriptTest(unittest.TestCase):
    def run_fixture(self, root, quiet, status, report, failed_target=False, failed_test=False,
                    geode_failed=False, geode_xml=None, remote_failure=False,
                    ci_diagnostics=False, summary_state=None, timing_state=None,
                    fail_summary_write=False):
        binaries = root / "bin"
        binaries.mkdir()
        (root / "output").mkdir()
        (root / "tools").mkdir()
        resolver = runfiles.Create()
        for name in (
            "coverage_bep_status.py",
            "filter_coverage.py",
            "check_lcov_report.py",
            "lcov_metrics.py",
        ):
            shutil.copyfile(resolver.Rlocation("donner/tools/" + name), root / "tools" / name)
        (root / "donner").mkdir()
        (root / "donner/example.cc").write_text("int answer() { return 42; }\n", encoding="utf-8")
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
        if failed_test:
            events.extend([
                {
                    "id": {"testSummary": {"label": "//fixture:failed_test"}},
                    "testSummary": {"overallStatus": "FAILED"},
                },
                {
                    "id": {"testSummary": {"label": "//fixture\nsecret:invalid"}},
                    "testSummary": {"overallStatus": "FAILED"},
                },
            ])
        if geode_failed:
            events.append({
                "id": {"testResult": {
                    "label": "//donner/svg/renderer/tests:renderer_geode_golden_tests",
                }},
                "testResult": {
                    "status": "FAILED",
                    "testActionOutput": [{"name": "test.xml", "uri": "file:///private/runner"}],
                },
            })
            events.append({
                "id": {"testSummary": {
                    "label": "//donner/svg/renderer/tests:renderer_geode_golden_tests",
                }},
                "testSummary": {"overallStatus": "FAILED"},
            })
            if geode_xml is not None:
                xml_path = (root / "bazel-testlogs/donner/svg/renderer/tests"
                            / "renderer_geode_golden_tests/test.xml")
                xml_path.parent.mkdir(parents=True)
                xml_path.write_text(geode_xml, encoding="utf-8")
        if remote_failure:
            events.append({
                "id": {"buildFinished": {}},
                "finished": {
                    "exitCode": {"code": 34, "name": "REMOTE_ERROR"},
                    "failureDetail": {
                        "message": "grpc://private.example/secret /runner/path token=secret",
                        "remoteExecution": {"code": "TOPLEVEL_OUTPUTS_DOWNLOAD_FAILURE"},
                    },
                },
            })
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
            '  printf "invoked\\n" > "$FIXTURE_ROOT/coverage-invoked"\n'
            '  for arg in "$@"; do\n'
            '   case "$arg" in --build_event_json_file=*)\n'
            '    cp "$FIXTURE_ROOT/fixture-bep.json" "${arg#*=}";;\n'
            '   esac\n'
            '  done\n'
            '  if [[ "$FIXTURE_REPORT" = 1 ]]; then\n'
            '   mkdir -p "$FIXTURE_ROOT/output/_coverage"\n'
            '   printf "SF:donner/example.cc\\nDA:1,1\\nLF:1\\nLH:1\\nend_of_record\\n" '
            '> "$FIXTURE_ROOT/output/_coverage/_coverage_report.dat"\n'
            '  fi\n'
            '  if [[ -n "$FIXTURE_PRIVATE_ERROR" ]]; then\n'
            '   printf "%s\\n" "$FIXTURE_PRIVATE_ERROR"\n'
            '  fi\n'
            '  exit "$FIXTURE_STATUS";;\n'
            'esac\n',
            encoding="utf-8",
        )
        fake_bazel.chmod(0o755)
        for name, body in (
            ("java", "#!/bin/sh\nexit 0\n"),
            # Discovery checks executable paths without running either tool.
            # Supply fixture-owned executables instead of assuming /bin/true
            # exists on every worker (macOS provides /usr/bin/true).
            ("clang", '#!/bin/sh\nprintf "%s/%s\\n" "${0%/*}" "${1#--print-prog-name=}"\n'),
            ("llvm-cov", "#!/bin/sh\nexit 99\n"),
            ("llvm-profdata", "#!/bin/sh\nexit 99\n"),
            ("python3", '#!/bin/sh\nexec "$FIXTURE_PYTHON" "$@"\n'),
        ):
            path = binaries / name
            path.write_text(body, encoding="utf-8")
            path.chmod(0o755)
        prepare_stale_diagnostics(root, ci_diagnostics, summary_state, timing_state)
        if fail_summary_write:
            prepare_failed_mktemp(binaries)
        command = ["bash", resolver.Rlocation("donner/tools/coverage.sh"), "--no-html"]
        if quiet:
            command.append("--quiet")
        result = subprocess.run(
            command,
            cwd=root,
            env={
                **os.environ,
                "PATH": str(binaries) + os.pathsep + os.environ["PATH"],
                "FIXTURE_ROOT": str(root),
                "FIXTURE_PYTHON": sys.executable,
                "FIXTURE_STATUS": str(status),
                "FIXTURE_REPORT": str(int(report)),
                "FIXTURE_PRIVATE_ERROR": (
                    "ERROR: io.grpc.StatusRuntimeException: UNAVAILABLE: "
                    "grpc://private.example/secret /runner/path token=secret "
                    "Failed to download remote output"
                    if remote_failure else ""
                ),
                "DONNER_BAZEL": str(fake_bazel),
                "DONNER_CI_DIAGNOSTICS_DIR": (
                    str(root / "private-diagnostics") if ci_diagnostics else ""
                ),
            },
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        self.assertEqual(
            "invoked\n",
            (root / "coverage-invoked").read_text(encoding="utf-8")
            if (root / "coverage-invoked").exists() else "",
            result.stdout + result.stderr,
        )
        return result

    def test_successful_report_reaches_real_filtering_and_validation(self):
        for quiet in (False, True):
            with self.subTest(quiet=quiet), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                result = self.run_fixture(root, quiet, status=0, report=True)
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertIn("Filtered LCOV coverage metrics:", result.stdout)
                filtered = (root / "coverage-report/filtered_report.dat").read_text(encoding="utf-8")
                self.assertIn("SF:donner/example.cc\n", filtered)
                self.assertIn("DA:1,1\n", filtered)
                self.assertIn("LF:1\n", filtered)
                self.assertIn("LH:1\n", filtered)
                self.assertFalse((root / "coverage-report/coverage_skipped").exists())

    def test_failed_run_cannot_publish_an_existing_combined_report(self):
        for quiet in (False, True):
            with self.subTest(quiet=quiet), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                result = self.run_fixture(root, quiet, status=3, report=True)
                self.assertEqual(3, result.returncode, result.stdout + result.stderr)
                self.assertIn("refusing to publish a partial report", result.stdout)
                self.assertTrue((root / "output/_coverage/_coverage_report.dat").is_file())
                self.assertFalse((root / "coverage-report/filtered_report.dat").exists())

    def test_quiet_failure_names_tests_without_exposing_raw_bep_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = self.run_fixture(root, True, status=3, report=True, failed_test=True)
            self.assertEqual(3, result.returncode, result.stdout + result.stderr)
            self.assertIn("//fixture:failed_test", result.stdout)
            self.assertNotIn("secret", result.stdout)
            self.assertFalse((root / "coverage-report/filtered_report.dat").exists())

    def test_exit34_with_no_failed_labels_writes_only_safe_structured_context(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = self.run_fixture(root, True, status=34, report=True,
                                      remote_failure=True, ci_diagnostics=True)
            self.assertEqual(result.returncode, 34)
            self.assertIn("Bazel coverage failure context (BEP):", result.stdout)
            self.assertIn("TOPLEVEL_OUTPUTS_DOWNLOAD_FAILURE", result.stdout)
            self.assertNotIn("private.example", result.stdout + result.stderr)
            self.assertNotIn("/runner/path", result.stdout + result.stderr)
            self.assertNotIn("token=secret", result.stdout + result.stderr)
            summary_path = root / "private-diagnostics/coverage/failure-summary.json"
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            self.assertEqual(summary["processExitCode"], 34)
            self.assertEqual(summary["failureCode"], "TOPLEVEL_OUTPUTS_DOWNLOAD_FAILURE")
            self.assertEqual(summary["remoteLogObservations"],
                             ["DOWNLOAD_FAILURE", "GRPC_UNAVAILABLE"])
            self.assertNotIn("secret", json.dumps(summary))
            self.assertEqual(list(summary_path.parent.glob(".failure-summary.*")), [])
            self.assertIn("token=secret", (root / "coverage-report/bazel_coverage.log").read_text())
            self.assertFalse((root / "coverage-report/filtered_report.dat").exists())

    def test_stale_regular_diagnostics_are_replaced_with_safe_values(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            result = self.run_fixture(root, True, status=34, report=True,
                                      remote_failure=True, ci_diagnostics=True,
                                      summary_state="file", timing_state="file")
            self.assertEqual(result.returncode, 34)
            diagnostics = root / "private-diagnostics/coverage"
            self.assertNotIn("secret", (diagnostics / "timing.txt").read_text())
            self.assertNotIn("secret", (diagnostics / "failure-summary.json").read_text())
            self.assertEqual(list(diagnostics.glob("failure-summary.json.*")), [])

    def test_stale_directory_and_summary_write_error_keep_original_status(self):
        for summary_state, timing_state, fail_write in (
                ("directory", None, False), (None, "directory", False),
                (None, None, True)):
            with self.subTest(summary=summary_state, timing=timing_state,
                              fail_write=fail_write), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                result = self.run_fixture(root, True, status=34, report=True,
                                          remote_failure=True, ci_diagnostics=True,
                                          summary_state=summary_state,
                                          timing_state=timing_state,
                                          fail_summary_write=fail_write)
                self.assertEqual(result.returncode, 34)
                self.assertNotIn("secret", result.stdout + result.stderr)
                diagnostics = root / "private-diagnostics/coverage"
                self.assertEqual(list(diagnostics.glob("failure-summary.json.*")), [])
                if summary_state == "directory":
                    self.assertTrue((diagnostics / "failure-summary.json").is_dir())
                if fail_write:
                    self.assertFalse((diagnostics / "failure-summary.json").exists())

    def test_quiet_geode_failure_names_case_without_exposing_assertion(self):
        xml = ('<testsuites failures="1"><testcase classname="RendererGeodeGoldenTests" '
               'name="Lion"><failure message="/private/runner/path">secret pixels'
               '</failure></testcase></testsuites>')
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_fixture(Path(directory), True, status=3, report=True,
                                      geode_failed=True, geode_xml=xml)
        self.assertEqual(result.returncode, 3)
        self.assertIn('"status": "cases_found"', result.stdout)
        self.assertIn("RendererGeodeGoldenTests.Lion", result.stdout)
        self.assertIn('"case_names_unavailable": 0', result.stdout)
        self.assertNotIn("private", result.stdout)
        self.assertNotIn("secret", result.stdout)

    def test_quiet_geode_failure_reports_missing_xml_as_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_fixture(Path(directory), True, status=3, report=True,
                                      geode_failed=True)
        self.assertEqual(result.returncode, 3)
        self.assertIn('"status": "unavailable"', result.stdout)
        self.assertIn('"reason": "missing_xml"', result.stdout)
        self.assertIn('"case_names_unavailable": 1', result.stdout)

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
                self.assertEqual(1, result.returncode, result.stdout + result.stderr)
                self.assertIn("ERROR: Coverage report was not generated", result.stdout)
                self.assertIn("//fixture:failed", result.stdout)
                self.assertEqual(
                    (root / "fixture-bep.json").read_text(encoding="utf-8"),
                    (root / "coverage-report/bep.json").read_text(encoding="utf-8"),
                )
                self.assertFalse((root / "coverage-report/coverage_skipped").exists())
                self.assertFalse((root / "coverage-report/filtered_report.dat").exists())


if __name__ == "__main__":
    unittest.main()
