"""Runs the BCR Preflight step commands against recording stand-ins for the tools they call.

The expected command lists are the commands the workflow's inline step scripts ran, written out
literally, so a change to what a step runs fails here and is reviewed as a workflow change.
"""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

from tools import bcr_preflight
from tools import test_timing
from tools.test_executable_fixtures import ExecutableFixtureDirectory

COMMIT = "0123456789abcdef0123456789abcdef01234567"
TARGETS = ["//tools/ci:release_svg", "//tools/ci:release_svg_notice",
           "//tools/ci:release_compiler_identity", "//tools/ci:release_cli_resources"]
LIBRARIES = ["@donner//donner/base:base", "@donner//donner/css:css",
             "@donner//donner/svg:svg", "@donner//donner/svg/renderer:renderer"]

# Each stand-in records its name, arguments and working directory, then exits with the status
# named for it in FAIL (`tool=status`, `tool:first-argument=status`, or
# `tool~text-in-any-argument=status`). `git` prints $HEAD,
# `bazelisk cquery` prints a label row, and `python3 -m venv DIR` creates DIR's pip and python.
_RECORDER = """\
import json, os, sys
tool, args = sys.argv[1], sys.argv[2:]
with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as capture:
    capture.write(json.dumps([tool, *args, os.getcwd()]) + '\\n')
if tool == 'git':
    print(os.environ['HEAD'])
if tool == 'bazelisk' and args[:1] == ['cquery']:
    print('cc_library rule @donner//donner/base:base (abc)')
if tool == 'python3' and args[:2] == ['-m', 'venv']:
    bin_dir = os.path.join(args[2], 'bin')
    os.makedirs(bin_dir)
    for name in ('pip', 'python'):
        path = os.path.join(bin_dir, name)
        with open(path, 'w', encoding='utf-8') as script:
            script.write(os.environ['STAND_IN'] % ('venv-' + name))
        os.chmod(path, 0o755)
failures = dict(item.split('=') for item in os.environ['FAIL'].split())
for key, status in failures.items():
    name, _, part = key.partition('~')
    if part and name == tool and any(part in arg for arg in args):
        sys.exit(int(status))
key = tool + ':' + args[0] if args else tool
sys.exit(int(failures.get(key, failures.get(tool, 0))))
"""


class BcrPreflightTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        recorder = cls.executables.path / "record.py"
        recorder.write_text(_RECORDER, encoding="utf-8")
        cls.stand_in = "#!/bin/sh\nexec %s %s %%s \"$@\"\n" % (
            shlex.quote(sys.executable), shlex.quote(str(recorder)))
        (cls.executables.path / "bin").mkdir()
        for tool in ("bazelisk", "git", "python3"):
            cls.executables.write("bin/" + tool, cls.stand_in % tool)
        cls.bin = cls.executables.path / "bin"

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="bcr preflight ")
        self.addCleanup(temporary.cleanup)
        # Resolved, since the stand-ins record the working directory the OS reports.
        self.work = Path(temporary.name).resolve()
        self.temp = self.work / "runner temp"
        self.temp.mkdir()
        self.workspace = self.work / "workspace"
        self.workspace.mkdir()
        self.capture = self.work / "calls.jsonl"
        self.summary = self.work / "summary.md"

    def _run(self, *args, fail="", head=COMMIT, **env):
        self.capture.write_text("", encoding="utf-8")
        self.summary.write_text("", encoding="utf-8")
        environ = {"PATH": str(self.bin), "CAPTURE": str(self.capture), "FAIL": fail,
                   "HEAD": head, "STAND_IN": self.stand_in,
                   "GITHUB_STEP_SUMMARY": str(self.summary), "RUNNER_TEMP": str(self.temp),
                   "SOURCE_COMMIT": COMMIT, "GITHUB_RUN_ID": "1234",
                   "GITHUB_RUN_ATTEMPT": "2", "GITHUB_WORKSPACE": str(self.workspace)}
        environ.update(env)
        result = subprocess.run([sys.executable, "-B", bcr_preflight.__file__, *args],
                                cwd=self.workspace, env=environ, capture_output=True,
                                text=True, check=False, timeout=test_timing.STALL_GUARD_SECONDS)
        calls = [json.loads(line) for line in self.capture.read_text().splitlines()]
        return result, calls

    def _consumer_commands(self):
        temp, workspace = str(self.temp), str(self.workspace)
        consumer = f"{temp}/donner-bcr/consumer-test/consumer"
        flags = ["--lockfile_mode=off",
                 f"--registry=file://{temp}/donner-bcr/consumer-test/registry",
                 "--registry=https://bcr.bazel.build"]
        return [
            ["python3", "-B", "-m", "tools.bcr_source", "consumer",
             "--artifacts", f"{temp}/donner-bcr/artifacts",
             "--registry", f"{temp}/donner-bcr/entry/registry",
             "--output", f"{temp}/donner-bcr/consumer-test", "--commit", COMMIT, workspace],
            ["bazelisk", "test", *flags, "//:all", consumer],
            ["bazelisk", "build", *flags, *LIBRARIES, consumer],
            ["git", "-C", workspace, "rev-parse", "HEAD", consumer],
            ["bazelisk", "cquery", *flags, "deps(%s)" % " + ".join(LIBRARIES),
             "--output=label_kind", consumer],
            ["python3", "-B", f"{workspace}/tools/bcr_dependency_closure.py",
             "--module", f"{workspace}/MODULE.bazel",
             "--labels", f"{temp}/donner-bcr/consumer-closure.labels", consumer],
        ]

    def _make_consumer_directory(self):
        (self.temp / "donner-bcr/consumer-test/consumer").mkdir(parents=True)

    def test_consumer_runs_the_workflow_commands_in_order(self):
        self._make_consumer_directory()
        result, calls = self._run("consumer")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, self._consumer_commands())
        # The audit reads the query's output, written where the workflow wrote it.
        self.assertEqual((self.temp / "donner-bcr/consumer-closure.labels").read_text(),
                         "cc_library rule @donner//donner/base:base (abc)\n")
        self.assertEqual(result.stdout.count("::group::"), 6)
        self.assertEqual(result.stdout.count("::endgroup::"), 6)
        summary = self.summary.read_text(encoding="utf-8")
        self.assertIn("### BCR consumer", summary)
        self.assertIn("| Audit the configured closure | passed |", summary)
        self.assertNotIn("failed", summary)

    def test_consumer_requires_the_checkout_at_the_source_commit(self):
        self._make_consumer_directory()
        result, calls = self._run("consumer", head="f" * 40)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(calls, self._consumer_commands()[:4])
        self.assertIn("failed (exit 1)", self.summary.read_text())

    def test_a_failing_command_stops_the_lane_with_its_status(self):
        commands = self._consumer_commands()
        for fail, count in (("python3:-B=3", 1), ("bazelisk:test=4", 2),
                            ("bazelisk:build=5", 3), ("git=6", 4),
                            ("bazelisk:cquery=7", 5)):
            with self.subTest(fail=fail):
                shutil.rmtree(self.temp)
                self.temp.mkdir()
                self._make_consumer_directory()
                result, calls = self._run("consumer", fail=fail)
                # A failing git leaves the commit check failing with status 1, as `test` did.
                status = 1 if fail.startswith("git") else int(fail.rsplit("=", 1)[1])
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertEqual(calls, commands[:count])
                self.assertIn(f"failed (exit {status})", self.summary.read_text())
                self.assertIn("failed with exit status %d" % status, result.stdout)
                self.assertEqual(result.stdout.count("::group::"),
                                 result.stdout.count("::endgroup::"))

    def test_audit_failure_is_the_lane_failure(self):
        self._make_consumer_directory()
        result, calls = self._run("consumer", fail="python3~bcr_dependency_closure.py=8")
        self.assertEqual(result.returncode, 8, result.stderr)
        self.assertEqual(calls, self._consumer_commands())
        self.assertIn("| Audit the configured closure | failed (exit 8) |",
                      self.summary.read_text())

    def test_consumer_without_its_workspace_fails_like_cd(self):
        result, calls = self._run("consumer")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(calls, self._consumer_commands()[:1])

    def test_cli_builds_proves_tests_and_packages_each_platform(self):
        for platform, short in (("linux-x86-64", "linux"), ("darwin-arm64", "macos")):
            with self.subTest(platform=platform):
                result, calls = self._run("cli", "--platform", platform)
                self.assertEqual(result.returncode, 0, result.stderr)
                workspace = str(self.workspace)
                expected = [
                    ["bazelisk", "build", "--lockfile_mode=update", "-c", "opt", "--copt=-Os",
                     *TARGETS, workspace],
                    ["bazelisk", "build", "--lockfile_mode=error", "-c", "opt", "--copt=-Os",
                     *TARGETS, workspace],
                    ["bazelisk", "test", "--lockfile_mode=error", "-c", "opt", "--copt=-Os",
                     "//tools/ci:release_svg_notice_coverage", workspace],
                    ["python3", "-B", "-m", "tools.release_cli", "package",
                     "--platform", platform, "--binary", "bazel-bin/donner/svg/tool/donner-cli",
                     "--notice", "bazel-bin/third_party/licenses/notice_cli.txt",
                     "--resources", "bazel-bin/tools/ci/release_cli_resources.assets",
                     "--compiler-probe", "bazel-bin/tools/ci/release_compiler_identity",
                     "--output", f"{self.temp}/donner-cli-{short}", "--commit", COMMIT,
                     "--run-id", "1234", "--attempt", "2", workspace],
                ]
                if platform == "darwin-arm64":
                    flags = ["-c", "opt", "--copt=-Os", "--config=geode", "--config=text-full",
                             "--//build_defs:enable_tracy=false",
                             f"--//build_defs:release_source_commit={COMMIT}"]
                    targets = ["//donner/editor:editor_macos_app",
                               "//third_party/licenses:notice_release_editor_txt",
                               "//tools/ci:release_compiler_identity",
                               "//donner/editor:editor_build_info_txt"]
                    expected.extend([
                        ["bazelisk", "build", "--lockfile_mode=update", *flags, *targets, workspace],
                        ["bazelisk", "build", "--lockfile_mode=error", *flags, *targets, workspace],
                        ["bazelisk", "test", "--lockfile_mode=error", *flags,
                         f"--test_env=DONNER_RELEASE_SOURCE_COMMIT={COMMIT}",
                         "//tools:editor_notice_coverage_tests",
                         "//tools:release_macos_editor_native_tests",
                         "//tools:editor_build_info_tests", workspace],
                        ["python3", "-B", "-m", "tools.release_macos_editor", "package",
                         "--app", "bazel-bin/donner/editor/Donner.app",
                         "--build-info", "bazel-bin/donner/editor/editor_build_info.txt",
                         "--notice", "bazel-bin/third_party/licenses/notice_release_editor.txt",
                         "--compiler-probe", "bazel-bin/tools/ci/release_compiler_identity",
                         "--output", f"{self.temp}/donner-editor-macos", "--commit", COMMIT,
                         "--run-id", "1234", "--attempt", "2", workspace],
                    ])
                self.assertEqual(calls, expected)

    def test_cli_stops_before_packaging_when_notice_coverage_fails(self):
        result, calls = self._run("cli", "--platform", "linux-x86-64", fail="bazelisk:test=3")
        self.assertEqual(result.returncode, 3)
        self.assertEqual([call[:2] for call in calls],
                         [["bazelisk", "build"], ["bazelisk", "build"], ["bazelisk", "test"]])

    def test_admission_validates_in_a_hash_locked_validator_environment(self):
        result, calls = self._run("admission", "--upstream", "../upstream-bcr")
        self.assertEqual(result.returncode, 0, result.stderr)
        temp, workspace = str(self.temp), str(self.workspace)
        self.assertEqual(calls, [
            ["python3", "-m", "venv", f"{temp}/bcr-validator", workspace],
            ["venv-pip", "install", "--require-hashes",
             "-r", "../upstream-bcr/tools/requirements_lock.txt", workspace],
            ["venv-python", "-B", "-m", "tools.bcr_admission", "--upstream", "../upstream-bcr",
             "--registry", f"{temp}/donner-bcr/registry",
             "--artifacts", f"{temp}/donner-bcr/artifacts", "--commit", COMMIT,
             "--report", f"{temp}/donner-bcr/admission.json", workspace],
        ])

    def test_admission_stops_when_the_requirements_do_not_install(self):
        result, calls = self._run("admission", "--upstream", "../upstream-bcr",
                                  fail="venv-pip=2")
        self.assertEqual(result.returncode, 2)
        self.assertEqual([call[0] for call in calls], ["python3", "venv-pip"])

    def test_verify_checks_both_clis_and_optionally_the_source_first(self):
        temp, workspace = str(self.temp), str(self.workspace)
        clis = [["python3", "-B", "-m", "tools.release_cli", "verify",
                 "--artifacts", f"{temp}/donner-bcr/cli-{short}", "--platform", platform,
                 "--commit", COMMIT, "--run-id", "1234", "--attempt", "2", workspace]
                for platform, short in (("linux-x86-64", "linux"), ("darwin-arm64", "macos"))]
        clis.append(["python3", "-B", "-m", "tools.release_macos_editor", "verify",
                     "--artifacts", f"{temp}/donner-bcr/editor-macos", "--commit", COMMIT,
                     "--run-id", "1234", "--attempt", "2", workspace])
        source = ["python3", "-B", "-m", "tools.bcr_source", "verify",
                  "--artifacts", f"{temp}/donner-bcr/source", "--commit", COMMIT, workspace]
        result, calls = self._run("verify", "--with-source")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [source, *clis])
        result, calls = self._run("verify")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, clis)
        result, calls = self._run("verify", "--with-source", fail="python3=4")
        self.assertEqual(result.returncode, 4)
        self.assertEqual(calls, [source])

    def test_explain_missing_cli_always_fails(self):
        for attempt, count in (("1", 1), ("3", 2), ("x", 1)):
            with self.subTest(attempt=attempt):
                result, calls = self._run("explain-missing-cli", GITHUB_RUN_ATTEMPT=attempt)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(calls, [])
                self.assertEqual(result.stdout.count("::error::"), count)

    def test_missing_run_settings_run_nothing(self):
        for args, variable in ((["consumer"], "RUNNER_TEMP"), (["consumer"], "SOURCE_COMMIT"),
                               (["cli", "--platform", "linux-x86-64"], "GITHUB_RUN_ID"),
                               (["verify"], "GITHUB_RUN_ATTEMPT")):
            with self.subTest(args=args, variable=variable):
                result, calls = self._run(*args, **{variable: ""})
                self.assertEqual(result.returncode, 2)
                self.assertEqual(calls, [])
        result, calls = self._run("cli", "--platform", "windows")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(calls, [])

    def test_no_step_narrows_the_matrix(self):
        # Every run, nightly, dispatched or on a pull request that changes a registry input, is
        # the full matrix that qualifies a release candidate; no subcommand selects a smaller one.
        result, calls = self._run("scope")
        self.assertEqual(result.returncode, 2)
        self.assertIn("invalid choice: 'scope'", result.stderr)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
