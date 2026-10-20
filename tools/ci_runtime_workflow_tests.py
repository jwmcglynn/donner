"""Pins the self-hosted CI runtime boundaries that keep full runs viable."""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest.mock import patch

from python.runfiles import runfiles
from test_executable_fixtures import ExecutableFixtureDirectory, write_python_stand_in
from tools import test_timing


# The editor Wasm payload ceilings CI enforces. Changing one means editing the size test's
# arguments and this table together, so a widened ceiling is always a reviewed change.
ENFORCED_PAYLOAD_CEILINGS = {
    "--max-js-gzip-bytes": 54800,
    "--max-js-raw-bytes": 199000,
    "--max-total-raw-bytes": 12777000,
    "--max-wasm-data-segments": 64,
    "--max-wasm-function-body-bytes": 46000,
    "--max-wasm-gzip-bytes": 3439000,
    "--max-wasm-raw-bytes": 10661000,
}
PAYLOAD_SIZE_TEST = "//donner/editor/wasm:wasm_geode_package_size_tests"
# The standalone renderer Wasm ceilings CI enforces, changed together with that test's arguments.
RENDERER_PAYLOAD_CEILINGS = {
    "--max-js-gzip-bytes": 2800,
    "--max-js-raw-bytes": 5700,
    "--max-wasm-gzip-bytes": 867000,
    "--max-wasm-raw-bytes": 2210000,
}
RENDERER_SIZE_TEST = "//donner/svg/renderer/wasm:wasm_size_tests"
SIZE_CHECK_STEP = "- name: Build and size-check Geode editor Wasm package"
# Each Editor WASM build step that runs Bazel or stages an artifact, in workflow order, with the one
# command it runs. The commands themselves are pinned by //tools/ci:editor_wasm_lane_tests and
# //tools/ci:editor_wasm_stage_tests.
EDITOR_WASM_SCRIPT_STEPS = (
    ("Prefetch Editor WASM dependencies", "tools/ci/editor_wasm_lane.sh prefetch"),
    ("Retry Editor WASM dependency prefetch", "tools/ci/editor_wasm_lane.sh prefetch"),
    ("Build and size-check Geode editor Wasm package", "tools/ci/editor_wasm_lane.sh build"),
    ("Prove the package's resolved module graph",
     'tools/ci/editor_wasm_lane.sh prove-lockfile "$RUNNER_TEMP/editor-wasm-module.lock"'),
    ("Stage package for handoff", "python3 tools/ci/editor_wasm_stage.py package"),
    ("Link the browser GPU bridge", "tools/ci/editor_wasm_lane.sh link-bridge"),
    ("Stage immutable Geode deployment candidate",
     "python3 tools/ci/editor_wasm_stage.py candidate"),
    ("Record deployment candidate digest", "python3 tools/ci/editor_wasm_stage.py record-digest"),
)
def _repository_text(path):
    resolver = runfiles.Create()
    resolved = resolver.Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


def _declared_payload_ceilings(size_test_arguments, enforced=ENFORCED_PAYLOAD_CEILINGS):
    """The payload ceilings the size test declares, read from its build-graph arguments.

    A flag declared twice is dropped rather than reported: argparse keeps the
    last value, so a single reading of a repeated flag would bless a ceiling the
    test does not enforce.
    """
    ceilings = {}
    for flag in enforced:
        values = re.findall(r'"%s",\s*"(\d+)"' % re.escape(flag), size_test_arguments)
        if len(values) == 1:
            ceilings[flag] = int(values[0])
    return ceilings


class CiRuntimeWorkflowTest(unittest.TestCase):
    def test_executable_stubs_are_shared_for_the_test_class(self):
        stub = self.fixture_bin / "python3"
        resolved = []
        for _ in range(2):
            result = self._run_script("#!/bin/bash\ncommand -v python3\n", [])
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            resolved.append(Path(result.stdout.strip()))
        self.assertEqual(resolved, [stub, stub])
        self.assertEqual(resolved[0].stat().st_ino, resolved[1].stat().st_ino)

    @classmethod
    def setUpClass(cls):
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        cls.fixture_bin = cls.executables.path
        cls.executables.write_python3()
        cls.executables.write("bash", '#!/bin/sh\nexec /bin/bash "$@"\n')
        cls.bazelrc = _repository_text(".bazelrc")
        cls.main = _repository_text(".github/workflows/main.yml")
        cls.cmake = _repository_text(".github/workflows/cmake.yml")
        cls.coverage = _repository_text(".github/workflows/coverage.yml")
        cls.editor_wasm = _repository_text(".github/workflows/editor_wasm.yml")
        cls.editor_wasm_lane = _repository_text("tools/ci/editor_wasm_lane.sh")
        cls.lint = _repository_text(".github/workflows/lint.yml")
        cls.coverage_script = _repository_text("tools/coverage.sh")
        cls.apt_install = _repository_text(".github/actions/apt-install/action.yml")
        cls.wasm_size_test_arguments = _repository_text(
            "donner/editor/wasm/wasm_package_size_test_arguments.txt"
        )
        cls.ci_target_definitions = _repository_text("tools/ci/BUILD.bazel")
        cls.renderer_wasm_build = _repository_text("donner/svg/renderer/wasm/BUILD.bazel")
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

    def test_early_macos_bazel_failures_keep_their_artifacts(self):
        for lane in ("macos", "macos-browser", "macos-self-hosted"):
            with self.subTest(lane=lane):
                job = self._job_body(lane)
                archive = self._step_body(job, "Upload Bazel test failure artifacts")
                gates = [("Qualify pinned Chromium archive", "browser_preflight")]
                if lane == "macos-self-hosted":
                    gates.append(("Validate offline Metal compiler", "metal_compiler"))
                for name, step_id in gates:
                    step = self._step_body(job, name)
                    self.assertIn("id: " + step_id, step)
                    self.assertNotIn("continue-on-error", step)
                    self.assertIn("steps.%s.outcome == 'failure'" % step_id, archive)
                self.assertIn("./.github/actions/upload-bazel-test-artifacts", archive)

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
                           BAZEL_MACOS_TEST_FLAGS="", BROWSER_STALL_TEST_FLAGS="",
                           HOSTED_MACOS_QUARANTINE_TEST_FLAGS="")
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
        self.assertIn("if: ${{ !cancelled() && (steps.test.outcome == 'failure' || "
                      "steps.browser_test.outcome == 'failure') }}", collect)
        self.assertIn("continue-on-error: true", collect)
        self.assertIn("timeout-minutes: 5", collect)
        self.assertIn(
            "predicate='eventType == logEvent AND (process BEGINSWITH \"Google Chrome for Testing\" "
            "OR process == \"syspolicyd\" OR process == \"amfid\")'",
            collect,
        )
        upload = self._step_body(hosted, "Upload browser stall system log")
        self.assertIn("if: ${{ !cancelled() && (steps.test.outcome == 'failure' || "
                      "steps.browser_test.outcome == 'failure') }}", upload)
        self.assertIn("path: ${{ runner.temp }}/browser-stall-system-log", upload)
        self.assertIn("retention-days: 14", upload)
        # A self-hosted runner's unified log describes a persistent host; it is never published.
        for job in ("macos-self-hosted", "linux", "linux-self-hosted"):
            body = self._job_body(job)
            self.assertNotIn(name, body)
            self.assertNotIn("log show", body)

    def test_hosted_browser_tests_run_alone_after_the_suite(self):
        # A browser starved of CPU misses its wall-clock deadlines, so on the three-core
        # hosted runner every test that launches a browser runs after the rest of the
        # suite, one at a time, and nowhere else in that job.
        hosted = self._job_body("macos")
        main_test = self._step_body(hosted, "Test")
        self.assertIn(
            "--test_tag_filters=-manual,-perf,-requires_metal_toolchain,-requires_shader_validation,"
            "-ci-remote-gpu,-launches-browser,-fuzz_soak ",
            main_test,
        )
        browser = self._step_body(hosted, "Test (browser targets)")
        self.assertLess(hosted.index("      - name: Test\n"),
                        hosted.index("      - name: Test (browser targets)\n"))
        self.assertLess(hosted.index("      - name: Test (browser targets)\n"),
                        hosted.index("      - name: Upload Bazel test failure artifacts\n"))
        self.assertIn("id: browser_test", browser)
        self.assertIn(
            "if: ${{ !cancelled() && (steps.test.outcome == 'success' || "
            "steps.test.outcome == 'failure') }}",
            browser,
        )
        self.assertIn("--local_test_jobs=1", browser)
        self.assertIn(
            "--test_tag_filters=launches-browser,-manual,-perf,-requires_metal_toolchain,"
            "-ci-remote-gpu,-fuzz_soak ",
            browser,
        )
        self.assertIn("$BAZEL_MACOS_TEST_FLAGS $BROWSER_STALL_TEST_FLAGS $TARGETS", browser)
        # Every step that reacts to a failed test step also reacts to the browser phase.
        for step in ("Upload Bazel test failure artifacts", "Collect browser stall system log",
                     "Upload browser stall system log", "Detect browser GPU acquisition failure"):
            body = self._step_body(hosted, step)
            self.assertIn("steps.test.outcome == 'failure'", body, step)
            self.assertIn("steps.browser_test.outcome == 'failure'", body, step)
        # Other jobs keep their own scheduling.
        for job in ("linux", "linux-self-hosted"):
            self.assertNotIn("launches-browser", self._job_body(job), job)

    def test_self_hosted_browser_tests_run_alone_after_the_suite(self):
        job = self._job_body("macos-self-hosted")
        main_test = self._step_body(job, "Test")
        self.assertIn(
            "--test_tag_filters=-manual,-perf,-ci-remote-gpu,-launches-browser,-fuzz_soak ",
            main_test)
        browser = self._step_body(job, "Test (browser targets)")
        self.assertLess(job.index("      - name: Test\n"),
                        job.index("      - name: Test (browser targets)\n"))
        self.assertLess(job.index("      - name: Test (browser targets)\n"),
                        job.index("      - name: Test (Geode editor lane)\n"))
        self.assertIn("id: browser_test", browser)
        self.assertIn(
            "if: ${{ !cancelled() && (steps.test.outcome == 'success' || "
            "steps.test.outcome == 'failure') }}", browser)
        self.assertIn("--local_test_jobs=1", browser)
        self.assertIn(
            "--test_tag_filters=launches-browser,-manual,-perf,-requires_desktop_session,"
            "-ci-remote-gpu,-fuzz_soak ",
            browser)
        self.assertIn("bazelisk --nohome_rc --nosystem_rc test --config=ci", browser)
        for phase in (main_test, browser):
            self.assertIn("$BAZEL_MACOS_BUILD_FLAGS $BAZEL_MACOS_TEST_FLAGS $TARGETS", phase)
            self.assertNotIn("requires_metal_toolchain", phase)
        archive = self._step_body(job, "Upload Bazel test failure artifacts")
        self.assertIn("steps.browser_test.outcome == 'failure'", archive)

    def test_geode_phase_runs_after_a_completed_native_phase_even_if_browser_fails(self):
        for job_name in ("macos", "macos-self-hosted"):
            with self.subTest(job=job_name):
                job = self._job_body(job_name)
                geode = self._step_body(job, "Test (Geode editor lane)")
                self.assertIn(
                    "if: ${{ !cancelled() && (steps.test.outcome == 'success' || "
                    "steps.test.outcome == 'failure') }}", geode)
                self.assertNotIn("steps.browser_test.outcome", geode)
                self.assertLess(job.index("      - name: Test (browser targets)\n"),
                                job.index("      - name: Test (Geode editor lane)\n"))
                self.assertIn("$GEODE_EDITOR_TARGETS", geode)

    def test_every_test_that_launches_a_browser_is_tagged(self):
        # The hosted browser phase selects by tag, so an untagged browser test would
        # silently return to the oversubscribed main test step.
        launching = []
        for package in ("donner/editor/wasm/tests", "donner/gpu/shader/browser_tests"):
            build = _repository_text(package + "/BUILD.bazel")
            for match in re.finditer(r"^\w[\w.]*\(\n(.*?)\n\)\n", build, re.MULTILINE | re.DOTALL):
                body = match.group(1)
                launches = ('"@playwright//:' in body
                            or 'entry_point = "browser-archive-check.mjs"' in body)
                if not launches:
                    continue
                name = re.search(r'name = "([^"]+)"', body).group(1)
                tags = re.search(r"\n    tags = \[(.*?)\]", body, re.DOTALL)
                tag_names = re.findall(r'"([^"]+)"', tags.group(1)) if tags else []
                if "manual" in tag_names:
                    continue
                launching.append(name)
                self.assertIn("launches-browser", tag_names, "%s:%s" % (package, name))
        self.assertEqual(sorted(launching), [
            "boot_presentation_test",
            "browser_presentation_regression_test",
            "catalog_font_loading_test",
            "chromium_remote_smoke",
            "linux_browser_archive_tests",
            "mac_browser_archive_tests",
            "slug_endpoint_chromium_tests",
            "standalone_geode_browser_renderer_test",
            "wgsl_chromium_compilation_tests",
        ])

    def test_browser_affected_is_derived_from_the_affected_set_and_fails_closed(self):
        determine = self._job_body("determine-targets")
        self.assertIn(
            "      browser_affected: ${{ steps.determine.outputs.browser_affected }}\n",
            determine)
        script = determine.split("        name: Determine affected Bazel targets\n", 1)[1]
        script = script.split("\n      - ", 1)[0]
        query = 'attr(\\"tags\\", \\"launches-browser\\", set($affected))'
        self.assertIn(query, script)
        # The query runs on the filtered selection, against the PR's merge graph.
        self.assertLess(script.index("manual_affected="), script.index(query))
        self.assertIn('cd "$head_dir"', script.split(query, 1)[0].rsplit("if ", 1)[1])
        failed = script.split('echo "Browser target query failed', 1)[1].split("fi\n", 1)[0]
        self.assertIn('echo "browser_affected=true" >> "$GITHUB_OUTPUT"', failed)
        self.assertEqual(script.count("browser_affected="), 3)
        # Dependency manifests reach Bazel through module extensions, which bazel-diff cannot
        # attribute, so a change to any of them adds every browser test to the selection.
        manifests = script.split("browser_inputs_changed=false\n", 1)[1].split("esac", 1)[0]
        for path in ("donner/editor/wasm/tests/package.json",
                     "donner/editor/wasm/tests/package-lock.json",
                     "donner/editor/wasm/tests/pnpm-lock.yaml",
                     "donner/editor/wasm/tests/browsers.*.json"):
            self.assertIn(path, manifests)
        self.assertIn("browser_inputs_changed=true", manifests)
        self.assertLess(script.index("browser_inputs_changed=false"),
                        script.index('if [[ "$should_fallback" == "true" ]]; then'))
        opening = 'if [[ "$browser_inputs_changed" == "true" ]]; then\n'
        block = opening + script.split(opening, 1)[1].split("\n\n", 1)[0] + "\n"
        self.assertLess(script.index(opening), script.index(query))
        self.assertGreater(script.index(opening), script.index("manual_affected="))
        for listing, expected_affected, expected_output in (
            # The browser tests join the selection, so every lane runs them.
            ("//w:smoke\n//w:regression\n", "//a:b //w:smoke //w:regression", ""),
            # No listing falls back to //....
            ("", None, "fallback=true\naffected=\n"),
        ):
            with self.subTest(listing=listing), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "head").mkdir()
                listing_file = root / "listing.txt"
                listing_file.write_text(listing)
                fake = root / "bazelisk"
                fake.write_text('#!/bin/sh\ncat "%s"\n' % listing_file)
                fake.chmod(0o755)
                output = root / "output"
                output.write_text("")
                env = {**os.environ, "GITHUB_OUTPUT": str(output),
                       "PATH": str(root) + os.pathsep + os.environ.get("PATH", "")}
                result = self._run_script(
                    "set -euo pipefail\nbrowser_inputs_changed=true\nhead_dir=%s\n"
                    "affected='//a:b'\n%secho \"affected<$affected>\"\n"
                    % (root / "head", block), [], env=env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(output.read_text(), expected_output)
                if expected_affected is None:
                    self.assertNotIn("affected<", result.stdout)
                else:
                    shown = result.stdout.split("affected<", 1)[1].split(">", 1)[0]
                    self.assertEqual(shown.split(), expected_affected.split())

    def test_hosted_browser_subset_keeps_the_hosted_failure_diagnostics(self):
        # A browser failure in the trusted-PR subset needs the same evidence as one in the
        # hosted macos job: the stall system-log slice and the serial and parallel GPU probes.
        # The subset has no native Test step, so its conditions key on the browser step alone.
        hosted = self._job_body("macos")
        subset = self._job_body("macos-browser")
        steps = ("Collect browser stall system log", "Upload browser stall system log",
                 "Detect browser GPU acquisition failure", "Compare browser GPU tests (serial)",
                 "Upload browser GPU comparison (serial)", "Compare browser GPU tests (parallel)",
                 "Upload browser GPU comparison (parallel)",
                 "Upload bounded browser GPU timing summaries")
        positions = []
        for step in steps:
            expected = self._step_body(hosted, step).replace(
                "steps.test.outcome == 'failure' || steps.browser_test.outcome == 'failure'",
                "steps.browser_test.outcome == 'failure'")
            body = self._step_body(subset, step)
            # A job's last step body runs on into the next job's leading comments.
            self.assertEqual(body.split("\n\n", 1)[0], expected.split("\n\n", 1)[0], step)
            self.assertNotIn("steps.test.", body, step)
            positions.append(subset.index("      - name: %s\n" % step))
        self.assertEqual(positions, sorted(positions))
        self.assertGreater(positions[0], subset.index("      - name: Test (browser targets)\n"))

    def test_browser_stall_recorder_is_enabled_only_in_the_hosted_job(self):
        # The recorder publishes host load, memory and call graphs, so only the ephemeral
        # hosted job may enable it; every other lane and local runs leave it inert.
        switch = '--test_env=DONNER_BROWSER_STALL_DIAGNOSTICS=1'
        hosted = self._job_body("macos")
        self.assertIn('      BROWSER_STALL_TEST_FLAGS: "%s"\n' % switch, hosted)
        steps = ("Test", "Test (browser targets)", "Compare browser GPU tests (serial)",
                 "Compare browser GPU tests (parallel)")
        for step in steps:
            self.assertIn("$BAZEL_MACOS_TEST_FLAGS $BROWSER_STALL_TEST_FLAGS",
                          self._step_body(hosted, step), step)
        # The trusted-PR hosted browser subset runs the same browser step and GPU comparisons
        # on the same image.
        subset = self._job_body("macos-browser")
        self.assertIn('      BROWSER_STALL_TEST_FLAGS: "%s"\n' % switch, subset)
        subset_steps = steps[1:]
        for step in subset_steps:
            self.assertIn("$BAZEL_MACOS_TEST_FLAGS $BROWSER_STALL_TEST_FLAGS",
                          self._step_body(subset, step), step)
        recorder = _repository_text("donner/editor/wasm/tests/browser-stall-diagnostics.mjs")
        self.assertIn('kEnableVariable = "DONNER_BROWSER_STALL_DIAGNOSTICS"', recorder)
        self.assertEqual(self.main.count("DONNER_BROWSER_STALL_DIAGNOSTICS"), 2)
        self.assertEqual(self.main.count("BROWSER_STALL_TEST_FLAGS"),
                         2 + len(steps) + len(subset_steps))
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

    def test_hosted_macos_quarantine_is_set_only_for_the_hosted_test_environment(self):
        # Main's hosted macOS job skips browser cases quarantined for that runner alone (#1691,
        # #1702). The switch is one .bazelrc config, passed only by the gating test steps of the
        # jobs that reproduce that job's test environment, so they skip exactly the same cases.
        # Every other lane and local runs must keep running them.
        readers = {
            "macos": ("Test", "Test (browser targets)"),
            # The trusted-PR hosted browser subset runs main's hosted browser step.
            "macos-browser": ("Test (browser targets)",),
        }
        for job_name, steps in readers.items():
            job = self._job_body(job_name)
            for step in steps:
                self.assertIn("--config=hosted-macos-quarantine", self._step_body(job, step),
                              (job_name, step))
        self.assertEqual(self.main.count("--config=hosted-macos-quarantine"),
                         sum(len(steps) for steps in readers.values()))
        # The trusted-PR hosted-parity subset gets it through --config=hosted-parity.
        parity = self._job_body("macos-hosted-parity")
        self.assertIn("--config=hosted-parity-subset", self._step_body(parity, "Test"))
        self.assertNotIn("DONNER_HOSTED_MACOS_QUARANTINE", self.main)
        hosted = self._job_body("macos")
        for step in ("Test", "Test (browser targets)"):
            self.assertIn("$BROWSER_STALL_TEST_FLAGS", self._step_body(hosted, step), step)
        # The advisory comparisons never set the job's outcome and exist to diagnose exactly
        # these failures, so they keep running the quarantined cases.
        for mode in ("serial", "parallel"):
            step = "Compare browser GPU tests (%s)" % mode
            body = self._step_body(hosted, step)
            self.assertIn("continue-on-error: true", body, step)
            for name in ("hosted-macos-quarantine", "DONNER_HOSTED_MACOS_QUARANTINE",
                         "hosted-parity"):
                self.assertNotIn(name, body, step)
        for job in ("macos-self-hosted", "linux", "linux-self-hosted"):
            for name in ("hosted-macos-quarantine", "DONNER_HOSTED_MACOS_QUARANTINE",
                         "hosted-parity"):
                self.assertNotIn(name, self._job_body(job), job)
        resolver = runfiles.Create()
        workflows = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent
        others = [path for path in sorted(workflows.glob("*.y*ml")) if path.name != "main.yml"]
        self.assertGreater(len(others), 5)
        actions = sorted((workflows.parent / "actions").glob("*/action.y*ml"))
        self.assertGreater(len(actions), 3)
        for path in others + actions:
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("HOSTED_MACOS_QUARANTINE", text, path.name)
            self.assertNotIn("hosted-macos-quarantine", text, path.name)
        # In the repository bazelrc it is one named config, which no other config or default
        # names except the hosted-parity config.
        self.assertEqual(
            [line for line in self.bazelrc.splitlines() if "HOSTED_MACOS_QUARANTINE" in line
             or "hosted-macos-quarantine" in line],
            ["test:hosted-macos-quarantine --test_env=DONNER_HOSTED_MACOS_QUARANTINE=1",
             "test:hosted-parity --config=hosted-macos-quarantine"])

    def test_quarantine_report_lane_is_enabled_only_by_the_scheduled_editor_wasm_run(self):
        # The report-only lane lifts the Firefox quarantine of #1634 and never fails its job.
        # Only the nightly Editor WASM run may enable it: pull request and push runs keep the
        # quarantine, so a quarantined case can neither block a merge nor turn main red.
        resolver = runfiles.Create()
        workflows = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent
        editor = self.editor_wasm
        gate = ("          DONNER_BROWSER_QUARANTINE_REPORT_LANE: "
                "${{ github.event_name == 'schedule' && '1' || '0' }}\n")
        self.assertIn(gate, editor)
        self.assertEqual(editor.count("DONNER_BROWSER_QUARANTINE_REPORT"), 1)
        schedule_only = "        if: ${{ !cancelled() && github.event_name == 'schedule' }}\n"
        self.assertEqual(editor.count(schedule_only), 2)
        for step in ("Summarize quarantined cases", "Upload quarantined case evidence"):
            self.assertIn("      - name: %s\n%s" % (step, schedule_only), editor, step)
        summary = self._step_body(editor, "Summarize quarantined cases")
        self.assertIn('|| echo "::warning::Could not summarize the quarantined cases"', summary)
        self.assertIn("if-no-files-found: warn",
                      self._step_body(editor, "Upload quarantined case evidence"))
        others = [path for path in sorted(workflows.glob("*.y*ml"))
                  if path.name != "editor_wasm.yml"]
        self.assertGreater(len(others), 5)
        actions = sorted((workflows.parent / "actions").glob("*/action.y*ml"))
        self.assertGreater(len(actions), 3)
        for path in others + actions:
            self.assertNotIn("DONNER_BROWSER_QUARANTINE_REPORT", path.read_text(encoding="utf-8"),
                             path.name)
        self.assertNotIn("DONNER_BROWSER_QUARANTINE_REPORT", self.bazelrc)

    def test_headed_browser_tests_keep_required_hosted_coverage(self):
        for job_name in ("macos", "macos-browser", "macos-self-hosted"):
            with self.subTest(job=job_name):
                body = self._step_body(self._job_body(job_name), "Test (browser targets)")
                match = re.search(r"--test_tag_filters=([^\s]+)", body)
                self.assertIsNotNone(match)
                tags = match.group(1).split(",")
                self.assertIn("launches-browser", tags)
                self.assertEqual("-requires_desktop_session" in tags,
                                 job_name == "macos-self-hosted")
                self.assertNotIn("continue-on-error: true", body)

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
                                    env=environment, capture_output=True, text=True,
                                    timeout=test_timing.STALL_GUARD_SECONDS)
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

    def _run_script(self, text, args, env=None, cwd=None, context=None):
        with tempfile.TemporaryDirectory() as temp_dir:
            script = Path(temp_dir) / "fixture.sh"
            # Best-effort builtin markers expose progress without captured script output.
            # Scripts may replace EXIT traps, so an absent marker proves no lifecycle state.
            script.write_text(
                "printf 'fixture-stage:entered\\n' >&2\n"
                "trap \"printf 'fixture-stage:exited\\n' >&2\" EXIT\n" + text
            )
            fixture_env = dict(os.environ if env is None else env)
            # Fixture scripts must not execute an unrelated shell startup file.
            fixture_env.pop("BASH_ENV", None)
            fixture_env["PATH"] = (temp_dir + os.pathsep + str(self.fixture_bin) + os.pathsep +
                                   fixture_env.get("PATH", os.defpath))
            try:
                return subprocess.run(
                    ["/bin/bash", str(script), *args],
                    check=False,
                    capture_output=True,
                    text=True,
                    env=fixture_env,
                    timeout=test_timing.STALL_GUARD_SECONDS,
                    cwd=cwd,
                )
            except subprocess.TimeoutExpired as error:
                stderr = error.stderr or b""
                if isinstance(stderr, str):
                    stderr = stderr.encode()
                entered = b"fixture-stage:entered\n" in stderr
                exited = b"fixture-stage:exited\n" in stderr
                raise AssertionError(
                    f"{context or self.id()}: shell fixture exceeded "
                    f"{test_timing.STALL_GUARD_SECONDS} seconds "
                    f"(entry_marker_seen={entered}, exit_marker_seen={exited})"
                ) from None

    def test_wasm_build_and_browser_checks_share_one_runner_and_package(self):
        self.assertNotIn("\n  test:\n", self.editor_wasm)
        self.assertNotIn("actions/download-artifact@", self.editor_wasm)
        self.assertIn("PACKAGE_TARGET: ${{ steps.stage-package.outputs.package_target }}", self.editor_wasm)
        self.assertIn('push:\n    branches: ["main"]', self.editor_wasm)
        self.assertIn("uses: ./.github/actions/cache-bazel-actions", self.editor_wasm)
        # npm downloads restore from main's cache; only main pushes save it.
        self.assertIn("- name: Restore npm download cache", self.editor_wasm)
        self.assertIn("- name: Save main npm download cache", self.editor_wasm)
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
        self.assertIn("cmake --build build --target donner_cmake_consumer", consumer)
        self.assertIn("ctest --test-dir build --output-on-failure", consumer)
        # The CMake workflow runs the same consumer through its lane script, whose commands
        # //tools/ci:cmake_consumer_tests pins.
        self.assertIn("run: tools/ci/cmake_consumer.sh build --ccache\n", linux)
        self.assertEqual(2, self.cmake.count("run: tools/ci/cmake_consumer.sh build --ccache\n"))
        self.assertEqual(1, self.cmake.count("run: tools/ci/cmake_consumer.sh install-tools\n"))

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
                    ambient_bash = root / "bash"
                    ambient_bash.write_text("#!/bin/sh\necho ambient-bash-was-used >&2\nexit 97\n")
                    ambient_bash.chmod(0o755)
                    args_file = root / "args.txt"
                    env = {**os.environ, "BAZEL_ARGS": str(args_file),
                           "PATH": str(root) + os.pathsep + os.environ.get("PATH", "")}
                    result = self._run_script(script, [], cwd=root, env=env)
                    self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                    args = args_file.read_text()
                    self.assertIn("--test_tag_filters=requires_xvfb", args)
                    self.assertIn(expected_target, args)
                    self.assertEqual(no_tests, "No test targets" in result.stdout)

    def test_shell_fixture_timeout_reports_only_known_progress_markers(self):
        for stderr, entered, exited in (
            (b"untrusted-output", False, False),
            (b"fixture-stage:entered\nuntrusted-output", True, False),
            (b"fixture-stage:entered\nfixture-stage:exited\nuntrusted-output", True, True),
        ):
            with self.subTest(entered=entered, exited=exited):
                error = subprocess.TimeoutExpired("fixture", 10, stderr=stderr)
                with patch("subprocess.run", side_effect=error):
                    with self.assertRaises(AssertionError) as raised:
                        self._run_script("exit 0\n", [])
                message = str(raised.exception)
                self.assertIn(
                    f"entry_marker_seen={entered}, exit_marker_seen={exited}", message
                )
                self.assertNotIn("untrusted-output", message)
                self.assertTrue(raised.exception.__suppress_context__)

    def test_shell_fixture_keeps_a_script_owned_exit_trap(self):
        result = self._run_script(
            "trap \"printf 'script-owned-exit\\n' >&2\" EXIT\nexit 0\n", []
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("fixture-stage:entered\n", result.stderr)
        self.assertIn("script-owned-exit\n", result.stderr)
        self.assertNotIn("fixture-stage:exited\n", result.stderr)

    def test_shell_fixture_ignores_inherited_bash_startup_hook(self):
        with tempfile.TemporaryDirectory() as directory:
            startup = Path(directory) / "startup.sh"
            startup.write_text("echo ambient-startup-was-used >&2\nexit 73\n")
            result = self._run_script(
                "#!/bin/bash\nprintf 'fixture-complete\\n'\n",
                [],
                env={**os.environ, "BASH_ENV": str(startup)},
            )
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertEqual("fixture-complete\n", result.stdout)

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

    def test_shader_validation_runs_only_on_the_self_hosted_real_gpu(self):
        # The hosted image's virtual GPU cannot run Metal shader validation
        # (docs/metal_validation.md), so the hosted jobs run their Metal tests unvalidated and
        # skip the tests that assert validation; the self-hosted job keeps both gates mandatory.
        for job_name in ("macos", "macos-browser"):
            job = self._job_body(job_name)
            for step in ("Select Metal validation profile", "Metal validation preflight",
                         "Fetch Metal validation dependencies"):
                self.assertNotIn("- name: %s\n" % step, job, job_name)
            self.assertNotIn("metal_profile", job, job_name)
            self.assertIn("--test_env=MTL_SHADER_VALIDATION_TEXTURE_USAGE=0", job, job_name)
            self.assertNotIn("MTL_SHADER_VALIDATION_TEXTURE_USAGE=1", job, job_name)
        hosted_test = self._step_body(self._job_body("macos"), "Test")
        self.assertIn("-requires_shader_validation", hosted_test)
        self.assertIn("--config=hosted-macos-unvalidated", hosted_test)
        # Only main's hosted job turns shader validation off; every self-hosted lane keeps it.
        for job_name in ("macos-self-hosted", "macos-hosted-parity"):
            job = self._job_body(job_name)
            self.assertNotIn("hosted-macos-unvalidated", job, job_name)
            self.assertNotIn("metal_shader_validation", job, job_name)
        bazelrc = _repository_text(".bazelrc")
        setting = [line for line in bazelrc.splitlines()
                   if "metal_shader_validation" in line and not line.startswith("#")]
        self.assertEqual(setting,
                         ["test:hosted-macos-unvalidated --//build_defs:metal_shader_validation=off"])
        self.assertNotRegex(bazelrc, r"(?m)^\S+:(?!hosted-macos-unvalidated\s)\S*\s.*"
                                     r"--config=hosted-macos-unvalidated")
        trusted = self._job_body("macos-self-hosted")
        self.assertNotIn("MTL_SHADER_VALIDATION_TEXTURE_USAGE=0", trusted)
        self.assertNotIn("requires_shader_validation", self._step_body(trusted, "Test"))
        required = self._step_body(trusted, "Require full Metal shader validation")
        preflight = self._step_body(trusted, "Metal validation preflight")
        for step, target in ((required, "//tools/ci:metal_full_validation"),
                             (preflight, "//tools/ci:metal_preflight")):
            self.assertIn(target, step)
            self.assertIn("--nocache_test_results", step)
            # Mandatory: never skipped, never allowed to fail.
            self.assertNotRegex(step, r"(?m)^ {8}(if|continue-on-error):")
            self.assertLess(trusted.index(step), trusted.index("      - name: Build\n"))
        self.assertIn("--test_tag_filters=", required)
        self.assertIn("steps.metal_profile.outcome == 'failure'", trusted)
        self.assertIn("steps.metal_preflight.outcome == 'failure'", trusted)
        suites = _repository_text("tools/ci/BUILD.bazel")
        preflight_suite = suites.split('name = "metal_preflight"', 1)[1].split(")", 1)[0]
        self.assertIn("//donner/gpu/metal/tests:metal_shader_memory_validation_tests",
                      preflight_suite)
        metal = _repository_text("donner/gpu/metal/tests/BUILD.bazel")
        for target in ("metal_validation_profile", "metal_shader_memory_validation_tests"):
            rule = metal.split('name = "%s",' % target, 1)[1].split("\n)\n", 1)[0]
            self.assertIn('tags = ["requires_shader_validation"]', rule, target)

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

    def test_pr_test_lanes_exclude_manual_targets_at_the_command_line(self):
        """Runner bazelrcs cannot re-enable opt-in tests during full fallbacks."""
        self.assertIn("test:ci --test_tag_filters=-manual,-perf", self.bazelrc)
        for job_name in ("linux", "linux-self-hosted", "macos", "macos-self-hosted"):
            with self.subTest(job=job_name):
                job = self._job_body(job_name)
                self.assertEqual(1, job.count("--test_tag_filters=-manual,-perf"))

    def test_pr_test_lanes_exclude_fuzz_soaks(self):
        """Mutation soaks run in the Fuzz workflow; PR CI keeps only the corpus replay."""
        ci_filter = re.search(r"(?m)^test:ci --test_tag_filters=(\S+)$", self.bazelrc)
        self.assertIsNotNone(ci_filter)
        self.assertIn("-fuzz_soak", ci_filter.group(1).split(","))
        selected_suites = set()
        for job_name in ("linux", "linux-self-hosted", "macos", "macos-self-hosted"):
            job = self._job_body(job_name)
            filters = re.findall(r"--test_tag_filters=(\S*)", job)
            self.assertTrue(filters, job_name)
            for value in filters:
                # An empty filter accompanies an explicit //tools/ci suite, checked below.
                if value:
                    with self.subTest(job=job_name, filters=value):
                        self.assertIn("-fuzz_soak", value.split(","))
            uncommented = "\n".join(
                line for line in job.splitlines() if not line.lstrip().startswith("#"))
            selected_suites.update(re.findall(r"//tools/ci:(\w+)", uncommented))
            for variable in re.findall(r"\$(\w+_TARGETS)\b", uncommented):
                definition = re.search(
                    r"(?m)^      %s: >-\n((?:        .*\n)+)" % variable, job)
                self.assertIsNotNone(definition, variable)
                selected_suites.update(re.findall(r"//tools/ci:(\w+)", definition.group(1)))
        self.assertTrue(selected_suites)
        suites = dict(re.findall(
            r'(?ms)^test_suite\(\n    name = "(\w+)",\n(.*?)^\)$', self.ci_target_definitions))
        pending = sorted(selected_suites)
        seen = set()
        while pending:
            name = pending.pop()
            if name in seen:
                continue
            seen.add(name)
            if name not in suites:
                continue  # Not a suite, such as a build-only canary binary.
            with self.subTest(suite=name):
                self.assertNotRegex(suites[name], r'_soak"')
            pending.extend(re.findall(r'"(?://tools/ci)?:(\w+)"', suites[name]))

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
        target is incompatible with the host platform: nothing was measured.
        That path used to exit the script's inner
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

    def test_coverage_workflow_treats_a_missing_report_as_a_failure(self):
        """The complete-tree run must upload a baseline or fail.

        tools/coverage.sh may legitimately write no report when every explicit
        label is incompatible with the host. On the coverage workflow, which
        always selects the complete product tree, that outcome would mean a
        classification regression, and honouring it as a skip would end a
        nightly run green with no baseline uploaded. So the skip marker fails
        the build, and no report consumer is conditional on a report outcome.
        """
        jobs = dict(self._coverage_jobs())
        build = jobs["build"]
        generate = build[build.index("        name: Generate coverage\n"):]
        generate = generate[: generate.index("\n      - ")]
        # The lane script fails on the skip marker; //tools/ci:coverage_lane_tests runs it.
        self.assertIn("run: tools/ci/coverage_lane.sh\n", generate + "\n")
        self.assertNotRegex(generate, r"(?m)^        (if|continue-on-error): ")

        self.assertNotIn("report_written", self.coverage)
        consumers = []
        for job_name, job_body in self._coverage_jobs():
            for step_body in re.split(r"^      - ", job_body, flags=re.MULTILINE)[1:]:
                step_name = step_body.partition("\n")[0]
                if "filtered_report.dat" in step_body or "report_artifact_id" in step_body:
                    consumers.append("%s / %s" % (job_name, step_name))
                    self.assertNotRegex(
                        step_body, r"(?m)^        if: ",
                        "step %r in job %r can be skipped although the report "
                        "must exist" % (step_name, job_name),
                    )
        self.assertGreaterEqual(
            len(consumers), 3,
            "consumer discovery matched %r, which is fewer than exist" % (consumers,),
        )

    def test_failed_hosted_coverage_retains_test_failure_artifacts(self):
        jobs = dict(self._coverage_jobs())
        step = self._step_body(jobs["build"], "Upload coverage test failure artifacts")
        self.assertIn("if: failure() && steps.coverage.outcome == 'failure'", step)
        self.assertIn("uses: ./.github/actions/upload-bazel-test-artifacts", step)
        self.assertIn("name: coverage-test-failure-${{ github.job }}", step)

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
        for job_name in ("macos", "macos-self-hosted", "macos-hosted-parity"):
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

    def test_shared_apt_action_installs_recommends_only_when_asked(self):
        """The action's script runs the same apt commands as before, recommends opt-in."""
        script = textwrap.dedent(self.apt_install.split("      run: |\n", 1)[1])
        with tempfile.TemporaryDirectory() as temp:
            stubs = Path(temp) / "bin"
            stubs.mkdir()
            calls = Path(temp) / "calls.jsonl"
            recorder = stubs / "record.py"
            recorder.write_text(
                "import json, os, sys\n"
                "with open(os.environ['CAPTURE'], 'a', encoding='utf-8') as capture:\n"
                "    capture.write(json.dumps(sys.argv[1:]) + '\\n')\n",
                encoding="utf-8")
            for name, body in (
                    # sudo -E and timeout's own options are dropped; the command still runs.
                    ("sudo", '[ "$1" = -E ] && shift\nexec "$@"\n'),
                    ("timeout", 'shift 3\nexec "$@"\n')):
                path = stubs / name
                path.write_text("#!/bin/sh\n" + body, encoding="utf-8")
                path.chmod(0o755)
            write_python_stand_in(stubs / "apt-get", recorder)
            os.symlink(shutil.which("tr"), stubs / "tr")

            def run(recommends):
                calls.write_text("", encoding="utf-8")
                env = dict(os.environ, PATH=str(stubs), CAPTURE=str(calls),
                           APT_PACKAGES="graphviz\nfonts-liberation2", APT_ATTEMPTS="1",
                           APT_ATTEMPT_TIMEOUT="15m", APT_RETRY_WAIT="0",
                           APT_INSTALL_RECOMMENDS=recommends)
                result = subprocess.run(["/bin/bash", "-c", script], env=env,
                                        capture_output=True, text=True, check=False,
                                        timeout=test_timing.STALL_GUARD_SECONDS)
                return result, [json.loads(line) for line in calls.read_text().splitlines()]

            update = ["-o", "Acquire::Retries=3", "update"]
            install = ["-o", "Acquire::Retries=3", "install", "-y"]
            packages = ["graphviz", "fonts-liberation2"]
            for recommends, expected in (
                    ("false", install + ["--no-install-recommends"] + packages),
                    ("true", install + packages)):
                with self.subTest(recommends=recommends):
                    result, recorded = run(recommends)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(recorded, [update, expected, ["clean"]])
            result, recorded = run("yes")
            self.assertEqual(result.returncode, 1)
            self.assertIn("install-recommends must be true or false", result.stderr)
            self.assertEqual(recorded, [])

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

    def _editor_wasm_step_runs(self, workflow):
        """Each Editor WASM build step's name and its `run:` lines, in workflow order."""
        build_job = workflow.split("\n  build:\n", 1)[1]
        steps = re.split(r"^      - (?=id: |name: )", build_job, flags=re.MULTILINE)[1:]
        found = []
        for step in steps:
            name = re.search(r"^(?:        )?name: (.*)$", step, re.MULTILINE)
            found.append((name.group(1) if name else None,
                          re.findall(r"^        run: (.*)$", step, re.MULTILINE)))
        return found

    def test_editor_wasm_steps_each_run_one_tested_script_call(self):
        """Each Bazel or staging step is one call of a tested script, in the original order."""
        runs = {name: commands for name, commands in self._editor_wasm_step_runs(self.editor_wasm)}
        names = [name for name, _ in self._editor_wasm_step_runs(self.editor_wasm)]
        for name, command in EDITOR_WASM_SCRIPT_STEPS:
            with self.subTest(step=name):
                self.assertEqual(runs.get(name), [command])
        self.assertEqual([name for name in names if name in dict(EDITOR_WASM_SCRIPT_STEPS)],
                         [name for name, _ in EDITOR_WASM_SCRIPT_STEPS])
        # Every other Bazel command of the job runs in tools/run-browser-ci.sh.
        self.assertNotRegex(self.editor_wasm, r"bazelisk (?:fetch|build|test|query|cquery|run)\b")
        # The handoff step resolves the package label the candidate's provenance records.
        self.assertIn(
            "          PACKAGE_TARGET: ${{ steps.stage-package.outputs.package_target }}\n"
            "        run: python3 tools/ci/editor_wasm_stage.py candidate\n", self.editor_wasm)
        # The artifact digest reaches the summary through the environment, not the script text.
        self.assertIn(
            "          ARTIFACT_DIGEST: ${{ steps.upload-candidate.outputs.artifact-digest }}\n"
            "        run: python3 tools/ci/editor_wasm_stage.py record-digest\n", self.editor_wasm)

    def test_editor_wasm_step_binding_detects_inline_or_reordered_steps(self):
        workflow = self.editor_wasm
        prove = dict(EDITOR_WASM_SCRIPT_STEPS)["Prove the package's resolved module graph"]
        mutations = {
            "inline command": workflow.replace(
                "run: tools/ci/editor_wasm_lane.sh build",
                "run: bazelisk test //tools/ci:editor_wasm_audits", 1),
            "second command": workflow.replace(
                "        run: tools/ci/editor_wasm_lane.sh link-bridge\n",
                "        run: tools/ci/editor_wasm_lane.sh link-bridge\n"
                "        run: tools/ci/editor_wasm_lane.sh prefetch\n", 1),
            "proof and staging swapped": workflow.replace(
                "run: " + prove, "run: python3 tools/ci/editor_wasm_stage.py package", 1).replace(
                "id: stage-package\n        run: python3 tools/ci/editor_wasm_stage.py package",
                "id: stage-package\n        run: " + prove, 1),
        }
        for name, changed in mutations.items():
            with self.subTest(mutation=name):
                self.assertNotEqual(changed, workflow)
                runs = self._editor_wasm_step_runs(changed)
                commands = dict(runs)
                self.assertNotEqual(
                    [(step, commands.get(step)) for step, _ in EDITOR_WASM_SCRIPT_STEPS],
                    [(step, [command]) for step, command in EDITOR_WASM_SCRIPT_STEPS])

    def _lane_function(self, name, lane=None):
        """The body of one subcommand's function in tools/ci/editor_wasm_lane.sh, continuation
        lines joined. The opening and closing markers are both asserted, so a renamed function
        cannot widen the body to the rest of the script."""
        lane = self.editor_wasm_lane if lane is None else lane
        marker = "\n%s() {\n" % name
        self.assertEqual(1, lane.count(marker), "lane function %s not found" % name)
        rest = lane.split(marker, 1)[1]
        self.assertIn("\n}\n", rest)
        return re.sub(r"\\\n\s*", "", rest.split("\n}\n", 1)[0])

    def test_editor_wasm_prefetch_retries_before_single_attempt_tests(self):
        """Wasm dependency fetches retry without retrying build or test failures."""
        workflow = self.editor_wasm
        self.assertIn("- id: editor-wasm-prefetch", workflow)
        prefetch = workflow.split("- id: editor-wasm-prefetch", 1)[1].split(SIZE_CHECK_STEP, 1)[0]
        self.assertEqual(2, prefetch.count("run: tools/ci/editor_wasm_lane.sh prefetch\n"))
        self.assertEqual(1, prefetch.count("continue-on-error: true"))
        self.assertEqual(
            1,
            prefetch.count("if: steps.editor-wasm-prefetch.outcome == 'failure'"),
        )
        fetch = self._lane_function("prefetch")
        self.assertEqual(4, fetch.count("bazelisk fetch"))
        self.assertNotRegex(fetch, r"bazelisk (?:build|test)")

        size_check = self._step_body(workflow, SIZE_CHECK_STEP.removeprefix("- name: "))
        self.assertNotIn("continue-on-error", size_check)
        build = self._size_check_phase()
        self.assertEqual(4, build.count("bazelisk test"))
        self.assertNotIn("bazelisk fetch", build)
        self.assertNotIn("||", build)
        self.assertIn("//tools/ci:editor_wasm_shipped_browser_audit", build)
        self.assertIn("//tools/ci:editor_wasm_size_tests", build)

    def test_editor_wasm_pull_requests_link_the_browser_gpu_bridge(self):
        """Both bridge probes, the standalone module and its default audit run on every PR.

        The module is built explicitly: it is incompatible without Browser selection, and an
        incompatible audit inside a test_suite is skipped rather than failed.
        """
        self._assert_editor_wasm_browser_bridge_link(self.editor_wasm, self.editor_wasm_lane)

    def _assert_editor_wasm_browser_bridge_link(self, workflow, lane):
        build_job = workflow.split("\n  build:\n", 1)[1].split("\n  test:\n", 1)[0]
        step = self._step_body(build_job, "Link the browser GPU bridge")
        self.assertIn("        run: tools/ci/editor_wasm_lane.sh link-bridge\n", step)
        self.assertNotIn("continue-on-error", step)
        link = self._lane_function("link_bridge", lane)
        build = link.split("bazelisk build --config=wasm-geode", 1)[1].split("bazelisk test", 1)[0]
        self.assertIn("//tools/ci:browser_bridge_link_probes", build)
        self.assertIn("//tools/ci:geode_wasm_browser_module", build)
        self.assertIn("bazelisk test --config=wasm-geode", link)
        self.assertIn("//tools/ci:geode_wasm_browser_default_audit", link)
        self.assertNotIn("||", link)
        module = 'name = "geode_wasm_browser_module"'
        self.assertIn(module, self.ci_target_definitions)
        definition = self.ci_target_definitions.split(module, 1)[1].split("\n)", 1)[0]
        self.assertIn('srcs = ["//donner/svg/renderer/wasm:donner_wasm_geode"]', definition)

    def test_browser_bridge_step_stays_strict_with_later_evidence_recovery(self):
        workflow = self.editor_wasm
        lane = self.editor_wasm_lane
        step = self._step_body(workflow, "Link the browser GPU bridge")
        self.assertIn("continue-on-error: true", workflow)
        self.assertNotIn("continue-on-error", step)
        self._assert_editor_wasm_browser_bridge_link(workflow, lane)
        link = lane.split("\nlink_bridge() {\n", 1)[1].split("\n}\n", 1)[0]
        for defect in ("continue-on-error", "missing-module", "missing-audit", "masked-failure"):
            with self.subTest(defect=defect):
                changed_workflow, changed_lane = workflow, lane
                if defect == "continue-on-error":
                    changed_workflow = workflow.replace(
                        step, "        continue-on-error: true\n" + step, 1)
                elif defect == "masked-failure":
                    changed_lane = lane.replace(
                        link, link.replace("//tools/ci:geode_wasm_browser_default_audit",
                                           "//tools/ci:geode_wasm_browser_default_audit || true"),
                        1)
                else:
                    target = (
                        "//tools/ci:geode_wasm_browser_module" if defect == "missing-module"
                        else "//tools/ci:geode_wasm_browser_default_audit"
                    )
                    # A later mention cannot satisfy the owning lane function's contract.
                    changed_lane = lane.replace(
                        link, link.replace(target, "//tools/ci:unrelated_target"), 1)
                    changed_lane += "\n# Later target mention: " + target + "\n"
                self.assertNotEqual((changed_workflow, changed_lane), (workflow, lane))
                with self.assertRaises(AssertionError):
                    self._assert_editor_wasm_browser_bridge_link(changed_workflow, changed_lane)

    def _size_check_phase(self):
        """The lane commands the size-check step runs: its one call's `build` function."""
        self.assertIn("        run: tools/ci/editor_wasm_lane.sh build\n", self._step_body(
            self.editor_wasm, "Build and size-check Geode editor Wasm package"))
        return self._lane_function("build")

    def test_editor_wasm_payload_ceilings_are_enforced(self):
        """The size test declares each enforced ceiling once, and CI cannot relax it."""
        self.assertEqual(
            _declared_payload_ceilings(self.wasm_size_test_arguments),
            ENFORCED_PAYLOAD_CEILINGS,
            "%s and ENFORCED_PAYLOAD_CEILINGS must change together" % PAYLOAD_SIZE_TEST,
        )
        self.assertNotIn("--test_arg", self._size_check_phase())
        self.assertNotIn("--test_arg", self.editor_wasm)
        self.assertEqual(0, self.editor_wasm.count("payload-budget-mode"))
        self.assertEqual(0, self.editor_wasm_lane.count("payload-budget-mode"))

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

    def _renderer_size_test_definition(self):
        marker = 'name = "wasm_size_tests"'
        self.assertEqual(1, self.renderer_wasm_build.count(marker))
        return self.renderer_wasm_build.split(marker, 1)[1].split("\n)", 1)[0]

    def test_renderer_wasm_payload_ceilings_are_enforced(self):
        """The renderer module's size test declares each ceiling once, as CI enforces it."""
        self.assertEqual(
            _declared_payload_ceilings(
                self._renderer_size_test_definition(), RENDERER_PAYLOAD_CEILINGS
            ),
            RENDERER_PAYLOAD_CEILINGS,
            "%s and RENDERER_PAYLOAD_CEILINGS must change together" % RENDERER_SIZE_TEST,
        )

    def test_renderer_wasm_size_test_runs_in_the_size_check_step(self):
        """The Editor Wasm size-check step runs the suite that contains the renderer gate."""
        step = self._size_check_phase()
        self.assertIn("--config=wasm-size", step)
        self.assertIn("//tools/ci:wasm_size_tests", step)
        suite_name = 'name = "wasm_size_tests"'
        self.assertIn(suite_name, self.ci_target_definitions)
        suite = self.ci_target_definitions.split(suite_name, 1)[1].split("\n)", 1)[0]
        self.assertIn(RENDERER_SIZE_TEST, suite)

    def test_editor_wasm_size_check_step_runs_the_gated_size_test(self):
        """The gate reads the ceilings of the target the workflow actually runs."""
        step = self._size_check_phase()
        self.assertIn("//tools/ci:editor_wasm_size_tests", step)
        suite_name = 'name = "editor_wasm_size_tests"'
        self.assertIn(suite_name, self.ci_target_definitions)
        suite = self.ci_target_definitions.split(suite_name, 1)[1].split("\n)", 1)[0]
        self.assertIn(PAYLOAD_SIZE_TEST, suite)

    def _run_heartbeat_fixture(self, wrapper, log, env):
        return subprocess.run(
            ["/bin/bash", str(wrapper), str(log), "/bin/bash", "-c", "exit 17"],
            check=False,
            capture_output=True,
            text=True,
            env=env,
            timeout=test_timing.STALL_GUARD_SECONDS,
        )

    def test_heartbeat_fixture_ignores_unrelated_inherited_bash(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wrapper = root / "wrapper.sh"
            wrapper.write_text(self._heartbeat_script())
            wrapper.chmod(0o755)
            ambient = root / "ambient"
            ambient.mkdir()
            bash = ambient / "bash"
            bash.write_text("#!/bin/sh\necho unexpected-inherited-bash >&2\nexit 73\n")
            bash.chmod(0o755)
            env = dict(os.environ, PATH=str(ambient) + os.pathsep + os.environ["PATH"],
                       BAZEL_HEARTBEAT_INTERVAL_SECONDS="300")
            result = self._run_heartbeat_fixture(wrapper, root / "command.log", env)
            self.assertEqual(result.returncode, 17, result.stderr)
            self.assertEqual([], list(root.glob("*.heartbeat-control.*")))

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
            # The 300 s interval far outlasts the stall guard, so a sleeper left holding the pipe
            # fails the case.
            env["BAZEL_HEARTBEAT_INTERVAL_SECONDS"] = "300"

            result = self._run_heartbeat_fixture(wrapper, log, env)

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
            # The 300 s interval far outlasts the stall guard, so a watcher that misses the stop
            # fails the case.
            result = self._run_script(fixture, [log])
            self.assertEqual(23, result.returncode, result.stderr)
            self.assertEqual([], list(Path(temp_dir).glob("*.progress-control.*")))

    def test_quiet_coverage_progress_hides_raw_targets_and_runner_paths(self):
        functions = self.coverage_script.split("\nTARGETS=()", 1)[0]
        # The command stays alive until the watcher has reported its progress line, so the
        # report does not depend on a watcher tick landing within a fixed sleep. It also stops
        # once the fixture shell is gone, so a hung case leaves no process behind.
        fixture = functions + """

DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS=1
export DONNER_COVERAGE_PROGRESS_INTERVAL_SECONDS
report="$1.report"
status=0
run_quiet_with_progress "fixture" "$1" bash -c '
  printf "[1 / 2] Testing //donner/editor:sample_test /runner/secret\\n"
  until grep -qF "[1 / 2]" "$1" || ! kill -0 "$PPID" 2>/dev/null; do sleep 0.1; done
  exit 23' fixture "$report" > "$report" || status=$?
cat "$report"
exit "$status"
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            log = str(Path(temp_dir) / "coverage.log")
            result = self._run_script(fixture, [log])
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
