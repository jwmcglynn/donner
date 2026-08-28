"""Pins the self-hosted CI runtime boundaries that keep full runs viable."""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import textwrap
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
        cls.cmake = _workflow_text(".github/workflows/cmake.yml")
        cls.coverage = _workflow_text(".github/workflows/coverage.yml")
        cls.editor_wasm = _workflow_text(".github/workflows/editor_wasm.yml")
        cls.lint = _workflow_text(".github/workflows/lint.yml")
        cls.coverage_script = _workflow_text("tools/coverage.sh")
        cls.apt_install = _workflow_text(".github/actions/apt-install/action.yml")

    def _job_body(self, job):
        marker = "\n  %s:\n" % job
        self.assertIn(marker, self.main, "job %s not found" % job)
        rest = self.main.split(marker, 1)[1]
        end = re.search(r"^  [A-Za-z0-9_-]+:\s*$", rest, re.MULTILINE)
        return rest[: end.start()] if end else rest

    def _steps(self, workflow):
        """Every (step name, step body) pair in a workflow, in file order."""
        for part in re.split(r"^      - name: ", workflow, flags=re.MULTILINE)[1:]:
            name, _, body = part.partition("\n")
            yield name.strip(), body

    def _step_body(self, job, step_name):
        """Return one step's body from a job, stopping at the next step."""
        marker = "      - name: %s\n" % step_name
        self.assertIn(marker, job, "step %s not found" % step_name)
        rest = job.split(marker, 1)[1]
        end = re.search(r"^      - name: ", rest, re.MULTILINE)
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

    def _run_script(self, text, args, env=None, timeout=10):
        with tempfile.TemporaryDirectory() as temp_dir:
            script = Path(temp_dir) / "fixture.sh"
            script.write_text(text)
            script.chmod(0o755)
            return subprocess.run(
                [str(script), *args],
                check=False,
                capture_output=True,
                text=True,
                env=env,
                timeout=timeout,
            )

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

    def _coverage_jobs(self):
        """Every (name, body) pair for a top-level key in coverage.yml."""
        parts = re.split(
            r"^  ([A-Za-z0-9_-]+):\s*$", self.coverage, flags=re.MULTILINE
        )
        return list(zip(parts[1::2], parts[2::2]))

    def test_coverage_records_a_legitimate_skip_instead_of_announcing_a_report(self):
        """The all-skipped path must not name a file it did not write.

        `bazel coverage` legitimately produces no report when every selected
        target is incompatible with the lane's platform: nothing was measured,
        and the lane is satisfied. That path used to exit the script's inner
        subshell with 0, after which the tail of the script unconditionally
        announced "Filtered coverage report saved to .../filtered_report.dat".
        The upload step believed it, looked for that exact path with
        if-no-files-found: error, and failed the job on a file the script had
        just decided not to write.
        """
        self.assertIn(
            'echo "all_skipped" > "$COVERAGE_HTML_DIR/coverage_skipped"',
            self.coverage_script,
        )
        report_line = 'echo "Filtered coverage report saved to'
        self.assertEqual(1, self.coverage_script.count(report_line))
        guard = 'if [ -f "$COVERAGE_OUTPUT_DIR/coverage_skipped" ]; then'
        self.assertIn(guard, self.coverage_script)
        self.assertLess(
            self.coverage_script.index(guard),
            self.coverage_script.index(report_line),
            "the report announcement is not guarded by the skip marker",
        )

    def test_every_coverage_report_consumer_honours_a_legitimate_skip(self):
        """Discovered, not listed: anything reading the report must be gated.

        The report outcome is published as a step output and, for the
        cross-job handoff, as a job output. Every consumer of
        filtered_report.dat gates on one of them, so a legitimate skip cannot
        fail a lane by looking for a file that was correctly never written.
        """
        consumers = []
        for job_name, job_body in self._coverage_jobs():
            job_gated = re.search(
                r"^    if: .*report_written", job_body, re.MULTILINE
            ) is not None
            for step_name, step_body in self._steps(job_body):
                if "filtered_report.dat" not in step_body:
                    continue
                consumers.append("%s / %s" % (job_name, step_name))
                self.assertTrue(
                    job_gated
                    or "outputs.report_written == 'true'" in step_body,
                    "step %r in job %r consumes the coverage report without "
                    "honouring the report-written outcome" % (step_name, job_name),
                )
        self.assertGreaterEqual(
            len(consumers),
            3,
            "consumer discovery matched %r, which is fewer than exist; the "
            "match is stale and this test is no longer checking anything"
            % (consumers,),
        )

    def test_coverage_excludes_all_opt_in_test_tags(self):
        """Coverage must not run manual/perf tests through its own override."""
        match = re.search(
            r"^coverage --test_tag_filters=([^\n]+)$", self.bazelrc, re.MULTILINE
        )
        self.assertIsNotNone(match)
        self.assertEqual(
            {"-fuzz_target", "-lint", "-manual", "-perf"},
            set(match.group(1).split(",")),
        )
        query_match = re.search(
            r'attr\("tags", "[^"]*\(([^)]+)\)[^"]*"', self.coverage
        )
        self.assertIsNotNone(query_match)
        self.assertEqual(
            {tag.removeprefix("-") for tag in match.group(1).split(",")},
            set(query_match.group(1).split("|")),
        )

    def test_every_change_based_test_step_handles_an_empty_selection(self):
        """A selection with no test targets must not fail a change-based lane.

        `bazel test` exits 4 ("No test targets were found, yet testing was
        requested") when the target patterns it is given contain no tests. A
        change-based lane can hand it exactly that -- a docs filegroup, a
        tool-only change, a diff confined to a vendored subtree tested from its
        own workspace -- and the resulting red is unfixable by rerunning or by
        editing the change. The wrapper turns only that one status into success,
        while an empty pattern list (a derivation defect, not an empty test set)
        still fails hard.

        The lanes are DISCOVERED, not listed: any step that tests the derived
        target set is covered by this, including one added after this was
        written.
        """
        covered = []
        for name, body in self._steps(self.main):
            if "outputs.affected" not in body:
                continue
            if not re.search(r"bazelisk(?:\s+--\S+)*\s+test\b", body):
                continue
            covered.append(name)
            self.assertIn(
                "tools/ci_bazel_test.sh",
                body,
                "step %r tests the derived target set without tolerating an "
                "empty test selection" % name,
            )
            self.assertIn(
                'if [[ -z "${TARGETS// /}" ]]; then',
                body,
                "step %r tests the derived target set without rejecting an "
                "empty target list" % name,
            )
        self.assertGreaterEqual(
            len(covered),
            4,
            "step discovery matched %r, which is fewer lanes than exist; the "
            "match is stale and this test is no longer checking anything"
            % (covered,),
        )

    def test_fixed_target_lanes_do_not_tolerate_an_empty_test_selection(self):
        """A hardcoded target list that yields no tests is a real defect.

        The Geode editor lanes name their tests literally, so an empty test set
        there means a target was renamed or deleted out from under the lane.
        That must stay red rather than inherit the change-based lanes' notice.
        """
        for job_name in ("macos", "macos-self-hosted"):
            with self.subTest(job=job_name):
                step = self._step_body(
                    self._job_body(job_name), "Test (Geode editor lane)"
                )
                self.assertNotIn("tools/ci_bazel_test.sh", step)

    def test_linker_canary_uses_bounded_native_apt_retries(self):
        """The hosted canary must not ask an unprivileged action to kill apt.

        The bound now lives in the shared action rather than the step: retrying
        inside the shell means nothing external has to signal a sudo-owned
        process, which is the failure this contract exists to prevent.
        """
        job = self._job_body("linker-canary")
        install = job.split("- name: Install system dependencies", 1)[1].split(
            "- name: Setup Bazel", 1
        )[0]

        self.assertNotIn("nick-fields/retry", install)
        self.assertIn("uses: ./.github/actions/apt-install", install)
        self.assertNotIn("clang-tidy", install)

    def test_shared_apt_action_bounds_and_retries_without_external_kill(self):
        """Every apt install inherits the bound, so no lane can regress alone."""
        # No wrapper that would have to kill a sudo-owned process.
        self.assertNotIn("nick-fields/retry", self.apt_install)
        # A stuck apt is bounded by a timeout the shell itself owns.
        self.assertIn("timeout --signal=TERM", self.apt_install)
        self.assertIn('attempt-timeout', self.apt_install)
        # Transport-level retries cover a single flaky fetch; the surrounding
        # loop covers apt failing or hanging outright.
        self.assertEqual(2, self.apt_install.count("Acquire::Retries=3"))
        self.assertIn("attempt=$((attempt + 1))", self.apt_install)

        # No surviving wrapper wraps an apt install. A wrapper around brew is
        # fine and still present: brew does not run under sudo, so the kill
        # that fails here would succeed there.
        for block in self.main.split("uses: nick-fields/retry")[1:]:
            self.assertNotIn("apt-get", block.split("- name:", 1)[0])

    def test_cmake_generator_validation_retries_only_after_failure(self):
        """Transient Bazel query failures get one retry without masking a persistent failure."""
        for workflow_name, workflow in (("CMake", self.cmake), ("Lint", self.lint)):
            with self.subTest(workflow=workflow_name):
                self.assertEqual(
                    2,
                    workflow.count("run: python3 tools/cmake/gen_cmakelists.py --check"),
                )
                self.assertIn("- id: cmake-generator-check", workflow)
                self.assertIn("continue-on-error: true", workflow)
                self.assertEqual(
                    1,
                    workflow.count(
                        "if: steps.cmake-generator-check.outcome == 'failure'"
                    ),
                )

    def test_editor_wasm_prefetch_retries_before_single_attempt_tests(self):
        """Wasm dependency fetches retry without retrying build or test failures."""
        workflow = self.editor_wasm
        self.assertIn("- id: editor-wasm-prefetch", workflow)
        self.assertIn("- name: Build and size-check Geode editor Wasm package", workflow)
        self.assertIn("- name: Stage package for handoff", workflow)
        prefetch = workflow.split("- id: editor-wasm-prefetch", 1)[1].split(
            "- name: Build and size-check Geode editor Wasm package", 1
        )[0]
        build = workflow.split(
            "- name: Build and size-check Geode editor Wasm package", 1
        )[1].split("- name: Stage package for handoff", 1)[0]

        self.assertEqual(6, prefetch.count("bazelisk fetch"))
        self.assertEqual(1, prefetch.count("continue-on-error: true"))
        self.assertEqual(
            1,
            prefetch.count("if: steps.editor-wasm-prefetch.outcome == 'failure'"),
        )
        self.assertEqual(3, build.count("bazelisk test"))
        self.assertNotIn("continue-on-error", build)

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
            log = Path(temp_dir) / "command.log"
            wrapper = Path(temp_dir) / "wrapper.sh"
            wrapper.write_text(script)
            wrapper.chmod(0o755)
            env = os.environ.copy()
            env["BAZEL_HEARTBEAT_INTERVAL_SECONDS"] = "300"

            result = subprocess.run(
                [str(wrapper), str(log), "bash", "-c", "exit 17"],
                check=False,
                capture_output=True,
                text=True,
                env=env,
                timeout=5,
            )

            self.assertEqual(17, result.returncode, result.stderr)
            self.assertEqual([], list(Path(temp_dir).glob("*.heartbeat-control.*")))

    def test_progress_watchers_use_interruptible_control_channels(self):
        """Command completion must wake progress waits without a sleeper race."""
        script = self._heartbeat_script()
        self.assertNotIn('sleep "$heartbeat_interval" &', script)
        self.assertIn('read -r -t "$heartbeat_interval"', script)
        self.assertNotIn('sleep "$progress_interval" &', self.coverage_script)
        self.assertIn('read -r -t "$progress_interval"', self.coverage_script)

    def test_coverage_cleanup_is_portable_and_prompt_without_ps(self):
        """Coverage remains portable and wakes its progress watcher promptly."""
        self.assertNotIn("awk -v p=", self.coverage_script)
        self.assertNotIn('ps -o pid= --ppid "$root"', self.coverage_script)
        self.assertIn("ps -A -o pid=,ppid=", self.coverage_script)

        with tempfile.TemporaryDirectory() as temp_dir:
            functions = self.coverage_script.split("\nTARGETS=()", 1)[0]
            fixture = functions + """

ps() { return 127; }
DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS=300
export DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS
run_quiet_with_progress "fixture" "$1" bash -c 'exit 23'
"""
            log = str(Path(temp_dir) / "coverage.log")
            result = self._run_script(fixture, [log], timeout=5)
            self.assertEqual(23, result.returncode, result.stderr)
            self.assertEqual([], list(Path(temp_dir).glob("*.progress-control.*")))


if __name__ == "__main__":
    unittest.main()
