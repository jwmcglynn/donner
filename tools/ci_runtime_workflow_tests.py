"""Pins the self-hosted CI runtime boundaries that keep full runs viable."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import textwrap
import time
import unittest

from python.runfiles import runfiles


def _workflow_text(path):
    resolver = runfiles.Create()
    resolved = resolver.Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


class CiRuntimeWorkflowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bazelrc = _workflow_text(".bazelrc")
        cls.main = _workflow_text(".github/workflows/main.yml")
        cls.coverage = _workflow_text(".github/workflows/coverage.yml")
        cls.coverage_script = _workflow_text("tools/coverage.sh")

    def _job_body(self, job):
        marker = "\n  %s:\n" % job
        self.assertIn(marker, self.main, "job %s not found" % job)
        rest = self.main.split(marker, 1)[1]
        end = re.search(r"^  [A-Za-z0-9_-]+:\s*$", rest, re.MULTILINE)
        return rest[: end.start()] if end else rest

    def _heartbeat_script(self):
        match = re.search(
            r"cat > \"\$DIAG_DIR/run_bazel_with_heartbeat\.sh\" <<'EOF'\n"
            r"(?P<body>.*?)^\s+EOF$",
            self.main,
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(match, "heartbeat wrapper heredoc not found")
        return textwrap.dedent(match.group("body"))

    def _run_script(self, text, args, env=None, timeout=5):
        with tempfile.TemporaryDirectory() as temp_dir:
            script = Path(temp_dir) / "fixture.sh"
            script.write_text(text)
            script.chmod(0o755)
            started = time.monotonic()
            result = subprocess.run(
                [str(script), *args],
                check=False,
                capture_output=True,
                text=True,
                env=env,
                timeout=timeout,
            )
            return result, time.monotonic() - started

    def _inject_pre_publish_signal(self, script, sleep_command, hook):
        """Signal the watcher after sleep forks but before it publishes the PID."""
        self.assertEqual(1, script.count(sleep_command))
        return script.replace(sleep_command, f'{sleep_command}\n"{hook}"', 1)

    def _write_parent_signal_hook(self, directory):
        hook = directory / "signal-parent.sh"
        hook.write_text('#!/bin/sh\nprintf x >> "$0.count"\nkill -TERM "$PPID"\n')
        hook.chmod(0o755)
        return hook, Path(f"{hook}.count")

    def test_coverage_does_not_expand_ci_config_twice(self):
        """The coverage command inherits its CI config from the runner rc."""
        flag_line = re.search(
            r'^\s*DONNER_COVERAGE_BAZEL_FLAGS: "([^"]*)"$',
            self.coverage,
            re.MULTILINE,
        )
        self.assertIsNotNone(flag_line)
        self.assertNotIn("--config=ci", flag_line.group(1))

    def test_pr_test_lanes_exclude_manual_targets_at_the_command_line(self):
        """Runner bazelrcs cannot re-enable opt-in tests during full fallbacks."""
        self.assertIn("test:ci --test_tag_filters=-manual,-perf", self.bazelrc)
        for job_name in ("linux", "linux-self-hosted", "macos", "macos-self-hosted"):
            with self.subTest(job=job_name):
                job = self._job_body(job_name)
                self.assertEqual(1, job.count("--test_tag_filters=-manual,-perf"))

    def test_linker_canary_uses_bounded_native_apt_retries(self):
        """The hosted canary must not ask an unprivileged action to kill apt."""
        job = self._job_body("linker-canary")
        install = job.split("- name: Install system dependencies", 1)[1].split(
            "- name: Setup Bazel", 1
        )[0]

        self.assertNotIn("nick-fields/retry", install)
        self.assertIn("timeout-minutes: 15", install)
        self.assertEqual(2, install.count("Acquire::Retries=3"))
        self.assertNotIn("clang-tidy", install)

    def test_heartbeat_cleanup_is_prompt_without_ps(self):
        """A finished command cannot leave the heartbeat sleeper holding the pipe."""
        job = self._job_body("linux-self-hosted")
        self.assertNotIn("awk -v p=", job)
        self.assertNotIn('ps -o pid= --ppid "$root"', job)

        script = self._heartbeat_script()
        self.assertIn(
            'heartbeat_interval="${BAZEL_HEARTBEAT_INTERVAL_SECONDS:-60}"',
            script,
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            hook, hook_count = self._write_parent_signal_hook(temp_path)
            script = self._inject_pre_publish_signal(
                script, 'sleep "$heartbeat_interval" &', hook
            )
            fake_bin = Path(temp_dir) / "bin"
            fake_bin.mkdir()
            fake_ps = fake_bin / "ps"
            fake_ps.write_text("#!/bin/sh\nexit 127\n")
            fake_ps.chmod(0o755)
            log = Path(temp_dir) / "command.log"
            wrapper = Path(temp_dir) / "wrapper.sh"
            wrapper.write_text(script)
            wrapper.chmod(0o755)
            env = os.environ.copy()
            env["BAZEL_HEARTBEAT_INTERVAL_SECONDS"] = "5"
            env["PATH"] = f"{fake_bin}:{env['PATH']}"

            for iteration in range(4):
                started = time.monotonic()
                result = subprocess.run(
                    [str(wrapper), str(log), "bash", "-c", "exit 17"],
                    check=False,
                    capture_output=True,
                    text=True,
                    env=env,
                    timeout=3,
                )
                elapsed = time.monotonic() - started

                self.assertEqual(17, result.returncode, f"iteration {iteration}: {result.stderr}")
                self.assertLess(
                    elapsed, 2.0, f"iteration {iteration}: heartbeat sleeper delayed wrapper exit"
                )
                self.assertEqual("x" * (iteration + 1), hook_count.read_text())

    def test_coverage_cleanup_is_portable_and_prompt_without_ps(self):
        """Coverage remains portable to macOS and owns its heartbeat sleeper."""
        self.assertNotIn("awk -v p=", self.coverage_script)
        self.assertNotIn('ps -o pid= --ppid "$root"', self.coverage_script)
        self.assertIn("ps -A -o pid=,ppid=", self.coverage_script)

        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            hook, hook_count = self._write_parent_signal_hook(temp_path)
            functions = self._inject_pre_publish_signal(
                self.coverage_script.split("\nTARGETS=()", 1)[0],
                'sleep "$progress_interval" &',
                hook,
            )
            fixture = functions + """

ps() { return 127; }
DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS=5
export DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS
run_quiet_with_progress "fixture" "$1" bash -c 'exit 23'
"""
            log = str(temp_path / "coverage.log")
            for iteration in range(4):
                result, elapsed = self._run_script(fixture, [log], timeout=3)
                self.assertEqual(23, result.returncode, f"iteration {iteration}: {result.stderr}")
                self.assertLess(
                    elapsed,
                    2.0,
                    f"iteration {iteration}: coverage heartbeat sleeper delayed wrapper exit",
                )
                self.assertEqual("x" * (iteration + 1), hook_count.read_text())


if __name__ == "__main__":
    unittest.main()
