"""Pins the self-hosted CI runtime boundaries that keep full runs viable."""

import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import textwrap
import unittest

from python.runfiles import runfiles


# The editor Wasm payload ceilings CI enforces. Changing one means editing the size test's
# arguments and this table together, so a widened ceiling is always a reviewed change.
ENFORCED_PAYLOAD_CEILINGS = {
    "--max-js-gzip-bytes": 51000,
    "--max-js-raw-bytes": 185000,
    "--max-total-raw-bytes": 12100000,
    "--max-wasm-data-segments": 64,
    "--max-wasm-function-body-bytes": 46000,
    "--max-wasm-gzip-bytes": 3300000,
    "--max-wasm-raw-bytes": 10000000,
}
PAYLOAD_SIZE_TEST = "//donner/editor/wasm:wasm_geode_package_size_tests"
SIZE_CHECK_STEP = "- name: Build and size-check Geode editor Wasm package"
STEP_AFTER_SIZE_CHECK = "- name: Stage package for handoff"
def _repository_text(path):
    resolver = runfiles.Create()
    resolved = resolver.Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


def _declared_payload_ceilings(size_test_arguments):
    """The payload ceilings the size test declares, read from its build-graph arguments.

    A flag declared twice is dropped rather than reported: argparse keeps the
    last value, so a single reading of a repeated flag would bless a ceiling the
    test does not enforce.
    """
    ceilings = {}
    for flag in ENFORCED_PAYLOAD_CEILINGS:
        values = re.findall(r'"%s", "(\d+)"' % re.escape(flag), size_test_arguments)
        if len(values) == 1:
            ceilings[flag] = int(values[0])
    return ceilings


class CiRuntimeWorkflowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bazelrc = _repository_text(".bazelrc")
        cls.main = _repository_text(".github/workflows/main.yml")
        cls.cmake = _repository_text(".github/workflows/cmake.yml")
        cls.coverage = _repository_text(".github/workflows/coverage.yml")
        cls.editor_wasm = _repository_text(".github/workflows/editor_wasm.yml")
        cls.lint = _repository_text(".github/workflows/lint.yml")
        cls.coverage_script = _repository_text("tools/coverage.sh")
        cls.apt_install = _repository_text(".github/actions/apt-install/action.yml")
        cls.wasm_size_test_arguments = _repository_text(
            "donner/editor/wasm/wasm_package_size_test_arguments.txt"
        )
        cls.ci_target_definitions = _repository_text("tools/ci/BUILD.bazel")
        cls.editor_tests_build = _repository_text("donner/editor/tests/BUILD.bazel")
        cls.xvfb_runner = _repository_text(
            "donner/editor/tests/RunEditorWindowVulkanSurfaceTests.sh"
        )
        cls.ci_bazel_test = _repository_text("tools/ci_bazel_test.sh")

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

    def test_hosted_browser_comparisons_preserve_the_failed_gate(self):
        hosted = self._job_body("macos")
        original = self._step_body(hosted, "Test")
        self.assertNotIn("continue-on-error", original)
        original_artifacts = hosted.index("      - name: Upload Bazel test failure artifacts")
        detection = self._step_body(hosted, "Detect browser GPU acquisition failure")
        self.assertIn("stage=selection outcome=deadline_pending", detection)
        # The browser test recorder prints this marker; see browser-stall-diagnostics.mjs.
        self.assertIn("browser-stall-diagnostics deadline", detection)
        stall_recorder = _repository_text("donner/editor/wasm/tests/browser-stall-diagnostics.mjs")
        self.assertIn('kStallMarker = "browser-stall-diagnostics"', stall_recorder)
        self.assertIn("return `${kStallMarker} deadline status=${test.status} durationMs=",
                      stall_recorder)
        self.assertIn("run_probe=$should_probe", detection)
        self.assertIn("ci:browser-gpu-diagnostics", detection)
        self.assertIn("BROWSER_GPU_DIAGNOSTICS_REQUESTED:", detection)
        self.assertIn('should_probe="$BROWSER_GPU_DIAGNOSTICS_REQUESTED"', detection)
        targets = (
            "chromium_remote_smoke", "catalog_font_loading_test",
            "browser_presentation_regression_test", "standalone_geode_browser_renderer_test",
        )
        metadata = _repository_text("tools/ci/BUILD.bazel")
        suite = re.search(r'test_suite\(\s+name = "browser_gpu_comparison",.*?\n\)',
                          metadata, re.DOTALL)
        self.assertIsNotNone(suite, "browser comparison suite must be declared")
        for target in targets:
            self.assertIn("//donner/editor/wasm/tests:" + target, suite.group())
        for mode, jobs in (("serial", 1), ("parallel", 4)):
            name = "Compare browser GPU tests (%s)" % mode
            body = self._step_body(hosted, name)
            self.assertGreater(hosted.index("      - name: " + name), original_artifacts)
            self.assertIn("!cancelled() && steps.browser_gpu_failure.outputs.run_probe == 'true'", body)
            self.assertIn("continue-on-error: true", body)
            self.assertIn("--nocache_test_results", body)
            self.assertIn("--local_test_jobs=%d" % jobs, body)
            self.assertIn("$BAZEL_MACOS_BUILD_FLAGS $BAZEL_MACOS_TEST_FLAGS", body)
            self.assertIn("//tools/ci:browser_gpu_comparison", body)
            artifact = self._step_body(hosted, "Upload browser GPU comparison (%s)" % mode)
            self.assertIn("./.github/actions/upload-bazel-test-artifacts", artifact)
            self.assertIn("browser-gpu-%s-" % mode, artifact)

    def test_hosted_browser_comparisons_keep_failure_reports_with_errexit(self):
        hosted = self._job_body("macos")
        environment = os.environ.copy()
        environment.update(RUNNER_TEMP=".", BAZEL_MACOS_BUILD_FLAGS="",
                           BAZEL_MACOS_TEST_FLAGS="", BROWSER_STALL_TEST_FLAGS="")
        prefix = "bazelisk() { return 3; }; python3() { echo summary-ran; return 0; };\n"
        for mode in ("serial", "parallel"):
            body = self._step_body(hosted, "Compare browser GPU tests (%s)" % mode)
            script = textwrap.dedent(body.split("run: |\n", 1)[1])
            result = subprocess.run(["/bin/bash", "-e", "-c", prefix + script],
                                    env=environment, capture_output=True, text=True)
            self.assertEqual(result.returncode, 3, result.stderr)
            self.assertIn("summary-ran", result.stdout)

    def test_browser_stall_system_log_is_hosted_only_and_follows_the_failure_archive(self):
        hosted = self._job_body("macos")
        name = "Collect browser stall system log"
        collect = self._step_body(hosted, name)
        position = hosted.index("      - name: " + name)
        self.assertGreater(position, hosted.index("      - name: Upload Bazel test failure artifacts"))
        # The comparison reruns overwrite these test logs, so the slice must come first.
        self.assertLess(position, hosted.index("      - name: Compare browser GPU tests (serial)"))
        self.assertIn("if: ${{ !cancelled() && steps.test.outcome == 'failure' }}", collect)
        self.assertIn("continue-on-error: true", collect)
        self.assertIn("timeout-minutes: 5", collect)
        self.assertIn(
            "predicate='eventType == logEvent AND (process BEGINSWITH \"Google Chrome for Testing\" "
            "OR process == \"syspolicyd\" OR process == \"amfid\")'",
            collect,
        )
        upload = self._step_body(hosted, "Upload browser stall system log")
        self.assertIn("if: ${{ !cancelled() && steps.test.outcome == 'failure' }}", upload)
        self.assertIn("path: ${{ runner.temp }}/browser-stall-system-log", upload)
        self.assertIn("retention-days: 14", upload)
        # A self-hosted runner's unified log describes a persistent host; it is never published.
        for job in ("macos-self-hosted", "linux", "linux-self-hosted"):
            body = self._job_body(job)
            self.assertNotIn(name, body)
            self.assertNotIn("log show", body)

    def test_browser_stall_recorder_is_enabled_only_in_the_hosted_job(self):
        # The recorder publishes host load, memory and call graphs, so only the ephemeral
        # hosted job may enable it; every other lane and local runs leave it inert.
        switch = '--test_env=DONNER_BROWSER_STALL_DIAGNOSTICS=1'
        hosted = self._job_body("macos")
        self.assertIn('      BROWSER_STALL_TEST_FLAGS: "%s"\n' % switch, hosted)
        steps = ("Test", "Compare browser GPU tests (serial)",
                 "Compare browser GPU tests (parallel)")
        for step in steps:
            self.assertIn("$BAZEL_MACOS_TEST_FLAGS $BROWSER_STALL_TEST_FLAGS",
                          self._step_body(hosted, step), step)
        recorder = _repository_text("donner/editor/wasm/tests/browser-stall-diagnostics.mjs")
        self.assertIn('kEnableVariable = "DONNER_BROWSER_STALL_DIAGNOSTICS"', recorder)
        self.assertEqual(self.main.count("DONNER_BROWSER_STALL_DIAGNOSTICS"), 1)
        self.assertEqual(self.main.count("BROWSER_STALL_TEST_FLAGS"), 1 + len(steps))
        for job in ("macos-self-hosted", "linux", "linux-self-hosted"):
            self.assertNotIn("BROWSER_STALL", self._job_body(job), job)
        resolver = runfiles.Create()
        workflows = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent
        others = [path for path in sorted(workflows.glob("*.y*ml")) if path.name != "main.yml"]
        self.assertGreater(len(others), 5)
        # A composite action or the repository bazelrc would enable it on every lane.
        actions = sorted((workflows.parent / "actions").glob("*/action.y*ml"))
        self.assertGreater(len(actions), 3)
        for path in others + actions:
            self.assertNotIn("BROWSER_STALL", path.read_text(encoding="utf-8"), path.name)
        self.assertNotIn("BROWSER_STALL", self.bazelrc)

    def test_browser_stall_system_log_slices_only_well_formed_windows(self):
        collect = self._step_body(self._job_body("macos"), "Collect browser stall system log")
        script = textwrap.dedent(collect.split("run: |\n", 1)[1])
        # Records each invocation and stands in for the host's unified log; the window
        # starting at 03:40 fails, as an unreadable log would.
        prefix = ('log() { printf "%s|" "$@"; printf "\\n";'
                  ' [[ "$*" != *"03:40:00+0000"* ]]; }\n')
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            windows = {
                "a": "2026-10-04T03:24:35Z 2026-10-04T03:25:35Z\n",
                "b": "2026-10-04T03:24:35Z 2026-10-04 03:25:35\n",
                "c": "2026-10-04T03:30:00Z 2026-10-04T03:31:00Z\n",
                "d": "2026-10-04T03:40:00Z 2026-10-04T03:41:00Z\n",
                "e": "2026-10-04T03:50:00Z 2026-10-04T03:51:00Z\n",
                "f": "2026-10-04T04:00:00Z 2026-10-04T04:01:00Z\n",
            }
            for target, window in windows.items():
                directory = (root / "bazel-testlogs" / target / "test.outputs" / "playwright"
                             / "case" / "browser-stall")
                directory.mkdir(parents=True)
                (directory / "system-log-window.txt").write_text(window)
            environment = dict(os.environ, RUNNER_TEMP=str(root))
            result = subprocess.run(["/bin/bash", "-c", prefix + script], cwd=temp_dir,
                                    env=environment, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Skipping malformed window: bazel-testlogs/b/", result.stdout)
            self.assertIn("System log slice 3 failed: bazel-testlogs/d/", result.stdout)
            self.assertIn("Collected 4 browser stall system log window(s).", result.stdout)
            out = root / "browser-stall-system-log"
            self.assertEqual(sorted(path.name for path in out.iterdir()),
                             ["window-%d.log" % index for index in range(1, 5)])
            self.assertEqual(
                (out / "window-1.log").read_text().split("|")[:7],
                ["show", "--style", "compact", "--timezone", "UTC",
                 "--start", "2026-10-04 03:24:35+0000"],
            )
            self.assertIn("2026-10-04 03:51:00+0000", (out / "window-4.log").read_text())
            self.assertEqual(list(root.glob("browser-stall-window-*.raw")), [])

    def _heartbeat_script(self):
        match = re.search(
            r"cat > \"\$DIAG_DIR/run_bazel_with_heartbeat\.sh\" <<'EOF'\n"
            r"(?P<body>.*?)^\s+EOF$",
            self.main,
            re.MULTILINE | re.DOTALL,
        )
        self.assertIsNotNone(match, "heartbeat wrapper heredoc not found")
        return textwrap.dedent(match.group("body"))

    def _run_script(self, text, args, env=None, timeout=10, cwd=None):
        with tempfile.TemporaryDirectory() as temp_dir:
            script = Path(temp_dir) / "fixture.sh"
            script.write_text(text)
            script.chmod(0o755)
            python = Path(temp_dir) / "python3"
            python.write_text('#!/bin/sh\nexec "$FIXTURE_PYTHON" "$@"\n')
            python.chmod(0o755)
            fixture_env = dict(os.environ if env is None else env)
            fixture_env["FIXTURE_PYTHON"] = sys.executable
            fixture_env["PATH"] = temp_dir + os.pathsep + fixture_env.get("PATH", os.defpath)
            return subprocess.run(
                ["/bin/bash", str(script), *args],
                check=False,
                capture_output=True,
                text=True,
                env=fixture_env,
                timeout=timeout,
                cwd=cwd,
            )

    def test_wasm_build_and_browser_checks_share_one_runner_and_package(self):
        self.assertNotIn("\n  test:\n", self.editor_wasm)
        self.assertNotIn("actions/download-artifact@", self.editor_wasm)
        self.assertIn("PACKAGE_TARGET: ${{ steps.stage-package.outputs.package_target }}", self.editor_wasm)
        self.assertIn('push:\n    branches: ["main"]', self.editor_wasm)
        self.assertIn("uses: ./.github/actions/cache-bazel-actions", self.editor_wasm)
        self.assertIn("cache: npm", self.editor_wasm)
        self.assertEqual(1, self.editor_wasm.count("uses: bazel-contrib/setup-bazel@"))
        setup_header = self.editor_wasm.split("uses: bazel-contrib/setup-bazel@", 1)[0]
        self.assertIn("Setup Bazel for native browser comparison", setup_header)
        self.assertLess(
            self.editor_wasm.index("uses: bazel-contrib/setup-bazel@"),
            self.editor_wasm.index("- name: Serve Geode package and run browser suites"),
        )

    def test_linux_cmake_consumer_has_one_pr_and_main_owner(self):
        linux = self.cmake.split("\n  linux:\n", 1)[1].split("\n  macos:\n", 1)[0]
        admission = linux.split("    env:\n", 1)[0]
        self.assertIn("github.event_name == 'push'", admission)
        self.assertIn("github.ref != 'refs/heads/main'", admission)
        consumer = self._step_body(self._job_body("linux"), "Build and test CMake consumer")
        self.assertIn("python3 tools/cmake/gen_cmakelists.py --check", consumer)
        self.assertIn("cmake -S examples/cmake_consumer -B build -G Ninja", consumer)
        for body in (linux, consumer):
            self.assertIn("cmake --build build --target donner_cmake_consumer", body)
            self.assertIn("ctest --test-dir build --output-on-failure", body)

    def test_retired_closure_scanner_is_not_invoked(self):
        self.assertNotIn("configured_rust_closure.py", self.main)
        self.assertNotIn("configured-rust-closure-", self.main)

    def test_platform_checks_reuse_native_build_jobs(self):
        for redundant in ("configured-rust-closure-platform", "no-rust-configured-closure",
                          "linux-local-xvfb", "linker-canary"):
            self.assertNotIn("\n  %s:\n" % redundant, self.main)
        self.assertIn("//tools/ci:linker_canary", self._job_body("linux"))

    def test_native_window_gate_runs_on_hosted_linux_when_remote_routing_is_selected(self):
        hosted = self._job_body("linux")
        local = hosted
        remote = self._job_body("linux-self-hosted")
        self.assertNotIn("use_self_hosted_linux", hosted.split("steps:", 1)[0])
        self.assertIn("use_self_hosted_linux == 'true'", self._step_body(local, "Test local Xvfb targets"))
        self.assertIn("xvfb xauth", hosted)
        self.assertIn("xvfb xauth", local)
        self.assertIn("-local-gpu-isolated", remote)
        target = self.editor_tests_build.split(
            'name = "editor_window_vulkan_surface_tests",', 1
        )[1].split("\n)\n", 1)[0]
        for tag in ("exclusive-if-local", "local-gpu-isolated", "requires_xvfb"):
            self.assertIn('"%s"' % tag, target)
        self.assertIn('data = [":editor_window_tests_geode"]', target)
        self.assertIn("export DONNER_GPU_BACKEND=vulkan", self.xvfb_runner)
        self.assertIn("xvfb-run -a", self.xvfb_runner)

        step = self._step_body(local, "Test local Xvfb targets")
        self.assertIn("tools/ci_bazel_test.sh bazelisk test", step)
        self.assertIn("--test_tag_filters=requires_xvfb", step)
        self.assertIn("--strategy=TestRunner=local", step)
        script_template = textwrap.dedent(step.split("run: |\n", 1)[1])
        native_target = "//donner/editor/tests:editor_window_vulkan_surface_tests"
        for fallback, affected, expected_target, no_tests in (
            ("false", native_target, native_target, False),
            ("false", "//donner/base:base_tests", "//donner/base:base_tests", True),
            ("true", "", "//...", False),
        ):
            with self.subTest(fallback=fallback, affected=affected):
                script = script_template.replace(
                    "${{ needs.determine-targets.outputs.fallback }}", fallback
                ).replace(
                    "${{ needs.determine-targets.outputs.affected }}", affected
                )
                with tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    (root / "tools").mkdir()
                    wrapper = root / "tools/ci_bazel_test.sh"
                    wrapper.write_text(self.ci_bazel_test)
                    wrapper.chmod(0o755)
                    fake_bazel = root / "bazelisk"
                    fake_bazel.write_text(
                        '#!/bin/bash\nprintf "%s\\n" "$*" > "$BAZEL_ARGS"\n'
                        'case " $* " in\n'
                        '  *" //... "*|*" //donner/editor/tests:editor_window_vulkan_surface_tests "*) exit 0 ;;\n'
                        '  *) exit 4 ;;\n'
                        'esac\n'
                    )
                    fake_bazel.chmod(0o755)
                    args_file = root / "args.txt"
                    env = {**os.environ, "BAZEL_ARGS": str(args_file),
                           "PATH": str(root) + os.pathsep + os.environ.get("PATH", "")}
                    result = subprocess.run(
                        ["/bin/bash", "-c", script], cwd=root, env=env,
                        capture_output=True, text=True, timeout=10,
                    )
                    self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                    args = args_file.read_text()
                    self.assertIn("--test_tag_filters=requires_xvfb", args)
                    self.assertIn(expected_target, args)
                    self.assertEqual(no_tests, "No test targets" in result.stdout)

    def test_shell_fixture_uses_the_test_interpreter(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            python = root / "python3"
            python.write_text("#!/bin/sh\necho ambient-python-was-used >&2\nexit 97\n")
            python.chmod(0o755)
            result = self._run_script(
                "#!/bin/bash\npython3 -c 'import sys; print(sys.executable)'\n",
                [],
                env={"PATH": str(root) + os.pathsep + os.environ["PATH"]},
            )
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertEqual(sys.executable, result.stdout.strip())

    def test_metal_profile_selection_is_bounded_and_precedes_the_full_build(self):
        hosted = self._job_body("macos")
        fetching = self._step_body(hosted, "Fetch Metal validation dependencies")
        self.assertIn("nick-fields/retry@v4", fetching)
        self.assertIn("bazelisk fetch --config=ci", fetching)
        self.assertIn("max_attempts: 3", fetching)
        self.assertNotIn("bazelisk test", fetching)
        self.assertLess(hosted.index("Fetch Metal validation dependencies"),
                        hosted.index("Select Metal validation profile"))
        selection = self._step_body(hosted, "Select Metal validation profile")
        self.assertIn("//tools/ci:metal_profile", selection)
        self.assertIn("tools/metal_validation_profile.py", selection)
        self.assertIn("--nocache_test_results", selection)
        case_start = selection.index('          case "$profile" in')
        case_end = selection.index("          esac", case_start) + len("          esac")
        script = "#!/bin/bash\nset -euo pipefail\n" + textwrap.dedent(selection[case_start:case_end])
        for profile, expected_status, expected_flags in (
            ("full", 0, ""),
            ("paravirtual-texture-usage-off", 0,
             "BAZEL_MACOS_TEST_FLAGS=--test_env=HOME --test_env=MTL_SHADER_VALIDATION_TEXTURE_USAGE=0\n"),
            ("unknown", 1, ""),
            ("", 1, ""),
        ):
            with self.subTest(profile=profile), tempfile.TemporaryDirectory() as temporary:
                env_file = Path(temporary) / "environment"
                summary = Path(temporary) / "summary"
                result = self._run_script(script, [], env={
                    "profile": profile, "GITHUB_ENV": str(env_file),
                    "GITHUB_STEP_SUMMARY": str(summary),
                })
                self.assertEqual(expected_status, result.returncode, result.stderr)
                self.assertEqual(expected_flags, env_file.read_text() if env_file.exists() else "")
        for job_name in ("macos", "macos-self-hosted"):
            job = self._job_body(job_name)
            preflight = self._step_body(job, "Metal validation preflight")
            self.assertIn("--nocache_test_results", preflight)
            self.assertLess(job.index("Metal validation preflight"), job.index("- name: Build"))
            self.assertIn("//tools/ci:metal_preflight", preflight)
            self.assertIn("steps.metal_preflight.outcome == 'failure'", job)
        trusted = self._job_body("macos-self-hosted")
        required = self._step_body(trusted, "Require full Metal shader validation")
        self.assertIn("--test_tag_filters=", required)
        self.assertIn("--nocache_test_results", required)
        self.assertIn("//tools/ci:metal_full_validation", required)
        self.assertNotIn("MTL_SHADER_VALIDATION_TEXTURE_USAGE=0", trusted)

    def test_bazel_version_change_selects_full_suite_before_graph_hashing(self):
        """A Bazel upgrade changes the query graph, not just two version files."""
        job = self._job_body("determine-targets")
        start = job.index("          should_fallback=false\n")
        end = job.index('          work_dir="$(mktemp -d)"', start)
        script = "#!/usr/bin/env bash\nset -euo pipefail\n"
        script += textwrap.dedent(job[start:end])
        script += 'echo "incremental=true" >> "$GITHUB_OUTPUT"\n'
        for changed_files, expected in (
            (".bazelversion\nexamples/.bazelversion", "fallback=true\naffected=\n"),
            ("MODULE.bazel", "fallback=true\naffected=\n"),
            ("donner/base/Utils.h", "incremental=true\n"),
        ):
            with self.subTest(changed_files=changed_files):
                with tempfile.TemporaryDirectory() as temp_dir:
                    output = Path(temp_dir) / "outputs"
                    result = self._run_script(
                        script, [],
                        env={**os.environ, "changed_files": changed_files,
                             "GITHUB_OUTPUT": str(output)},
                    )
                    self.assertEqual(0, result.returncode, result.stderr)
                    self.assertEqual(expected, output.read_text())

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

    def test_linux_wgpu_resvg_reference_is_selected_only_for_relevant_prs(self):
        """The manual comparison and dependency audit stay in the explicit Linux suite."""
        determine = self._job_body("determine-targets")
        self.assertIn("wgpu_reference: ${{ steps.select_wgpu_resvg.outputs.run }}", determine)
        start = determine.index("          # The manual reference is removed")
        end = determine.index('          manual_affected="', start)
        impacted_script = "#!/usr/bin/env bash\nset -euo pipefail\n"
        impacted_script += textwrap.dedent(determine[start:end])
        self.assertIn("labels(dep, tests(//tools/ci:linux_wgpu_resvg_reference))", impacted_script)
        suite = self.ci_target_definitions.split('name = "linux_wgpu_resvg_reference"', 1)[1]
        suite = suite.split("\n)", 1)[0]
        labels = re.findall(r'"(//[^\"]+)"', suite)
        self.assertEqual(
            labels,
            [
                "//donner/svg/renderer/tests:resvg_test_suite_wgpu_reference_linux",
                "//donner/svg/renderer/tests:resvg_wgpu_reference_dependency_audit_test",
            ],
        )
        reference = labels[0]
        with tempfile.TemporaryDirectory() as directory:
            fake_bazelisk = Path(directory) / "bazelisk"
            fake_bazelisk.write_text(
                "#!/bin/sh\nprintf '%s\\n' '" + reference + "' '" + reference + "_impl'\n"
            )
            fake_bazelisk.chmod(0o755)
            for index, (affected, selected) in enumerate((
                (reference, True),
                (reference + "_impl", True),
                (reference + "_extra", False),
                ("//donner/svg/renderer/tests:unrelated_manual_test", False),
                ("//donner/base:base_tests", False),
            )):
                with self.subTest(affected=affected):
                    output = Path(directory) / ("outputs-%d" % index)
                    result = self._run_script(
                        impacted_script, [],
                        env={**os.environ, "affected": affected,
                             "head_dir": directory, "GITHUB_OUTPUT": str(output),
                             "PATH": directory + os.pathsep + os.environ.get("PATH", os.defpath)},
                    )
                    self.assertEqual(0, result.returncode, result.stderr)
                    self.assertEqual(
                        "wgpu_reference_affected=true\n" if selected else "",
                        output.read_text() if output.exists() else "",
                    )
            fake_bazelisk.write_text("#!/bin/sh\nexit 0\n")
            empty = self._run_script(
                impacted_script, [],
                env={**os.environ, "affected": reference, "head_dir": directory,
                     "GITHUB_OUTPUT": str(Path(directory) / "empty-outputs"),
                     "PATH": directory + os.pathsep + os.environ.get("PATH", os.defpath)},
            )
            self.assertEqual(1, empty.returncode)
            self.assertIn("Linux WebGPU resvg suite has no test target", empty.stdout)

        selector = self._step_body(determine, "Select Linux WebGPU resvg reference")
        self.assertIn("if: github.event_name == 'pull_request'", selector)
        path_start = determine.index("          # Direct renderer/GPU/image inputs")
        path_end = determine.index("          should_fallback=false", path_start)
        path_script = "#!/usr/bin/env bash\nset -euo pipefail\n"
        path_script += textwrap.dedent(determine[path_start:path_end])
        for path, expected in (
            ("donner/svg/renderer/RendererGeode.cc", "true"),
            ("donner/gpu/browser/BrowserDevice.cc", "true"),
            ("donner/gpu/shader/WgslEmitter.cc", "true"),
            ("donner/svg/resources/ImageLoader.cc", "true"),
            ("third_party/resvg-test-suite/tests/icon.svg", "true"),
            ("MODULE.bazel", "true"),
            (".github/workflows/main.yml", "true"),
            (".github/workflows/lint.yml", "false"),
            ("donner/base/Utils.h", "false"),
        ):
            with self.subTest(path=path), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "outputs"
                result = self._run_script(
                    path_script, [],
                    env={**os.environ, "changed_files": path, "GITHUB_OUTPUT": str(output)},
                )
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertEqual("wgpu_reference_changed=%s\n" % expected, output.read_text())

        script = textwrap.dedent(selector.split("        run: |\n", 1)[1])
        for impacted, changed, unknown, full_test, expected in (
            ("true", "false", "", "false", "true"),
            ("", "true", "", "false", "true"),
            ("", "false", "true", "false", "true"),
            ("", "false", "", "true", "true"),
            ("", "false", "", "false", "false"),
        ):
            with self.subTest(impacted=impacted, changed=changed,
                              unknown=unknown, full_test=full_test), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "outputs"
                configured = script.replace(
                    "${{ steps.determine.outputs.wgpu_reference_affected }}", impacted
                ).replace(
                    "${{ steps.determine.outputs.wgpu_reference_changed }}", changed
                ).replace(
                    "${{ steps.determine.outputs.wgpu_reference_unknown }}", unknown
                ).replace(
                    "${{ steps.determine.outputs.full_test }}", full_test
                )
                result = self._run_script(
                    "#!/usr/bin/env bash\n" + configured, [],
                    env={**os.environ, "GITHUB_OUTPUT": str(output)},
                )
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertEqual("run=%s\n" % expected, output.read_text())

        target = "//tools/ci:linux_wgpu_resvg_reference"
        for job_name in ("linux", "linux-self-hosted"):
            with self.subTest(job=job_name):
                job = self._job_body(job_name)
                step = self._step_body(job, "Test Linux WebGPU resvg reference")
                self.assertIn(
                    "needs.determine-targets.outputs.wgpu_reference == 'true'",
                    step,
                )
                self.assertIn("--test_tag_filters=", step)
                self.assertIn(target, step)
                self.assertIn("steps.wgpu_resvg_reference.outcome == 'failure'", job)
        for job_name in ("macos", "macos-self-hosted"):
            self.assertNotIn("Test Linux WebGPU resvg reference", self._job_body(job_name))

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

    def test_failed_hosted_coverage_retains_test_failure_artifacts(self):
        jobs = dict(self._coverage_jobs())
        step = self._step_body(jobs["build"], "Upload coverage test failure artifacts")
        self.assertIn("if: failure() && steps.coverage.outcome == 'failure'", step)
        self.assertIn("uses: ./.github/actions/upload-bazel-test-artifacts", step)
        self.assertIn("name: coverage-test-failure-${{ github.job }}", step)
        self.assertNotIn(
            "uses: ./.github/actions/upload-bazel-test-artifacts", jobs["coverage-self-hosted"]
        )

    def test_coverage_excludes_all_opt_in_test_tags(self):
        """Coverage must not run manual/perf tests through its own override."""
        match = re.search(
            r"^coverage --test_tag_filters=([^\n]+)$", self.bazelrc, re.MULTILINE
        )
        self.assertIsNotNone(match)
        self.assertEqual(
            {"-fuzz_target", "-lint", "-manual", "-perf", "-ci-remote-gpu"},
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
        """A named fixed test suite that yields no tests is a real defect.

        The Geode editor lanes select a fixed suite, so an empty test set
        there means its Bazel membership is broken.
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
        job = self._job_body("linux")
        install = job.split("- name: Install system dependencies", 1)[1].split(
            "- name: Setup Bazel", 1
        )[0]

        self.assertNotIn("nick-fields/retry", install)
        self.assertIn("uses: ./.github/actions/apt-install", install)

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
        prefetch = workflow.split("- id: editor-wasm-prefetch", 1)[1].split(SIZE_CHECK_STEP, 1)[0]
        build = self._size_check_step(workflow)

        self.assertEqual(8, prefetch.count("bazelisk fetch"))
        self.assertEqual(1, prefetch.count("continue-on-error: true"))
        self.assertEqual(
            1,
            prefetch.count("if: steps.editor-wasm-prefetch.outcome == 'failure'"),
        )
        self.assertEqual(4, build.count("bazelisk test"))
        self.assertNotIn("continue-on-error", build)
        self.assertIn("//tools/ci:editor_wasm_shipped_browser_audit", build)
        self.assertIn("//tools/ci:editor_wasm_size_tests", build)

    def test_editor_wasm_pull_requests_link_the_browser_gpu_bridge(self):
        """Both bridge probes, the standalone module and its default audit run on every PR.

        The module is built explicitly: it is incompatible without Browser selection, and an
        incompatible audit inside a test_suite is skipped rather than failed.
        """
        workflow = self.editor_wasm
        build_job = workflow.split("\n  build:\n", 1)[1].split("\n  test:\n", 1)[0]
        self.assertIn("- name: Link the browser GPU bridge", build_job)
        link = build_job.split("- name: Link the browser GPU bridge", 1)[1]
        build = link.split("bazelisk build --config=wasm-geode", 1)[1].split("bazelisk test", 1)[0]
        self.assertIn("//tools/ci:browser_bridge_link_probes", build)
        self.assertIn("//tools/ci:geode_wasm_browser_module", build)
        self.assertIn("bazelisk test --config=wasm-geode", link)
        self.assertIn("//tools/ci:geode_wasm_browser_default_audit", link)
        self.assertNotIn("continue-on-error", link)
        module = 'name = "geode_wasm_browser_module"'
        self.assertIn(module, self.ci_target_definitions)
        definition = self.ci_target_definitions.split(module, 1)[1].split("\n)", 1)[0]
        self.assertIn('srcs = ["//donner/svg/renderer/wasm:donner_wasm_geode"]', definition)

    def _size_check_step(self, workflow):
        """The workflow step that builds and size-checks the editor Wasm package.

        Both markers are asserted: without the terminator a renamed following
        step would silently widen this to the rest of the file, and every
        step-scoped assertion built on it would pass vacuously.
        """
        self.assertIn(SIZE_CHECK_STEP, workflow)
        self.assertIn(STEP_AFTER_SIZE_CHECK, workflow)
        return workflow.split(SIZE_CHECK_STEP, 1)[1].split(STEP_AFTER_SIZE_CHECK, 1)[0]

    def test_editor_wasm_payload_ceilings_are_enforced(self):
        """The size test declares each enforced ceiling once, and CI cannot relax it."""
        self.assertEqual(
            _declared_payload_ceilings(self.wasm_size_test_arguments),
            ENFORCED_PAYLOAD_CEILINGS,
            "%s and ENFORCED_PAYLOAD_CEILINGS must change together" % PAYLOAD_SIZE_TEST,
        )
        size_check_step = self._size_check_step(self.editor_wasm)
        self.assertNotIn("--test_arg", size_check_step)
        self.assertEqual(0, self.editor_wasm.count("payload-budget-mode"))

    def test_editor_wasm_payload_gate_detects_widened_or_repeated_ceilings(self):
        flag = "--max-wasm-raw-bytes"
        declared = '"%s", "%d"' % (flag, ENFORCED_PAYLOAD_CEILINGS[flag])
        self.assertIn(declared, self.wasm_size_test_arguments)
        widened = self.wasm_size_test_arguments.replace(
            declared, '"%s", "%d"' % (flag, ENFORCED_PAYLOAD_CEILINGS[flag] + 1)
        )
        self.assertEqual(
            ENFORCED_PAYLOAD_CEILINGS[flag] + 1, _declared_payload_ceilings(widened)[flag]
        )
        repeated = self.wasm_size_test_arguments.replace(declared, declared + ", " + declared)
        self.assertNotIn(flag, _declared_payload_ceilings(repeated))

    def test_editor_wasm_size_check_step_runs_the_gated_size_test(self):
        """The gate reads the ceilings of the target the workflow actually runs."""
        step = self._size_check_step(self.editor_wasm)
        self.assertIn("//tools/ci:editor_wasm_size_tests", step)
        suite_name = 'name = "editor_wasm_size_tests"'
        self.assertIn(suite_name, self.ci_target_definitions)
        suite = self.ci_target_definitions.split(suite_name, 1)[1].split("\n)", 1)[0]
        self.assertIn(PAYLOAD_SIZE_TEST, suite)

    def test_editor_wasm_handoff_resolves_artifact_and_provenance_from_metadata(self):
        stage = self.editor_wasm.split("- name: Stage package for handoff", 1)[1].split(
            "- name: Upload Geode package artifact", 1
        )[0]
        script = textwrap.dedent(stage.split("        run: |\n", 1)[1])
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = root / "package"
            package.mkdir()
            (package / "editor.wasm").write_bytes(b"package fixture")
            bazel = root / "bazelisk"
            bazel.write_text(
                '#!/bin/sh\n'
                'case "$1" in\n'
                'query) printf "%s\\n" "//fixture:package" ;;\n'
                'cquery) printf "%s\\n" "$PACKAGE_PATH" ;;\n'
                '*) exit 31 ;;\n'
                'esac\n'
            )
            bazel.chmod(0o755)
            output = root / "output"
            environment = dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"],
                               PACKAGE_PATH=str(package), RUNNER_TEMP=str(root),
                               GITHUB_OUTPUT=str(output))
            result = self._run_script("#!/bin/bash\n" + script, [], env=environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("package_target=//fixture:package\n", output.read_text())
            staged = root / "editor-wasm-package-geode" / "editor.wasm"
            self.assertEqual(staged.read_bytes(), b"package fixture")
            environment["PACKAGE_PATH"] = str(package) + "\n" + str(package)
            result = self._run_script("#!/bin/bash\n" + script, [], env=environment)
            self.assertNotEqual(result.returncode, 0, "ambiguous artifact outputs must fail")
        self.assertIn("PACKAGE_TARGET: ${{ steps.stage-package.outputs.package_target }}", self.editor_wasm)
        self.assertIn('\\"targets\\":[\\"${PACKAGE_TARGET}\\"]', self.editor_wasm)

    def _catalog_candidate_fixture(self, root):
        package = root / "editor-wasm-package-geode"
        (package / "fonts").mkdir(parents=True)
        for name in ("donner_icon.svg", "editor-bootstrap.js", "editor.css", "editor.js",
                     "enable-threads.js", "index.html", "catalog-fonts.js"):
            (package / name).write_text(name)
        (package / "editor.wasm").write_bytes(b"\0asm\1\0\0\0")
        (package / "CatalogFontNotices.txt").write_text("SIL OPEN FONT LICENSE Version 1.1")
        fonts = []
        families = ("Bebas Neue", "Bitter", "Inter", "JetBrains Mono", "Lato", "Lora",
                    "Montserrat", "Open Sans", "Oswald", "Pacifico", "Playfair Display",
                    "Roboto Mono")
        for index, family in enumerate(families):
            payload = bytearray([index] * 64)
            payload[:4] = b"wOF2"
            struct.pack_into(">I", payload, 16, 128)
            digest = hashlib.sha256(payload).hexdigest()
            path = "fonts/%s.woff2" % digest
            (package / path).write_bytes(payload)
            fonts.append({"family": family, "sha256": digest, "path": path,
                          "encoded_bytes": len(payload), "decoded_bytes": 128})
        (package / "catalog-fonts.json").write_text(json.dumps({
            "fonts": fonts, "encoded_bytes": 64 * len(fonts),
        }))
        wasm = root / "donner/editor/wasm"
        (wasm / "tests").mkdir(parents=True)
        (wasm / "tests/package-lock.json").write_text("{}")
        (wasm / "catalog_package_integrity_test.py").write_text(_repository_text(
            "donner/editor/wasm/catalog_package_integrity_test.py"))
        return package

    def _stage_catalog_candidate(self, root):
        body = self._step_body(self.editor_wasm, "Stage immutable Geode deployment candidate")
        script = "#!/bin/bash\n" + textwrap.dedent(body.split("        run: |\n", 1)[1])
        # The real staging job runs on macOS, where shasum is available. The
        # workflow contract test also runs on Linux, so provide the same
        # SHA-256 command from the fixture rather than relying on host tools.
        (root / "shasum_fixture.py").write_text(
            "import hashlib, sys\n"
            "assert sys.argv[1:3] == ['-a', '256']\n"
            "for name in sys.argv[3:]:\n"
            "    with open(name, 'rb') as stream:\n"
            "        digest = hashlib.sha256(stream.read()).hexdigest()\n"
            "    print(f'{digest}  {name}')\n"
        )
        shasum = root / "shasum"
        shasum.write_text('#!/bin/sh\nexec "$FIXTURE_PYTHON" "$FIXTURE_ROOT/shasum_fixture.py" "$@"\n')
        shasum.chmod(0o755)
        return self._run_script(script, [], cwd=root, env={
            **os.environ, "RUNNER_TEMP": str(root), "GITHUB_SHA": "a" * 40,
            "GITHUB_RUN_ID": "12", "GITHUB_RUN_ATTEMPT": "2",
            "PACKAGE_TARGET": "//fixture:package", "GITHUB_OUTPUT": str(root / "output"),
            "PATH": str(root) + os.pathsep + os.environ["PATH"],
            "FIXTURE_PYTHON": sys.executable, "FIXTURE_ROOT": str(root),
        })

    def test_editor_wasm_candidate_preserves_deferred_catalog_assets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            package = self._catalog_candidate_fixture(root)
            expected = {p.relative_to(package).as_posix(): p.read_bytes()
                        for p in package.rglob("*") if p.is_file()}
            result = self._stage_catalog_candidate(root)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            candidate = root / ("donner-editor-wasm-geode-" + "a" * 40)
            site = {p.relative_to(candidate / "site").as_posix(): p.read_bytes()
                    for p in (candidate / "site").rglob("*") if p.is_file()}
            self.assertEqual(site, expected)
            deploy = {p.relative_to(candidate / "deploy").as_posix(): p.read_bytes()
                      for p in (candidate / "deploy").rglob("*") if p.is_file()}
            self.assertEqual(gzip.decompress(deploy.pop("editor.wasm.gz")),
                             expected.pop("editor.wasm"))
            self.assertEqual(deploy, expected)
            checksums = dict(line.split("  ", 1)[::-1]
                             for line in (candidate / "SHA256SUMS").read_text().splitlines())
            expected_checksums = {p.relative_to(candidate).as_posix():
                                  hashlib.sha256(p.read_bytes()).hexdigest()
                                  for directory in (candidate / "site", candidate / "deploy")
                                  for p in directory.rglob("*") if p.is_file()}
            self.assertEqual(checksums, expected_checksums)
            provenance = json.loads((candidate / "provenance.json").read_text())
            self.assertEqual(provenance["source_revision"], "a" * 40)
            self.assertEqual(provenance["producer_run_id"], "12")
            self.assertEqual(provenance["producer_attempt"], "2")
            self.assertEqual(provenance["targets"], ["//fixture:package"])

    def test_editor_wasm_candidate_rejects_incomplete_or_unexpected_assets(self):
        for failure in ("corrupt-font", "extra-font", "extra-root-file", "missing-broker"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                package = self._catalog_candidate_fixture(root)
                if failure == "corrupt-font":
                    next((package / "fonts").glob("*.woff2")).write_bytes(b"corrupt")
                elif failure == "extra-font":
                    (package / "fonts/extra.woff2").write_bytes(b"unexpected")
                elif failure == "extra-root-file":
                    (package / "unexpected.txt").write_text("unexpected")
                else:
                    (package / "catalog-fonts.js").unlink()
                result = self._stage_catalog_candidate(root)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse((root / "output").exists())

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

    def test_quiet_coverage_progress_hides_raw_targets_and_runner_paths(self):
        functions = self.coverage_script.split("\nTARGETS=()", 1)[0]
        fixture = functions + """

DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS=1
export DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS
run_quiet_with_progress "fixture" "$1" bash -c \
  'printf "[1 / 2] Testing //donner/editor:sample_test /runner/secret\\n"; sleep 2; exit 23'
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            log = str(Path(temp_dir) / "coverage.log")
            result = self._run_script(fixture, [log], timeout=8)
            self.assertEqual(23, result.returncode, result.stderr)
            self.assertIn("[1 / 2]", result.stdout)
            self.assertIn("//donner/editor:sample_test", result.stdout)
            self.assertIn("coverage.log", result.stdout)
            self.assertNotIn(temp_dir, result.stdout)
            self.assertNotIn("/runner/secret", result.stdout)

        sanitizer = functions + """
safe_bazel_progress '[2 / 3] Linking /private/runner/secret'
safe_bazel_progress '[3 / 4] Testing @@module+//pkg:target /private/runner/secret'
safe_bazel_progress '[4 / 5] Testing //private/runner/path'
safe_bazel_progress '[5 / 6] Testing //pkg:target$secret'
safe_bazel_progress '[6 / 7] Linking output Testing //pkg:false_target'
safe_bazel_progress '[7 / 8] 1 / 2 tests, 1 failed; Testing //pkg:real_target'
"""
        sanitizer += f"safe_bazel_progress '[8 / 9] Testing //pkg:{'x' * 513}'\n"
        sanitizer += f"safe_bazel_progress '[{'9' * 65} / 1] Testing //pkg:too_long'\n"
        result = self._run_script(sanitizer, [])
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            "[2 / 3]\n[3 / 4] test @@module+//pkg:target\n"
            "[4 / 5]\n[5 / 6]\n[6 / 7]\n"
            "[7 / 8] test //pkg:real_target\n[8 / 9]\n",
            result.stdout,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
