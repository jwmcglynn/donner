"""Pins the self-hosted CI runtime boundaries that keep full runs viable."""

import ast
import itertools
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
from tools import bazelrc_configs
from tools import test_timing
from tools.ci import lane as lane_entry
from tools.ci import lane_view
from tools.ci import lanes


# The editor Wasm payload ceilings CI enforces. Changing one means editing the size test's
# arguments and this table together, so a widened ceiling is always a reviewed change.
ENFORCED_PAYLOAD_CEILINGS = {
    "--max-js-gzip-bytes": 63000,
    "--max-js-raw-bytes": 227000,
    "--max-total-raw-bytes": 14880000,
    "--max-wasm-data-segments": 64,
    "--max-wasm-function-body-bytes": 46000,
    "--max-wasm-gzip-bytes": 3570000,
    "--max-wasm-raw-bytes": 10880000,
}
PAYLOAD_SIZE_TEST = "//donner/editor/wasm:wasm_geode_package_size_tests"
# The standalone renderer Wasm ceilings CI enforces, changed together with that test's arguments.
RENDERER_PAYLOAD_CEILINGS = {
    "--max-js-gzip-bytes": 3100,
    "--max-js-raw-bytes": 6200,
    "--max-wasm-gzip-bytes": 1020000,
    "--max-wasm-raw-bytes": 2500000,
}
RENDERER_SIZE_TEST = "//donner/svg/renderer/wasm:wasm_size_tests"
SIZE_CHECK_STEP = "- name: Build and size-check Geode editor Wasm package"
# Scripts that hold step bodies moved out of a workflow. The target selection treats a change to
# one exactly as a change to the CI workflow.
WORKFLOW_LOGIC_SCRIPTS = ("tools/ci/bazel_watchdog.sh", "tools/ci/determine_targets.sh",
                          "tools/ci/fetch_bazel_diff.sh", "tools/ci/lane.py", "tools/ci/lanes.py")
# The start of the target selection's list of those scripts.
WORKFLOW_LOGIC_MARKER = "          # Workflow logic kept in scripts"
# Other repository scripts the CI workflow and its composite actions run, each with the test
# target whose data lists it, so bazel-diff selects that test when the script changes. None marks
# a script no target lists yet; the reason says why that is accepted.
TRACKED_CI_SCRIPTS = {
    "tools/bazelrc_configs.py": "//tools:bazelrc_configs",
    "tools/browser_gpu_test_summary.py": "//tools:browser_gpu_test_summary_tests",
    "tools/ci/darwin_selection.py": "//tools/ci:darwin_selection_tests",
    "tools/ci/free_hosted_macos_disk.sh": "//tools/ci:free_hosted_macos_disk_tests",
    "tools/ci/gatekeeper.py": "//tools/ci:gatekeeper_tests",
    # The advisory macOS shadow lane: its library's only user is
    # //tools:ci_macos_remote_shadow_tests, which bazel-diff selects when the script changes. It is
    # not workflow logic: a change to it must not send every gating lane to full coverage.
    "tools/ci/macos_remote_shadow.py": "//tools/ci:macos_remote_shadow",
    "tools/ci/verify_unified_tests.py": "//tools/ci:verify_unified_tests_tests",
    "tools/ci_bazel_test.sh": "//tools:ci_bazel_test_tests",
    "tools/ci_coverage_contract.py": "//tools:ci_coverage_contract_tests",
    "tools/ci_diagnostics_report.py": "//tools:ci_diagnostics_report_tests",
    "tools/ci_disk_cache.py": "//tools:ci_disk_cache_tests",
    "tools/ci_remote_gpu_targets.py": "//tools:ci_remote_gpu_targets",
    "tools/ci_select_macos_xcode.sh": "//tools:macos_cache_key_parity_tests",
}
UNTRACKED_CI_SCRIPTS = {
    "tools/ci_check_runner_disk.sh":
        "Fails a step early when the runner's disk is short; no test lists it yet, so a change "
        "to it is covered only by the runs that use it.",
}
_RUN = re.compile(r"^(?P<indent> *)(?P<item>- )?run: *(?P<value>.*)$")
_SCRIPT_PATH = re.compile(r"(?<![\w/.-])((?:tools|build_defs)/[\w./-]+\.(?:sh|py))\b")
# Each Editor WASM build step that runs Bazel or stages an artifact, in workflow order, with the one
# command it runs. The commands themselves are pinned by //tools/ci:editor_wasm_lane_tests and
# //tools/ci:editor_wasm_stage_tests.
EDITOR_WASM_SCRIPT_STEPS = (
    ("Prefetch Editor WASM dependencies", "tools/ci/editor_wasm_lane.sh prefetch"),
    ("Retry Editor WASM dependency prefetch", "tools/ci/editor_wasm_lane.sh prefetch"),
    ("Build and size-check Geode editor Wasm package", "tools/ci/editor_wasm_lane.sh build"),
    ("Snapshot the browser-tested release files", "python3 tools/ci/editor_wasm_stage.py snapshot"),
    ("Prove the package's resolved module graph",
     'tools/ci/editor_wasm_lane.sh prove-lockfile "$RUNNER_TEMP/editor-wasm-module.lock"'),
    ("Stage package for handoff", "python3 tools/ci/editor_wasm_stage.py package"),
    ("Run quarantined Firefox cases", "tools/ci/editor_wasm_lane.sh quarantine"),
    ("Stage immutable Geode deployment candidate",
     "python3 tools/ci/editor_wasm_stage.py candidate"),
    ("Record deployment candidate digest", "python3 tools/ci/editor_wasm_stage.py record-digest"),
)
def _repository_text(path):
    resolver = runfiles.Create()
    resolved = resolver.Rlocation("donner/%s" % path)
    with open(resolved, encoding="utf-8") as handle:
        return handle.read()


def _rule_text(build_text, name):
    """The attributes of the BUILD rule named `name`."""
    return build_text.split('name = "%s",' % name, 1)[1].split("\n)\n", 1)[0]


def _assignment_block(build_text, name):
    """The body of a top-level `name = {...}` assignment in a BUILD file."""
    return build_text.split("\n%s = " % name, 1)[1].split("\n}", 1)[0]


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


_HEREDOC_COMMAND = re.compile(r"^\s*(?:cat|tee)\b[^\n]*<<[^\n]*$", re.MULTILINE)
_FILE_WRITE = re.compile(r"(?:^|\s)(?:>>?|tee)\s*[^\s|&;<>]")


def _heredoc_file_writes(text):
    """Lines that write a heredoc to a file with `cat` or `tee`."""
    return [line.strip() for line in _HEREDOC_COMMAND.findall(text)
            if _FILE_WRITE.search(re.sub(r"<<-?\s*\S+", "", line))]


def _block_lines(lines, indent):
    """The leading lines indented past `indent`, as a block scalar's content, without comments."""
    body = []
    for line in lines:
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        body.append(line)
    return [line for line in body if not line.lstrip().startswith("#")]


def _run_bodies(text):
    """The commands of every `run:` step in a workflow or action, without comment lines."""
    lines = text.splitlines()
    bodies = []
    for index, line in enumerate(lines):
        match = _RUN.match(line)
        value = match.group("value") if match else ""
        if value in ("|", ">", "|-", ">-"):
            indent = len(match.group("indent")) + (2 if match.group("item") else 0)
            bodies.append("\n".join(_block_lines(lines[index + 1:], indent)).rstrip("\n"))
        elif value:
            bodies.append(value)
    return bodies


def _action_input_defaults(text):
    """The default values of a composite action's inputs, which its steps may run as scripts."""
    parts = re.split(r"^inputs:[ \t]*\n", text, maxsplit=1, flags=re.MULTILINE)
    inputs = re.split(r"^\S", parts[1], maxsplit=1, flags=re.MULTILINE)[0] if len(parts) > 1 else ""
    return re.findall(r"^ +default: *(.*)$", inputs, re.MULTILINE)


_PYTHON_MODULE = re.compile(r"\bpython3\b[^\n|;&]*?\s-m\s+((?:tools|build_defs)(?:\.\w+)+)")


def _script_references(text):
    """The repository scripts, and the modules run with `python3 -m`, that commands name."""
    text = re.sub(r"\$\{?GITHUB_WORKSPACE\}?/", "", text)
    return (set(_SCRIPT_PATH.findall(text))
            | {module.replace(".", "/") + ".py" for module in _PYTHON_MODULE.findall(text)})


def _resolve_import(path, names):
    """The file of the first of `names` found beside `path` or at the repository root."""
    for name in names:
        for base in (path.rsplit("/", 1)[0] + "/", ""):
            candidate = base + name.replace(".", "/") + ".py"
            if Path(runfiles.Create().Rlocation("donner/" + candidate)).is_file():
                return candidate
    raise AssertionError("%s imports %s, which is not in this test's data" % (path, names[-1]))


def _python_imports(path):
    """The repository modules a Python script imports.

    Every import outside the standard library must resolve to a file in this test's data, so a
    new import fails here instead of being skipped.
    """
    return {_resolve_import(path, names)
            for node in ast.walk(ast.parse(_repository_text(path)))
            for names in _import_choices(node)
            if names[-1].split(".")[0] not in sys.stdlib_module_names}


def _import_choices(node):
    """For each name an import statement binds, the modules that may provide it, in order."""
    if isinstance(node, ast.Import):
        return [[alias.name] for alias in node.names]
    if isinstance(node, ast.ImportFrom) and node.module and not node.level:
        return [[node.module + "." + alias.name, node.module] for alias in node.names]
    return []


def _phases_testing_the_selection():
    """(lane, phase, actions, test actions) of each lane phase that tests the planner's selection
    or a subset of it."""
    found = []
    for lane, entry in lanes.LANES.items():
        for phase, definition in entry.phases.items():
            tests = [action for action in definition.actions
                     if isinstance(action, lanes.Bazel) and action.args[0] == "test"]
            if tests and _reads_selection(definition.actions):
                found.append((lane, phase, definition.actions, tests))
    return found


def _reads_selection(actions):
    """Whether a phase's commands or queries read the planner's selection."""
    return any(isinstance(action, lanes.QueryTag) or (isinstance(action, lanes.Bazel) and any(
        isinstance(item, lanes.Selected) for item in action.args)) for action in actions)


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
        cls.configs = bazelrc_configs.parse(cls.bazelrc)
        # main.yml with each lane step's commands (tools/ci/lanes.py) under its `run:` line.
        cls.main_text = _repository_text(".github/workflows/main.yml")
        cls.main = lane_view.with_dry_runs(cls.main_text)
        cls.cmake = _repository_text(".github/workflows/cmake.yml")
        cls.coverage = _repository_text(".github/workflows/coverage.yml")
        cls.editor_wasm = _repository_text(".github/workflows/editor_wasm.yml")
        cls.editor_wasm_lane = _repository_text("tools/ci/editor_wasm_lane.sh")
        cls.lint = _repository_text(".github/workflows/lint.yml")
        cls.sanitizers_pr = _repository_text(".github/workflows/sanitizers-pr.yml")
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
        cls.bazel_watchdog = _repository_text("tools/ci/bazel_watchdog.sh")
        cls.determine_targets = _repository_text("tools/ci/determine_targets.sh")

    def _job_body(self, job, workflow=None):
        workflow = self.main if workflow is None else workflow
        marker = "\n  %s:\n" % job
        self.assertIn(marker, workflow, "job %s not found" % job)
        rest = workflow.split(marker, 1)[1]
        end = re.search(r"^  [A-Za-z0-9_-]+:\s*$", rest, re.MULTILINE)
        return rest[: end.start()] if end else rest

    def _mixed_step(self):
        return self._step_body(self._job_body("linux-self-hosted"), "Test Linux and macOS with native platform transitions")

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

    @staticmethod
    def _phase_commands(lane, phase):
        """The argv of each command a lane phase runs, from its dry run."""
        return lane_view.phase_commands(lane, phase)

    def _expanded_commands(self, lane, phase):
        """A phase's commands with each repository config replaced by its flags."""
        return lane_view.phase_commands(lane, phase, self.configs)

    def _lane_test_commands(self, lane):
        """Every expanded `bazel test` command of a lane's phases."""
        return [command for phase in lanes.LANES[lane].phases
                for command in self._expanded_commands(lane, phase)
                if "bazelisk" in command and "test" in command[:command.index("bazelisk") + 4]]

    def _phases_with(self, word):
        """(lane, phase) of every lane table phase with a command that passes `word`, as written
        or from a repository config."""
        return {(lane, phase) for lane, entry in lanes.LANES.items() for phase in entry.phases
                if any(word in argv for argv in self._phase_commands(lane, phase)
                       + self._expanded_commands(lane, phase))}

    def test_lint_runs_changed_files_on_a_shallow_hosted_checkout(self):
        job = self._job_body("lint", self.lint)
        self.assertIn("runs-on: ubuntu-26.04", job)
        self.assertNotIn("self-hosted", self.lint)
        self.assertEqual(self.lint.count("uses: actions/checkout@"), 1)
        self.assertIn("fetch-depth: 2", job)
        self.assertIn("persist-credentials: false", job)
        self.assertIn('run: tools/ci/lint_lane.sh --changed "$LINT_BASE"', job)
        self.assertIn("LINT_BASE: ${{ steps.changes.outputs.base }}", job)
        self.assertNotIn("docs_only", job)
        self.assertIsNone(re.search(r"(?m)^ +run: .*gen_cmakelists\.py", self.lint))

    @staticmethod
    def _variables(job):
        """The job's and its steps' literal `env:` values (`NAME: "value"` or `NAME: value`).
        A value computed at run time (`${{ }}`) or a block scalar is left out, so its variable
        stays a word."""
        return {name: value.strip('"') for name, value in re.findall(
            r"(?m)^ +([A-Z][A-Z0-9_]*): (\"[^\"\n]*\"|[^\s\"${>|][^\n]*)$", job)}

    def _test_options(self, step, job):
        """The options of a step's one `bazel test` command, with the job's literal env values
        substituted and each repository `--config` replaced by its flags, so a lane's filters
        can move into a .bazelrc config or a flags variable."""
        tests = [options for command, options, _ in
                 bazelrc_configs.commands(step, self.configs, self._variables(job))
                 if command == "test"]
        self.assertEqual(1, len(tests), step)
        return tests[0]

    def _tag_filters(self, step, job):
        return bazelrc_configs.last_value(self._test_options(step, job), "test_tag_filters")

    def _job_tag_filters(self, job):
        """The effective tag filters of every `bazel test` command in a job."""
        return [bazelrc_configs.last_value(options, "test_tag_filters")
                for command, options, _ in
                bazelrc_configs.commands(job, self.configs, self._variables(job))
                if command == "test"]

    def test_context_diagnostic_is_a_fixed_trusted_opt_in_in_the_graph(self):
        mixed = self._mixed_step()
        self.assertIn("if: needs.gatekeeper.outputs.unified_macos == 'true'", mixed)
        self.assertIn("contains(github.event.pull_request.labels.*.name, 'ci:browser-context-probe')", mixed)
        diagnostic = mixed.split('if [[ "$BROWSER_DIAGNOSTIC" == true ]]; then', 1)[1].split("\n          fi", 1)[0]
        self.assertIn("suites+=(//tools/ci:unified_browser_diagnostic)", diagnostic)
        self.assertNotIn("continue-on-error", mixed)
        suite = _rule_text(self.ci_target_definitions, "unified_browser_diagnostic")
        self.assertIn('tags = ["manual"]', suite)
        self.assertIn("ci_darwin_diagnostic_chromium_context_lifecycle_diagnostic", suite)
        self.assertIn('timeout = "short"', _rule_text(_repository_text("donner/editor/wasm/tests/BUILD.bazel"), "chromium_context_lifecycle_diagnostic"))

    def test_all_arm64_linux_test_phases_retain_failure_artifacts(self):
        job = self._job_body("linux-self-hosted")
        upload = self._step_body(job, "Upload Bazel test failure artifacts")
        condition = next(line.strip() for line in upload.splitlines() if line.strip().startswith("if:"))
        self.assertTrue(condition.startswith("if: failure() && "))
        for step_id in ("test",):
            with self.subTest(step=step_id):
                self.assertIn("steps.%s.outcome == 'failure'" % step_id, condition)
        self.assertIn("uses: ./.github/actions/upload-bazel-test-artifacts", upload)

    def test_early_macos_bazel_failures_keep_their_artifacts(self):
        hosted = self._job_body("macos")
        preflight = self._step_body(hosted, "Qualify pinned Chromium archive")
        self.assertIn("id: browser_preflight", preflight)
        self.assertNotIn("continue-on-error", preflight)
        self.assertIn("steps.browser_preflight.outcome == 'failure'",
                      self._step_body(hosted, "Upload Bazel test failure artifacts"))
        mixed = self._job_body("linux-self-hosted")
        for step in ("Stage failed mixed-platform test outputs", "Upload mixed-platform test failure artifacts"):
            self.assertIn("if: failure() && steps.unified.outcome == 'failure'", self._step_body(mixed, step))

    def test_hosted_browser_comparisons_preserve_the_failed_gate(self):
        hosted = self._job_body("macos")
        original = self._step_body(hosted, "Build and test")
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
        targets = ("standalone_geode_browser_renderer_test",)
        metadata = _repository_text("tools/ci/BUILD.bazel")
        suite = re.search(r'test_suite\(\s+name = "browser_gpu_comparison",.*?\n\)',
                          metadata, re.DOTALL)
        self.assertIsNotNone(suite, "browser comparison suite must be declared")
        # The comparison reruns exactly the Bazel browser lanes the detection step reads, and no
        # lane whose Chromium cases moved to the Editor WASM chromium-default lane.
        self.assertEqual(
            sorted(re.findall(r'"//donner/editor/wasm/tests:([^"]+)"', suite.group())),
            sorted(targets))
        self.assertIn("for target in %s; do" % " ".join(targets), detection)
        for mode, jobs in (("serial", 1), ("parallel", 4)):
            name = "Compare browser GPU tests (%s)" % mode
            body = self._step_body(hosted, name)
            self.assertGreater(hosted.index("      - name: " + name), original_artifacts)
            self.assertIn("!cancelled() && steps.browser_gpu_failure.outputs.run_probe == 'true'", body)
            self.assertIn("continue-on-error: true", body)
            self.assertIn("--nocache_test_results", body)
            self.assertIn("--local_test_jobs=%d" % jobs, body)
            [command, summary] = self._expanded_commands("macos", "browser_gpu_" + mode)
            for flag in bazelrc_configs.expand(
                    "test", ["--config=lane-macos", "--config=lane-macos-hosted"], self.configs):
                self.assertIn(flag, command)
            self.assertEqual("//tools/ci:browser_gpu_comparison", command[-1])
            self.assertEqual(["python3", "tools/browser_gpu_test_summary.py"], summary[:2])
            artifact = self._step_body(hosted, "Upload browser GPU comparison (%s)" % mode)
            self.assertIn("./.github/actions/upload-bazel-test-artifacts", artifact)
            self.assertIn("browser-gpu-%s-" % mode, artifact)

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
        for job in ("linux", "linux-self-hosted"):
            body = self._job_body(job)
            self.assertNotIn(name, body)
            self.assertNotIn("log show", body)

    def test_macos_build_and_test_builds_the_complete_selection_once(self):
        hosted = self._job_body("macos")
        self.assertNotIn("      - name: Build\n", hosted)
        [command] = self._expanded_commands("macos", "test")
        self.assertEqual(command[-2:], ["--nobuild_tests_only", "@selected_targets"])
        self.assertIn("tools/ci_bazel_test.sh", command)
        mixed = self._mixed_step()
        self.assertIn("--nobuild_tests_only", mixed)
        self.assertIn('cat "$ordinary" "$linux"', mixed)
        self.assertIn('--target_pattern_file="$patterns"', mixed)

    def test_hosted_browser_tests_run_alone_after_the_suite(self):
        # A browser starved of CPU misses its wall-clock deadlines, so on the three-core
        # hosted runner every test that launches a browser runs after the rest of the
        # suite, one at a time, and nowhere else in that job.
        hosted = self._job_body("macos")
        main_test = self._step_body(hosted, "Build and test")
        self.assertEqual(
            "-manual,-perf,-requires_metal_toolchain,-requires_shader_validation,"
            "-ci-remote-gpu,-launches-browser,-fuzz_soak",
            self._tag_filters(main_test, hosted),
        )
        browser = self._step_body(hosted, "Test (browser targets)")
        self.assertLess(hosted.index("      - name: Build and test\n"),
                        hosted.index("      - name: Test (browser targets)\n"))
        self.assertLess(hosted.index("      - name: Test (browser targets)\n"),
                        hosted.index("      - name: Upload Bazel test failure artifacts\n"))
        self.assertIn("id: browser_test", browser)
        self.assertIn(
            "if: ${{ !cancelled() && (steps.test.outcome == 'success' || "
            "steps.test.outcome == 'failure') }}",
            browser,
        )
        self.assertIn("--local_test_jobs=1", self._test_options(browser, hosted))
        self.assertEqual(
            "launches-browser,-manual,-perf,-requires_metal_toolchain,-ci-remote-gpu,-editor-wasm-browser,-fuzz_soak",
            self._tag_filters(browser, hosted),
        )
        [command] = self._phase_commands("macos", "browser_test")
        self.assertEqual(command[-2:], [lanes.BROWSER_STALL[0], "@selected_targets"])
        # Every step that reacts to a failed test step also reacts to the browser phase.
        for step in ("Upload Bazel test failure artifacts", "Collect browser stall system log",
                     "Upload browser stall system log", "Detect browser GPU acquisition failure"):
            body = self._step_body(hosted, step)
            self.assertIn("steps.test.outcome == 'failure'", body, step)
            self.assertIn("steps.browser_test.outcome == 'failure'", body, step)
        # Other jobs keep their own scheduling.
        for job in ("linux", "linux-self-hosted"):
            self.assertNotIn("launches-browser", self._job_body(job), job)

    def test_native_browser_roots_join_the_same_iteration_as_the_selected_darwin_build(self):
        step = self._mixed_step()
        self.assertEqual(step.count('darwin_selection.py variants < "$linux.jsonl" > "$ordinary"'), 1)
        self.assertEqual(step.count('--target_pattern_file="$patterns"'), 1)
        preflight = step.split('if [[ "$RUN_BROWSER" == true ]]; then', 1)[1].split("\n          fi", 1)[0]
        self.assertIn("suites+=(//tools/ci:unified_mac_browser_archive)", preflight)
        self.assertIn('--ordinary "$ordinary" --linux "$linux"', step)
        self.assertNotIn("continue-on-error", step)

    def test_geode_phase_runs_after_a_completed_native_phase_even_if_browser_fails(self):
        job_name = "macos"
        job = self._job_body(job_name)
        geode = self._step_body(job, "Test (Geode editor lane)")
        self.assertIn(
            "if: ${{ !cancelled() && (steps.test.outcome == 'success' || "
            "steps.test.outcome == 'failure') }}", geode)
        self.assertNotIn("steps.browser_test.outcome", geode)
        self.assertLess(job.index("      - name: Test (browser targets)\n"),
                        job.index("      - name: Test (Geode editor lane)\n"))
        self.assertIn(" " + lanes.GEODE_EDITOR + "\n", geode)

        self.assertIn("suites+=(//tools/ci:unified_editor_geode)", self._mixed_step())
        self.assertIn("--test_keep_going", self._mixed_step())
        original = _rule_text(self.ci_target_definitions, "editor_geode")
        mixed = _rule_text(self.ci_target_definitions, "unified_editor_geode")
        expected = {label.replace(":", ":ci_darwin_geode_") for label in re.findall(r'"(//[^"]+)"', original)}
        self.assertEqual(set(re.findall(r'"(//[^"]+)"', mixed)), expected)
        self.assertEqual(len(expected), 6)

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
            "docs_selector_browser_tests",
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
        self.assertIn("        name: Determine affected Bazel targets\n        shell: bash\n"
                      "        run: tools/ci/determine_targets.sh determine\n", determine)
        script = self._selection_script()
        query = 'attr(\\"tags\\", \\"launches-browser\\", set($affected))'
        self.assertIn(query, script)
        # The query runs on the filtered selection, against the PR's merge graph.
        self.assertLess(script.index("manual_affected="), script.index(query))
        self.assertIn('cd "$head_dir"', script.split(query, 1)[0].rsplit("if ", 1)[1])
        failed = script.split('echo "Browser target query failed', 1)[1].split("fi\n", 1)[0]
        self.assertIn('echo "browser_affected=true" >> "$GITHUB_OUTPUT"', failed)
        # The query's two results, its failure, and both proven empty selections.
        self.assertEqual(script.count("browser_affected="), 5)
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

    def test_native_browser_retains_test_artifacts_without_host_diagnostics(self):
        job = self._job_body("linux-self-hosted")
        archive = self._step_body(job, "Upload mixed-platform test failure artifacts")
        self.assertIn("./.github/actions/upload-bazel-test-artifacts", archive)
        self.assertIn("testlogs: ${{ runner.temp }}/unified/test-artifacts/mixed", archive)
        for forbidden in ("browser-stall-system-log", "Collect browser stall system log",
                          "Compare browser GPU tests", "Probe runner resources"):
            self.assertNotIn(forbidden, job)

    def test_browser_stall_recorder_is_enabled_only_in_the_hosted_job(self):
        # The recorder publishes host load, memory and call graphs, so only the ephemeral
        # hosted job may enable it; every other lane and local runs leave it inert.
        switch = '--test_env=DONNER_BROWSER_STALL_DIAGNOSTICS=1'
        hosted = self._job_body("macos")
        self.assertEqual(lanes.BROWSER_STALL, (switch,))
        steps = ("Build and test", "Test (browser targets)", "Compare browser GPU tests (serial)",
                 "Compare browser GPU tests (parallel)")
        for step in steps:
            self.assertIn(switch, self._step_body(hosted, step), step)
        # Only those phases of the hosted lane pass it.
        self.assertEqual(self._phases_with(switch), {
            ("macos", "test"), ("macos", "browser_test"), ("macos", "browser_gpu_serial"),
            ("macos", "browser_gpu_parallel")})
        recorder = _repository_text("donner/editor/wasm/tests/browser-stall-diagnostics.mjs")
        self.assertIn('kEnableVariable = "DONNER_BROWSER_STALL_DIAGNOSTICS"', recorder)
        self.assertEqual(_repository_text("tools/ci/lanes.py").count(switch), 1)
        self.assertNotIn("BROWSER_STALL", self.main_text)
        for job in ("linux", "linux-self-hosted"):
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
        for rule_file in ("build_defs/rules.bzl", "build_defs/ci_darwin.bzl"):
            self.assertNotIn("DONNER_BROWSER_STALL_DIAGNOSTICS", _repository_text(rule_file))

    def test_hosted_macos_quarantine_is_set_only_for_the_hosted_test_environment(self):
        # Main's hosted macOS job skips browser cases quarantined for that runner alone (#1691,
        # #1702). The switch is one .bazelrc config, passed only by the gating test steps of the
        # jobs that reproduce that job's test environment, so they skip exactly the same cases.
        # Every other lane and local runs must keep running them.
        readers = {
            "macos": ("Build and test", "Test (browser targets)"),
        }
        for job_name, steps in readers.items():
            job = self._job_body(job_name)
            for step in steps:
                self.assertIn("--config=hosted-macos-quarantine", self._step_body(job, step),
                              (job_name, step))
        self.assertEqual(self.main.count("--config=hosted-macos-quarantine"),
                         sum(len(steps) for steps in readers.values()))
        # No pull request job reproduces that environment, so no other job passes it.
        self.assertNotIn("DONNER_HOSTED_MACOS_QUARANTINE", self.main)
        hosted = self._job_body("macos")
        for step in ("Build and test", "Test (browser targets)"):
            self.assertIn(lanes.BROWSER_STALL[0], self._step_body(hosted, step), step)
        # The advisory comparisons never set the job's outcome and exist to diagnose exactly
        # these failures, so they keep running the quarantined cases.
        for mode in ("serial", "parallel"):
            step = "Compare browser GPU tests (%s)" % mode
            body = self._step_body(hosted, step)
            self.assertIn("continue-on-error: true", body, step)
            for name in ("hosted-macos-quarantine", "DONNER_HOSTED_MACOS_QUARANTINE"):
                self.assertNotIn(name, body, step)
        for job in ("linux", "linux-self-hosted"):
            for name in ("hosted-macos-quarantine", "DONNER_HOSTED_MACOS_QUARANTINE"):
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
        # The lane table passes the config in those two phases alone.
        self.assertEqual(self._phases_with("--config=hosted-macos-quarantine"),
                         {("macos", "test"), ("macos", "browser_test")})
        # In the repository bazelrc it is one named config, which no other config or default
        # names.
        self.assertEqual(
            [line for line in self.bazelrc.splitlines() if "HOSTED_MACOS_QUARANTINE" in line
             or "hosted-macos-quarantine" in line],
            ["test:hosted-macos-quarantine --test_env=DONNER_HOSTED_MACOS_QUARANTINE=1"])

    def test_quarantine_report_lane_is_enabled_only_by_the_scheduled_editor_wasm_run(self):
        # The report-only lane lifts the Firefox quarantine of #1634 and never fails its job.
        # Only the nightly Editor WASM run may enable it: pull request and push runs keep the
        # quarantine, so a quarantined case can neither block a merge nor turn main red.
        resolver = runfiles.Create()
        workflows = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent
        editor = self.editor_wasm
        gate = self._step_body(editor, "Run quarantined Firefox cases")
        self.assertIn("github.event_name == 'schedule'", gate)
        self.assertIn("steps.wasm_graph.outcome == 'success' || steps.wasm_graph.outcome == 'failure'", gate)
        self.assertIn("continue-on-error: true", gate)
        self.assertIn("run: tools/ci/editor_wasm_lane.sh quarantine", gate)
        for name in ("Collect quarantine evidence", "Summarize quarantined cases", "Upload quarantined case evidence"):
            self.assertIn("github.event_name == 'schedule'", self._step_body(editor, name))
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

    def test_editor_wasm_runs_on_every_input_of_its_chromium_lane(self):
        # The chromium-default lane is the only Chromium run of the editor's browser specs. A
        # change to anything its served package, comparator or browser specs are built from must
        # start it: the editor sources, the module extension that configures them, the notice
        # renderer embedded in the package and the package that defines it, the native pixel
        # comparator, the build setup, and the scripts the job itself runs (the Xcode selection
        # and its pin file, the runner disk check).
        header = self.editor_wasm.split("\njobs:", 1)[0]
        triggers = {}
        for event in ("push", "pull_request"):
            body = header.split("\n  %s:\n" % event, 1)[1].split("\n  schedule:", 1)[0]
            if event == "push":
                body = body.split("\n  pull_request:", 1)[0]
            triggers[event] = re.findall(r'^      - "([^"]+)"$', body, re.MULTILINE)
        self.assertEqual(triggers["push"], triggers["pull_request"])
        for path in (".bazelrc", ".bazelversion", "BUILD.bazel", "MODULE.bazel",
                     "MODULE.bazel.lock", "build_defs/**", "config/**", "donner/**",
                     "third_party/**", "tools/BUILD.bazel", "tools/ci/**",
                     "tools/ci_check_runner_disk.sh", "tools/ci_disk_cache.py",
                     "tools/ci_macos_xcode.txt", "tools/ci_select_macos_xcode.sh",
                     "tools/http/**", "tools/render_notice.py", "tools/run-browser-ci.sh",
                     "tools/svg_suite/**"):
            self.assertIn(path, triggers["pull_request"])

    def test_headed_browser_tests_have_a_single_native_owner(self):
        hosted = self._job_body("macos")
        self.assertIn("unified_macos != 'true'", hosted.split("steps:", 1)[0])
        body = self._step_body(hosted, "Test (browser targets)")
        self.assertIn("launches-browser", self._tag_filters(body, hosted).split(","))
        self.assertNotIn("continue-on-error: true", body)
        self.assertIn("unified_macos == 'true'", self._mixed_step())
        self.assertNotIn("runs-on: [self-hosted, macOS", self.main)

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
        return self.bazel_watchdog

    def _selection_script(self):
        """The body of determine_targets.sh's `determine`, indented as a workflow step body.

        Fragments cut from it dedent to the commands the determine-targets job runs.
        """
        body = self.determine_targets.split("\ndetermine() {\n", 1)[1].split("\n}\n", 1)[0]
        return "".join(" " * 8 + line if line.strip() else line
                       for line in body.splitlines(keepends=True))

    def _workflow_logic_function(self, determine):
        """The selection's `is_workflow_logic` function, which lists WORKFLOW_LOGIC_SCRIPTS."""
        start = determine.index(WORKFLOW_LOGIC_MARKER)
        end = determine.index("\n          }\n", start) + len("\n          }\n")
        return textwrap.dedent(determine[start:end])

    def test_workflow_logic_scripts_select_workflow_coverage(self):
        function = self._workflow_logic_function(self._selection_script())
        listed = [path for arm in re.findall(r"^\s+([\w./|-]+)\)$", function, re.MULTILINE)
                  for path in arm.split("|")]
        self.assertEqual(sorted(listed), sorted(WORKFLOW_LOGIC_SCRIPTS))
        lane_table = _repository_text("tools/ci/lanes.py")
        # The workflow with the composite actions it uses, whose steps run in its jobs.
        workflow = self.main_text + "".join(
            _repository_text(".github/actions/%s/action.yml" % name) for name in
            sorted(set(re.findall(r"uses: \./\.github/actions/([\w-]+)$", self.main_text,
                                  re.MULTILINE))))
        for path in WORKFLOW_LOGIC_SCRIPTS:
            with self.subTest(path=path):
                if path == "tools/ci/lanes.py":
                    # The lane table, which the entry point imports.
                    self.assertIn("from tools.ci import lanes",
                                  _repository_text("tools/ci/lane.py"))
                elif path.endswith(".py"):
                    # Run by its steps as `python3 <path>`.
                    self.assertIn("run: python3 %s " % path, self.main_text)
                else:
                    # A step body that a step, an action input or a lane phase runs, kept
                    # executable.
                    self.assertIn(path, workflow + lane_table)
                    mode = Path(runfiles.Create().Rlocation("donner/" + path)).stat().st_mode
                    self.assertTrue(mode & 0o111, path)

    def test_workflows_do_not_write_scripts_from_heredocs(self):
        """Step logic lives in tested scripts, not in files a step writes from a heredoc."""
        resolver = runfiles.Create()
        workflows = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent
        paths = (sorted(workflows.glob("*.y*ml"))
                 + sorted((workflows.parent / "actions").glob("*/action.y*ml")))
        self.assertGreater(len(paths), 10)
        for path in paths:
            self.assertEqual([], _heredoc_file_writes(path.read_text(encoding="utf-8")),
                             path.name)
        for line in ('cat > "$DIR/run.sh" <<\'EOF\'', "cat <<EOF > run.sh", "tee run.sh <<EOF",
                     "cat >>run.sh <<-EOF"):
            self.assertEqual([line], _heredoc_file_writes("  " + line + "\n"), line)
        for line in ("python3 - <<'PY'", "echo 'cat > /dev/null'", "cat <<EOF | sort",
                     "cat run.sh"):
            self.assertEqual([], _heredoc_file_writes("  " + line + "\n"), line)

    def test_every_script_the_ci_workflow_runs_is_tracked(self):
        """A script the CI workflow runs must reach the target selection some way.

        Either it holds workflow logic and selects full coverage like a workflow change, or a test
        target lists it as data, so bazel-diff selects that test when it changes.
        """
        resolver = runfiles.Create()
        workflows = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent
        texts = [self.main, (workflows / "pr_base_change.yml").read_text(encoding="utf-8")]
        actions = [path.read_text(encoding="utf-8")
                   for path in sorted((workflows.parent / "actions").glob("*/action.y*ml"))]
        commands = [body for text in texts + actions for body in _run_bodies(text)]
        # An action may run a script named by an input, so a caller can pass its own checkout's.
        commands += [value for text in actions for value in _action_input_defaults(text)]
        invoked = {path for command in commands for path in _SCRIPT_PATH.findall(command)}
        # And every script a lane phase runs, which the entry point and its table name.
        invoked |= {path for source in ("tools/ci/lane.py", "tools/ci/lanes.py")
                    for path in _SCRIPT_PATH.findall(_repository_text(source))}
        self.assertEqual(sorted(invoked), sorted(set(WORKFLOW_LOGIC_SCRIPTS)
                                                 | set(TRACKED_CI_SCRIPTS)
                                                 | set(UNTRACKED_CI_SCRIPTS)))
        builds = {"//tools": _repository_text("tools/BUILD.bazel"),
                  "//tools/ci": self.ci_target_definitions}
        for path, target in TRACKED_CI_SCRIPTS.items():
            with self.subTest(path=path):
                package, name = target.split(":")
                self.assertIn('"%s"' % path.rsplit("/", 1)[1], _rule_text(builds[package], name))
        self.assertEqual(_run_bodies("      - run: tools/a.sh\n        run: |\n          # x\n"
                                     "          tools/b.sh\n      - name: c\n"),
                         ["tools/a.sh", "          tools/b.sh"])
        self.assertEqual(_action_input_defaults(
            "name: a\ninputs:\n  script:\n    default: tools/a.sh\nruns:\n  using: composite\n"
            "  steps:\n    - with:\n        default: tools/b.sh\n"), ["tools/a.sh"])
        self.assertEqual(_action_input_defaults("inputs:\n  script:\n    default: tools/a.sh\n"),
                         ["tools/a.sh"])

    def _hosted_editor_runtime_scripts(self):
        """Every repository script the Editor WASM workflow runs, sources or imports."""
        actions = [_repository_text(".github/actions/%s/action.yml" % name) for name in sorted(
            set(re.findall(r"uses: \./\.github/actions/([\w-]+)$", self.editor_wasm,
                           re.MULTILINE)))]
        commands = [body for text in [self.editor_wasm, *actions] for body in _run_bodies(text)]
        commands += [value for text in actions for value in _action_input_defaults(text)]
        pending, found = _script_references("\n".join(commands)), set()
        while pending:
            path = pending.pop()
            found.add(path)
            if path.endswith(".py"):
                pending |= _python_imports(path)
            else:
                pending |= _script_references("\n".join(
                    line for line in _repository_text(path).splitlines()
                    if not line.lstrip().startswith("#")))
            pending -= found
        return found

    def test_every_script_the_hosted_editor_build_runs_selects_full_hosted_coverage(self):
        """bazel-diff cannot attribute these scripts, so the hosted planner keeps the graph."""
        scripts = self._hosted_editor_runtime_scripts()
        # Sourced by the lane script and imported by the evidence collector.
        self.assertTrue({"tools/ci/lane.sh", "tools/ci/verify_unified_tests.py"} <= scripts,
                        sorted(scripts))
        start = self.determine_targets.index("\nhosted_editor_unhashed_input() {\n")
        end = self.determine_targets.index("\n}\n", start) + len("\n}\n")
        script = "#!/usr/bin/env bash\nset -euo pipefail\n"
        script += self.determine_targets[start:end]
        script += self._workflow_logic_function(self._selection_script())
        start = self.determine_targets.index("\n  has_unhashed_input() {\n")
        end = self.determine_targets.index("\n  }\n", start) + len("\n  }\n")
        script += textwrap.dedent(self.determine_targets[start:end])
        # A workflow-logic change selects full coverage before the hosted early return; the
        # other two lists select the full hosted graph before any hashing.
        script += ('for path in "$@"; do\n'
                   '  is_workflow_logic "$path" || has_unhashed_input <<< "$path" ||'
                   ' hosted_editor_unhashed_input <<< "$path" || echo "$path"\n'
                   'done\n')
        unlisted = "tools/ci/unlisted_hosted_step.sh"
        result = self._run_script(script, [*sorted(scripts), unlisted])
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(unlisted + "\n", result.stdout)

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

    def test_browser_comparator_runfiles_copy_is_removable_without_changing_bazel_inputs(self):
        script = _repository_text("tools/run-browser-ci.sh")
        staging = script.split('compare_copy="${work_dir}/browser-bitmap-compare"', 1)[1]
        staging = 'compare_copy="${work_dir}/browser-bitmap-compare"' + staging.split(
            'export DONNER_BROWSER_GOLDEN_COMPARE=', 1)[0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source-comparator"
            source.write_bytes(b"comparator executable fixture")
            source.chmod(0o555)
            assets = root / "bazel-assets"
            (assets / "assets").mkdir(parents=True)
            (assets / "manifest.json").write_bytes(b"manifest fixture")
            (assets / "assets/font.otf").write_bytes(b"font fixture")
            for path in assets.rglob("*"):
                path.chmod(0o555 if path.is_dir() else 0o444)
            assets.chmod(0o555)
            runfiles = Path(str(source) + ".runfiles") / "_main/third_party/public-sans"
            runfiles.mkdir(parents=True)
            (runfiles / "resource_gen.assets").symlink_to(assets, target_is_directory=True)
            work = root / "work"
            work.mkdir()
            try:
                environment = dict(os.environ, work_dir=str(work), compare_rel=str(source))
                result = self._run_script(staging, [], env=environment)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                copied = work / "browser-bitmap-compare.runfiles"
                self.assertEqual((work / "browser-bitmap-compare").read_bytes(), source.read_bytes())
                delivered = copied / "_main/third_party/public-sans/resource_gen.assets"
                self.assertEqual((delivered / "manifest.json").read_bytes(), b"manifest fixture")
                self.assertEqual((delivered / "assets/font.otf").read_bytes(), b"font fixture")
                for path in [copied, *copied.rglob("*")]:
                    self.assertFalse(path.is_symlink())
                    if path.is_dir():
                        self.assertEqual(path.stat().st_mode & 0o200, 0o200, str(path))
                self.assertEqual(assets.stat().st_mode & 0o777, 0o555)
                self.assertEqual((assets / "assets").stat().st_mode & 0o777, 0o555)
                self.assertEqual((assets / "assets/font.otf").stat().st_mode & 0o777, 0o444)
                self.assertEqual((assets / "assets/font.otf").read_bytes(), b"font fixture")
                self.assertEqual(source.stat().st_mode & 0o777, 0o555)
                shutil.rmtree(copied)
                self.assertFalse(copied.exists())
            finally:
                # Restore only this disposable fixture, including after the expected red control.
                for path in root.rglob("*"):
                    if path.is_dir() and not path.is_symlink():
                        path.chmod(path.stat().st_mode | 0o200)

    def test_wasm_build_and_browser_checks_share_one_runner_and_package(self):
        self.assertNotIn("\n  test:\n", self.editor_wasm)
        self.assertNotIn("actions/download-artifact@", self.editor_wasm)
        self.assertIn("PACKAGE_TARGET: ${{ steps.stage-package.outputs.package_target }}", self.editor_wasm)
        self.assertIn('push:\n    branches: ["main"]', self.editor_wasm)
        self.assertIn("uses: ./.github/actions/cache-bazel-actions", self.editor_wasm)
        self.assertNotIn("npm ci", self.editor_wasm)
        self.assertNotIn("actions/setup-node@", self.editor_wasm)
        self.assertIn("//tools/ci:hosted_editor_wasm_package", self.editor_wasm_lane)
        self.assertEqual(1, self.editor_wasm.count("uses: bazel-contrib/setup-bazel@"))
        setup_header = self.editor_wasm.split("uses: bazel-contrib/setup-bazel@", 1)[0]
        self.assertIn("Setup Bazel for native browser comparison", setup_header)
        self.assertLess(
            self.editor_wasm.index("uses: bazel-contrib/setup-bazel@"),
            self.editor_wasm.index(SIZE_CHECK_STEP),
        )

    def test_nightly_consumers_compile_and_test_on_both_hosted_platforms(self):
        self.assertNotIn("cmake_consumer.sh build", self.main)
        linux = self._job_body("linux", self.cmake)
        macos = self._job_body("macos", self.cmake)
        self.assertIn("runs-on: ubuntu-26.04", linux)
        self.assertIn("run: tools/ci/cmake_consumer.sh build --check --ccache", linux)
        self.assertIn("runs-on: xcode-27", macos)
        self.assertIn("run: tools/ci_select_macos_xcode.sh", macos)
        self.assertIn("run: tools/ci/cmake_consumer.sh install-tools", macos)
        self.assertIn("run: tools/ci/cmake_consumer.sh build --ccache", macos)

    def test_geode_asan_pr_lane_restores_the_main_linux_external_cache(self):
        # The lane only runs for pull requests, so it never saves an external repository manifest
        # of its own. It reads the one main's linux job saves, which setup-bazel names after the
        # lowercased workflow name, with spaces and slashes as hyphens, and the job id. Renaming
        # either side would silently restore nothing and leave every run fetching the toolchain.
        workflow_name = re.search(r"^name: (.+)$", self.main, re.MULTILINE).group(1).strip()
        manifest = "%s-linux" % re.sub(r"[ /]", "-", workflow_name.lower())
        linux = self._job_body("linux")
        main_setup = self._step_body(linux, "Setup Bazel")
        self.assertIn("          external-cache: true\n", main_setup)
        self.assertIn("          cache-save: ${{ github.event_name != 'pull_request' && "
                      "github.ref == 'refs/heads/main' }}\n", main_setup)

        lane = self._job_body("asan-geode", self.sanitizers_pr)
        lane_setup = self._step_body(lane, "Setup Bazel")
        self.assertIn("          external-cache: |\n            name: %s\n" % manifest,
                      lane_setup)
        self.assertIn("          cache-save: false\n", lane_setup)
        self.assertNotIn("disk-cache:", lane_setup)

    def test_retired_closure_scanner_is_not_invoked(self):
        self.assertNotIn("configured_rust_closure.py", self.main)
        self.assertNotIn("configured-rust-closure-", self.main)

    def test_platform_checks_reuse_native_build_jobs(self):
        for redundant in ("configured-rust-closure-platform", "no-rust-configured-closure",
                          "linux-local-xvfb", "linker-canary"):
            self.assertNotIn("\n  %s:\n" % redundant, self.main)
        self.assertIn("//tools/ci:linker_canary", self._job_body("linux"))

    def test_native_window_gate_runs_on_arm64_when_remote_routing_is_selected(self):
        hosted = self._job_body("linux")
        remote = self._job_body("linux-self-hosted")
        local = remote
        self.assertIn("use_self_hosted_linux != 'true'", hosted.split("steps:", 1)[0])
        self.assertIn("use_self_hosted_linux == 'true'", local.split("steps:", 1)[0])
        self.assertNotIn("Test local Xvfb targets", hosted)
        self.assertIn("xvfb xauth", hosted)
        self.assertTrue(any(isinstance(a, lanes.LinuxTestSelection) and "local-gpu-isolated" in a.excluded
                            for a in lanes.LANES["linux-self-hosted"].phases["test"].actions))
        # Both displayed-Vulkan targets stay on the local Xvfb route and take the local GPU
        # resource, so neither runs alongside another GPU-opening test.
        for name in ("editor_window_vulkan_surface_tests", "editor_window_vulkan_default_tests"):
            with self.subTest(target=name):
                target = self.editor_tests_build.split(
                    'name = "%s",' % name, 1
                )[1].split("\n)\n", 1)[0]
                for tag in ("no-remote-exec", "no-cache", "external", "requires_xvfb", "resources:local_gpu:1"):
                    self.assertIn('"%s"' % tag, target)
                self.assertNotIn('"exclusive-if-local"', target)
                self.assertNotIn('"local-gpu-isolated"', target)
                self.assertIn('data = [":editor_window_tests_geode"]', target)
        self.assertIn("export DONNER_GPU_BACKEND=vulkan", self.xvfb_runner)
        self.assertIn("xvfb-run -a", self.xvfb_runner)
        # The server must not reset when a case closes its last connection, or the next case's
        # reconnect can be dropped mid-reset.
        self.assertIn("xvfb_server_args='-screen 0 1280x720x24 -noreset'", self.xvfb_runner)
        invocations = [line for line in self.xvfb_runner.splitlines() if "xvfb-run -a" in line]
        self.assertEqual(2, len(invocations))
        for line in invocations:
            self.assertIn('xvfb-run -a -s "$xvfb_server_args" ', line)

        self.assertIn("common:linux --experimental_google_legacy_api", self.bazelrc.splitlines())
        for command in ("build", "query"):
            self.assertIn(command + " --enable_platform_specific_config", self.bazelrc.splitlines())
        self.assertNotIn("Test local Xvfb targets", local)
        test = self._step_body(local, "Test")
        for flag in ("--strategy=TestRunner=remote,local", "--remote_local_fallback=false",
                     '--invocation_policy=', 'experimental_remote_scrubbing_config',
                     '--aspects=//tools/ci:linux_test_venue.bzl%linux_test_venue',
                     '--noexperimental_merged_skyframe_analysis_execution', '--nokeep_going',
                     '--build_tests_only', '--check-results'):
            self.assertIn(flag, test)
        self.assertNotIn("--test_env=PATH", test)
        for target in ("editor_window_vulkan_surface_tests", "editor_window_vulkan_default_tests"):
            self.assertIn('env_inherit = ["PATH"]',
                          _rule_text(_repository_text("donner/editor/tests/BUILD.bazel"), target))
        self.assertIn("for tool in xvfb-run Xvfb xauth",
                      self._step_body(local, "Check native consumer tools"))
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
        for command in self._lane_test_commands("macos"):
            self.assertIn("--test_env=MTL_SHADER_VALIDATION_TEXTURE_USAGE=0", command)
            self.assertNotIn("--test_env=MTL_SHADER_VALIDATION_TEXTURE_USAGE=1", command)
        hosted_test = self._step_body(self._job_body("macos"), "Build and test")
        self.assertIn("-requires_shader_validation", hosted_test)
        mixed = self._mixed_step()
        self.assertIn('if [[ "$RUN_METAL" == true ]]; then', mixed)
        self.assertIn("unified_metal_{full_validation,preflight,offline_compiler,native}", mixed)
        self.assertNotIn("continue-on-error", mixed)
        variants = _repository_text("build_defs/ci_metal.bzl").split("def metal_ci_variants(", 1)[1].split("def metal_ci_suites(", 1)[0]
        self.assertIn("ci_platform_test(", variants)
        self.assertIn("metal_validation = True", variants)
        self.assertIn('target_platform = "//tools/ci:remote_macos_aarch64_gpu"', variants)
        rules = _repository_text("build_defs/rules.bzl")
        self.assertIn('options["//build_defs:metal_shader_validation"] = "on"', rules)
        self.assertIn('"MTL_SHADER_VALIDATION_TEXTURE_USAGE": "1"', rules)
        suites = _repository_text("tools/ci/BUILD.bazel")
        preflight_suite = suites.split('name = "metal_preflight"', 1)[1].split(")", 1)[0]
        self.assertIn("//donner/gpu/metal/tests:metal_shader_memory_validation_tests",
                      preflight_suite)
        metal = _repository_text("donner/gpu/metal/tests/BUILD.bazel")
        for target in ("metal_validation_profile", "metal_shader_memory_validation_tests"):
            self.assertIn('tags = ["requires_shader_validation"]', _rule_text(metal, target),
                          target)

    def test_api_only_metal_validation_is_scoped_to_hosted_and_vm_lanes(self):
        needles = ("hosted-macos-unvalidated", "metal_shader_validation")
        self.assertIn("--config=hosted-macos-unvalidated",
                      self._step_body(self._job_body("macos"), "Build and test"))
        # The hosted config and two explicit VM lane test environments own API-only validation.
        self.assertEqual(self._phases_with("--config=hosted-macos-unvalidated"),
                         {("macos", "test")})
        vm_flags = {
            "macos-self-hosted": "--//build_defs:metal_shader_validation=off "
                "--test_env=MTL_SHADER_VALIDATION_TEXTURE_USAGE=0 "
                "--test_env=DONNER_CI_MACOS_LANE=self-hosted-vm",
            "macos-vm-qualification": "--//build_defs:metal_shader_validation=off "
                "--test_env=MTL_SHADER_VALIDATION_TEXTURE_USAGE=0 "
                "--test_env=DONNER_CI_MACOS_LANE=vm-qualification",
        }
        expanded = {lane: " ".join(bazelrc_configs.expand("test", list(flags), self.configs))
                    for lane, flags in lanes.MACOS_TEST.items()}
        for job_name, flags in vm_flags.items():
            self.assertEqual(expanded[job_name], flags)
        self.assertEqual(sorted(vm_flags), sorted(
            lane for lane, flags in expanded.items() if needles[1] in flags))
        off = "--//build_defs:metal_shader_validation=off"
        self.assertEqual({lane for lane, _ in self._phases_with(off)} - set(vm_flags),
                         {"macos"})
        self.assertEqual({phase for lane, phase in self._phases_with(off) if lane == "macos"},
                         {"test"})
        lane_table = _repository_text("tools/ci/lanes.py")
        self.assertEqual(1, lane_table.count(needles[0]))
        self.assertNotIn(off, lane_table)
        workflow = _repository_text(".github/workflows/main.yml")
        self.assertNotIn(needles[0], workflow)
        self.assertNotIn(needles[1], workflow)
        workflows = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml")).parent
        others = [path for path in sorted(workflows.glob("*.y*ml")) if path.name != "main.yml"]
        actions = sorted((workflows.parent / "actions").glob("*/action.y*ml"))
        self.assertGreater(len(others), 5)
        self.assertGreater(len(actions), 3)
        # Keyed by repository path: every composite action file is named action.yml.
        texts = [(str(path.relative_to(workflows.parent.parent)), path.read_text(encoding="utf-8"))
                 for path in others + actions]
        texts += [(script, _repository_text(script))
                  for script in ("tools/ci_bazel_test.sh", "tools/ci/editor_wasm_lane.sh")]
        self.assertEqual(len(others) + len(actions) + 2, len({name for name, _ in texts}))
        for (name, text), needle in itertools.product(texts, needles):
            self.assertNotIn(needle, text, name)
        self._assert_api_only_validation_rc_scope(needles)


    def _assert_api_only_validation_rc_scope(self, needles):
        # The config's definition is the only .bazelrc line that names it, and only it and the
        # two VM lanes' test environment configs name the setting, so no other config
        # (qualified or not, `--config=x` or `--config x`) can pull either in.
        bazelrc = [line for line in _repository_text(".bazelrc").splitlines()
                   if not line.lstrip().startswith("#")]
        self.assertEqual(
            ["test:hosted-macos-unvalidated --//build_defs:metal_shader_validation=off"],
            [line for line in bazelrc if needles[0] in line])
        self.assertEqual(
            sorted(["test:lane-macos-%s --//build_defs:metal_shader_validation=off" % config
                    for config in ("vm", "vm-qualification")]
                   + ["test:hosted-macos-unvalidated --//build_defs:metal_shader_validation=off"]),
            sorted(line for line in bazelrc if needles[1] in line))
        for name, flags in self.configs.items():
            names = [flag for _, words in flags for flag in words if needles[0] in flag]
            self.assertEqual([], names, name)

    def test_metal_device_tests_default_to_full_shader_validation(self):
        metal = _repository_text("donner/gpu/metal/tests/BUILD.bazel")
        build_defs = _repository_text("build_defs/BUILD.bazel")
        # The setting defaults to full validation, and only `off` selects API-only validation.
        self.assertIn('build_setting_default = "on"',
                      _rule_text(build_defs, "metal_shader_validation"))
        self.assertIn('":metal_shader_validation": "off",',
                      _rule_text(build_defs, "metal_shader_validation_off"))
        self.assertIn('\n_METAL_API_VALIDATION_ENV = select({\n'
                      '    "//build_defs:metal_shader_validation_off": '
                      '_METAL_API_ONLY_VALIDATION_ENV,\n'
                      '    "//conditions:default": _METAL_SHADER_VALIDATION_ENV,\n'
                      '})\n', metal)
        self.assertIn('    env = select({\n'
                      '        "//build_defs:metal_shader_validation_off": '
                      '_METAL_API_ONLY_VALIDATION_ENV,\n'
                      '        "//conditions:default": {\n'
                      '            "MTL_DEBUG_LAYER": "1",\n'
                      '            "MTL_SHADER_VALIDATION": "1",\n'
                      '        },\n'
                      '    }),\n', _rule_text(metal, "metal_device_loss_tests"))
        # The API-only env keeps API validation and is reachable only through those two selects.
        self.assertEqual(3, metal.count("_METAL_API_ONLY_VALIDATION_ENV"))
        self.assertEqual(1, metal.count('"MTL_SHADER_VALIDATION": "0"'))
        api_only = _assignment_block(metal, "_METAL_API_ONLY_VALIDATION_ENV")
        self.assertIn('"MTL_DEBUG_LAYER": "1"', api_only)
        self.assertIn('"MTL_SHADER_VALIDATION": "0"', api_only)
        # The probe and the canary assert shader validation, so they never take the setting.
        self.assertIn('"MTL_SHADER_VALIDATION": "1"',
                      _assignment_block(metal, "_METAL_SHADER_VALIDATION_ENV"))
        self.assertIn("_METAL_VALIDATION_PROBE_ENV = dict(\n    _METAL_SHADER_VALIDATION_ENV,",
                      metal)
        for target, env in (("metal_validation_profile", "_METAL_VALIDATION_PROBE_ENV"),
                            ("metal_full_validation_required", "_METAL_VALIDATION_PROBE_ENV"),
                            ("metal_shader_memory_validation_tests",
                             "_METAL_SHADER_VALIDATION_ENV")):
            self.assertIn("    env = %s,\n" % env, _rule_text(metal, target), target)

    def test_bazel_version_change_selects_full_suite_before_graph_hashing(self):
        """A Bazel upgrade changes the query graph, not just two version files."""
        job = self._selection_script()
        start = job.index("          should_fallback=false\n")
        end = job.index('          work_dir="$(mktemp -d)"', start)
        script = "#!/usr/bin/env bash\nset -euo pipefail\n"
        # The main workflow's planner; the hosted Editor Wasm mode is pinned by
        # //tools/ci:determine_targets_tests.
        script += "selection=standard\n"
        # A full selection still hashes the revision once, so a change that breaks the hashing
        # fails its own pull request; //tools/ci:determine_targets_tests runs the real call.
        script += 'generate_hashes() { echo "self_check=hashed" >> "$GITHUB_OUTPUT"; }\n'
        script += self._workflow_logic_function(job)
        script += textwrap.dedent(job[start:end])
        script += 'echo "incremental=true" >> "$GITHUB_OUTPUT"\n'
        full = "self_check=hashed\nfallback=true\naffected=\n"
        for changed_files, expected in (
            (".bazelversion\nexamples/.bazelversion", full),
            ("MODULE.bazel", full),
            ("donner/base/Utils.h", "incremental=true\n"),
            # A script that holds a workflow's step logic selects what a workflow change does;
            # the rest of tools/ci reaches the build graph and bazel-diff as usual.
            *[(path, full) for path in WORKFLOW_LOGIC_SCRIPTS],
            ("tools/ci/lane.sh", "incremental=true\n"),
            # Rule definitions under build_defs reach every target; its other files are ordinary
            # sources. Only this workflow and its base-change caller define these lanes.
            ("build_defs/rules.bzl", full),
            ("build_defs/check_banned_patterns.py", "incremental=true\n"),
            (".github/workflows/pr_base_change.yml", full),
            # The composite action that pins bazel-diff.
            (".github/actions/prepare-bazel-diff/action.yml", full),
            (".github/workflows/lint.yml", "incremental=true\n"),
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

    def test_pr_test_lanes_exclude_manual_targets_from_ordinary_selection(self):
        for job in ("linux", "linux-self-hosted"):
            actions = lanes.LANES[job].phases["test"].actions
            self.assertTrue(any(isinstance(a, lanes.LinuxTestSelection) and "manual" in a.excluded
                                for a in actions))
        job = "macos"
        self.assertEqual(1, self._job_body(job).count("--test_tag_filters=-manual,-perf"))

    def test_run_time_flag_configs_do_not_select_tests(self):
        # BUILDBUDDY_FLAGS is a step output, so the policy tests cannot expand the config it
        # names where a lane uses it. That config may change where actions run, never which
        # tests run or how.
        self.assertIn("buildbuddy-re", self.configs)
        for kind, flags in self.configs["buildbuddy-re"]:
            self.assertNotIn(kind, ("test", "coverage"))
            for flag in flags:
                self.assertFalse(flag.startswith(("--test_", "--config")), flag)

    def test_pr_test_lanes_exclude_fuzz_soaks(self):
        """PR CI selects the deterministic wrapper probe and excludes exploratory mutation."""
        ci_filter = re.search(r"(?m)^test:ci --test_tag_filters=(\S+)$", self.bazelrc)
        self.assertIsNotNone(ci_filter)
        self.assertIn("-fuzz_soak", ci_filter.group(1).split(","))
        selected_suites = set()
        for job_name in ("linux", "linux-self-hosted", "macos"):
            job = self._job_body(job_name)
            filters = self._job_tag_filters(job)
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
                self._assert_pr_soak_suite_membership(name, suites[name])
            pending.extend(re.findall(r'"(?://tools/ci)?:(\w+)"', suites[name]))

    def _assert_pr_soak_suite_membership(self, name, body):
        if name == "deterministic_fuzzer_soak":
            self.assertEqual(re.findall(r'"(//[^"\n]+)"', body),
                             ["//build_defs:soak_probe_fuzzer_soak"])
        else:
            self.assertNotRegex(body, r'_soak"')

    def test_linux_wgpu_resvg_reference_is_selected_only_for_relevant_prs(self):
        """The manual comparison and dependency audit stay in the explicit Linux suite."""
        job = self._job_body("determine-targets")
        self.assertIn("wgpu_reference: ${{ steps.select_wgpu_resvg.outputs.run }}", job)
        determine = self._selection_script()
        start = determine.index("          # The manual reference is removed")
        # The reference selection ends where the macos-metal selection begins.
        end = determine.index("          # BEGIN macos-metal selection", start)
        self.assertLess(end, determine.index('          manual_affected="', start))
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

        selector = self._step_body(job, "Select Linux WebGPU resvg reference")
        self.assertIn("if: github.event_name == 'pull_request'", selector)
        path_start = determine.index(WORKFLOW_LOGIC_MARKER)
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
            *[(path, "true") for path in WORKFLOW_LOGIC_SCRIPTS],
            ("tools/ci/lane.sh", "false"),
            # Like the full fallback, only build_defs' rule definitions and BUILD file.
            ("build_defs/rules.bzl", "true"),
            ("build_defs/BUILD.bazel", "true"),
            ("build_defs/check_banned_patterns.py", "false"),
        ):
            with self.subTest(path=path), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "outputs"
                result = self._run_script(
                    path_script, [],
                    env={**os.environ, "changed_files": path, "GITHUB_OUTPUT": str(output)},
                )
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertEqual("wgpu_reference_changed=%s\n" % expected, output.read_text())

        self.assertIn("run: tools/ci/determine_targets.sh select-wgpu-reference\n", selector)
        passed = {"WGPU_REFERENCE_AFFECTED": "wgpu_reference_affected",
                  "WGPU_REFERENCE_CHANGED": "wgpu_reference_changed",
                  "WGPU_REFERENCE_UNKNOWN": "wgpu_reference_unknown", "FULL_TEST": "full_test"}
        for variable, output_name in passed.items():
            self.assertIn("          %s: ${{ steps.determine.outputs.%s }}\n"
                          % (variable, output_name), selector)
        script_path = runfiles.Create().Rlocation("donner/tools/ci/determine_targets.sh")
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
                values = dict(zip(passed, (impacted, changed, unknown, full_test)))
                result = self._run_script(
                    'exec /bin/bash "$1" select-wgpu-reference\n', [script_path],
                    env={**os.environ, **values, "GITHUB_OUTPUT": str(output)},
                )
                self.assertEqual(0, result.returncode, result.stderr)
                self.assertEqual("run=%s\n" % expected, output.read_text())

        target = "//tools/ci:linux_wgpu_resvg_reference"
        for job_name in ("linux", "linux-self-hosted"):
            with self.subTest(job=job_name):
                job = self._job_body(job_name)
                step = self._step_body(job, "Test")
                self.assertIn("WGPU_REFERENCE: ${{ needs.determine-targets.outputs.wgpu_reference }}", step)
                self.assertIn("LINUX_REFERENCE_ONLY: ${{ steps.build.outcome != 'success' }}", step)
                self.assertIn("needs.determine-targets.outputs.wgpu_reference == 'true'", step)
                self.assertIn("--test_tag_filters=", step)
                self.assertNotIn("Test Linux WebGPU resvg reference", job)
                self.assertIn("steps.test.outcome == 'failure'", job)
        job_name = "macos"
        self.assertNotIn("Test Linux WebGPU resvg reference", self._job_body(job_name))

    def test_native_metal_job_is_selected_only_for_prs_that_reach_its_suites(self):
        """The macos-metal job runs when the affected set reaches its suites, fails closed."""
        self.assertIn("      metal_affected: ${{ steps.determine.outputs.metal_affected }}\n",
                      self._job_body("determine-targets"))
        determine = self._selection_script()
        start = determine.index("          # BEGIN macos-metal selection (metal_affected).\n")
        end = determine.index("          # END macos-metal selection.\n", start)
        # It reads the selection before manual targets are filtered out.
        self.assertLess(end, determine.index('          manual_affected="'))
        block = "#!/usr/bin/env bash\nset -euo pipefail\n" + textwrap.dedent(determine[start:end])
        suites = ("//tools/ci:metal_full_validation", "//tools/ci:metal_preflight",
                  "//tools/ci:metal_offline_compiler", "//tools/ci:metal_native")
        self.assertIn("tests(%s)" % " + ".join(suites), block)
        for suite in suites:
            self.assertIn('name = "%s"' % suite.split(":", 1)[1], self.ci_target_definitions)
        # The dependency inputs bazel-diff cannot attribute that the WebGPU reference selection
        # lists also select the Metal job; it adds every third-party BUILD overlay.
        reference = determine[determine.index("          # Direct renderer/GPU/image inputs"):start]
        for entry in ("third_party/bazel/non_bcr_deps.bzl", "MODULE.bazel.lock"):
            self.assertIn(entry, reference)
            self.assertIn(entry, block)
        metal_test = "//donner/gpu/metal/tests:metal_buffer_bounds_tests"
        # A stand-in for Bazel: the suite query lists the suites' tests (SUITE_TESTS) and the
        # intersection query prints what Bazel would (INTERSECTION), failing with SUITE_STATUS or
        # INTERSECTION_STATUS. Each query is recorded so the test can read what was asked.
        stub = ("#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$QUERY_LOG\"\n"
                "case \"$*\" in\n"
                "  *intersect*) [ -n \"$INTERSECTION\" ] && printf '%s\\n' \"$INTERSECTION\"\n"
                "               exit \"${INTERSECTION_STATUS:-0}\" ;;\n"
                "  *) [ -n \"$SUITE_TESTS\" ] && printf '%s\\n' \"$SUITE_TESTS\"\n"
                "     exit \"${SUITE_STATUS:-0}\" ;;\n"
                "esac\n")
        unrelated = "//donner/base:base_tests"
        cases = (
            # (affected, changed path, suite tests, intersection, suite status, intersection
            #  status, expected)
            (metal_test, "donner/gpu/metal/MetalDevice.mm", metal_test, metal_test, 0, 0, "true"),
            (unrelated, "donner/base/x.cc", metal_test, "", 0, 0, "false"),
            # bazel-diff may print a label in another canonical form; Bazel resolves both.
            ("@@//donner/gpu/metal/tests:metal_buffer_bounds_tests", "donner/base/x.cc",
             metal_test, metal_test, 0, 0, "true"),
            (unrelated, "tools/ci_macos_xcode.txt", metal_test, "", 0, 0, "true"),
            (unrelated, "tools/ci_metal_toolchain.py", metal_test, "", 0, 0, "true"),
            # Dependency inputs bazel-diff cannot attribute, as the WebGPU reference lists them.
            (unrelated, "third_party/bazel/non_bcr_deps.bzl", metal_test, "", 0, 0, "true"),
            (unrelated, "MODULE.bazel.lock", metal_test, "", 0, 0, "true"),
            (unrelated, "third_party/BUILD.wgpu_native_platform", metal_test, "", 0, 0, "true"),
            (unrelated, "third_party/cmark-gfm.BUILD", metal_test, "", 0, 0, "true"),
            # Fail closed: a failed suite query, suites with no tests, a failed intersection.
            (unrelated, "donner/base/x.cc", "", "", 3, 0, "true"),
            (unrelated, "donner/base/x.cc", "", "", 0, 0, "true"),
            (unrelated, "donner/base/x.cc", metal_test, "", 0, 3, "true"),
            (unrelated, "donner/base/x.cc", metal_test, "", 0, 7, "true"),
        )
        with tempfile.TemporaryDirectory() as directory:
            fake_bazelisk = Path(directory) / "bazelisk"
            fake_bazelisk.write_text(stub)
            fake_bazelisk.chmod(0o755)
            for index, (affected, changed, suite, intersection, suite_status,
                        intersection_status, expected) in enumerate(cases):
                with self.subTest(index=index, affected=affected, changed=changed):
                    output = Path(directory) / ("outputs-%d" % index)
                    log = Path(directory) / ("queries-%d" % index)
                    result = self._run_script(
                        block, [],
                        env={**os.environ, "affected": affected, "changed_files": changed,
                             "head_dir": directory, "GITHUB_OUTPUT": str(output),
                             "QUERY_LOG": str(log), "SUITE_TESTS": suite,
                             "INTERSECTION": intersection, "SUITE_STATUS": str(suite_status),
                             "INTERSECTION_STATUS": str(intersection_status),
                             "PATH": directory + os.pathsep + os.environ.get("PATH", os.defpath)},
                    )
                    self.assertEqual(0, result.returncode, result.stderr)
                    self.assertEqual("metal_affected=%s\n" % expected, output.read_text())
                    # The decision is Bazel's intersection of the quoted affected labels with
                    # the suites, never a comparison of label strings.
                    queries = log.read_text() if log.exists() else ""
                    if changed.startswith("donner/") and suite and not suite_status:
                        self.assertIn('set("%s") intersect tests(%s)'
                                      % (affected, " + ".join(suites)), queries)
        mixed = self._mixed_step()
        self.assertIn("RUN_METAL: ${{ needs.determine-targets.outputs.metal_affected == 'true' || "
                      "needs.determine-targets.outputs.fallback == 'true' }}", mixed)
        self.assertIn('if [[ "$RUN_METAL" == true ]]; then', mixed)

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

        The phases are DISCOVERED from the lane table, not listed: any phase that
        tests the derived target set is covered by this, including one added
        after this was written.
        """
        covered = []
        for lane, phase, actions, tests in _phases_testing_the_selection():
            covered.append((lane, phase))
            with self.subTest(lane=lane, phase=phase):
                for action in tests:
                    self.assertEqual(lanes.TEST_WRAPPER, action.wrappers[:1],
                                     "tests the derived target set without tolerating an "
                                     "empty test selection")
                self.assertIsInstance(actions[0], lanes.RequireSelection,
                                      "tests the derived target set without rejecting an "
                                      "empty target list")
                # Each run of the phase's step sees the selection.
                if lane in lanes.WORKFLOW_LANES:
                    self.assertIn("TARGETS_AFFECTED: ${{ needs.determine-targets.outputs.affected }}",
                                  self._job_body(lane))
        self.assertGreaterEqual(
            len(covered),
            10,
            "phase discovery matched %r, which is fewer phases than exist; the "
            "match is stale and this test is no longer checking anything"
            % (covered,),
        )

    def test_target_lists_avoid_pattern_substitution(self):
        """Target lists reach tens of thousands of characters on large selections.

        `${var//pattern/replacement}` is superlinear in the value's length on the
        system bash of macOS runners (3.2): under a UTF-8 locale its cost grows
        roughly as length^2.8, 18 s on a 27k character list and minutes on the
        largest, paid before Bazel starts. The empty-selection guard uses a regex
        match instead, which takes milliseconds on the same lists.
        """
        workflows = Path(runfiles.Create().Rlocation("donner/.github/workflows/main.yml")).parent
        paths = sorted(workflows.glob("*.y*ml"))
        self.assertIn("main.yml", [path.name for path in paths])
        for path in paths:
            for number, line in enumerate(path.read_text().splitlines(), 1):
                with self.subTest(file=path.name, line=number):
                    self.assertNotRegex(line, r"\$\{\w*(?:TARGETS|targets)\w*//",
                                        "pattern substitution on a target list")

    def test_fixed_target_lanes_do_not_tolerate_an_empty_test_selection(self):
        """A named fixed test suite that yields no tests is a real defect.

        The Geode editor lanes select a fixed suite, so an empty test set
        there means its Bazel membership is broken.
        That must stay red rather than inherit the change-based lanes' notice.
        """
        job_name = "macos"
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

    def test_cmake_cache_qualification_artifacts_are_attempt_scoped(self):
        producer = self._job_body("cmake-cache-producer", self.cmake)
        consumer = self._job_body("cmake-cache-consumer", self.cmake)
        self.assertIn("needs: [gatekeeper, cmake-cache-producer]", consumer)
        for job in (producer, consumer):
            self.assertIn("${{ github.run_id }}-${{ github.run_attempt }}", job)
            self.assertNotIn("overwrite:", job)
        uploads = [self._step_body(job, "Upload cache receipt")
                   for job in (producer, consumer)]
        download = self._step_body(consumer, "Download producer receipt")
        names = [re.search(r"^ +name: (.+)$", step, re.M).group(1)
                 for step in [*uploads, download]]
        seen = set()
        for attempt in (1, 2):
            rendered = [name.replace("${{ github.run_attempt }}", str(attempt))
                        for name in names]
            self.assertEqual(rendered[0], rendered[2])
            self.assertEqual(len(set(rendered[:2])), 2)
            self.assertFalse(seen.intersection(rendered[:2]))
            seen.update(rendered[:2])
            self.assertEqual(rendered[:2], ["cmake-cache-producer-%s" % attempt,
                                            "cmake-cache-consumer-%s" % attempt])
        self.assertNotIn("pattern:", download)

    def test_cmake_schedule_preserves_opt_in_qualification_paths(self):
        trigger = self.cmake.split("\non:\n", 1)[1].split("\n\n", 1)[0]
        self.assertIn("  push:\n    branches: [main]", trigger)
        self.assertIn('    - cron: "10 9 * * *"', trigger)
        self.assertIn("  workflow_dispatch:", trigger)
        paths = re.findall(r"(?m)^      - (.+)$", trigger.split("  pull_request:\n", 1)[1]
                           .split("  workflow_dispatch:", 1)[0])
        self.assertEqual(paths, [
            ".github/workflows/cmake.yml", "CMakePresets.json", '"**/CMakeLists.txt"',
            '"**/*.cmake"', '"**/*.cmake.in"', "examples/cmake_consumer/**", "tools/cmake/**",
            "tools/rust_boundary/rust_scopes.py",
            "tools/ci/cmake_cache.py", "tools/ci/cmake_consumer.sh", "MODULE.bazel",
            "MODULE.bazel.lock", ".bazelrc", ".bazelversion", '"**/BUILD"', '"**/BUILD.bazel"',
            '"**/*.bzl"', "tools/ci_macos_xcode.txt",
            "tools/ci_select_macos_xcode.sh"])

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

    def _assert_editor_wasm_source_checkout(self, workflow):
        build = self._job_body("build", workflow)
        self.assertIn("    steps:\n      - uses: actions/checkout@v7\n        with:\n"
                      "          persist-credentials: false\n\n      - name: Select pinned Xcode", build)
        self.assertEqual(build.count("uses: actions/checkout@"), 1)

    def test_editor_wasm_uses_the_event_commit_for_its_source_checkout(self):
        self._assert_editor_wasm_source_checkout(self.editor_wasm)
        checkout = "      - uses: actions/checkout@v7\n        with:\n"
        for override in ("          ref: another-branch\n", "          repository: other/project\n"):
            changed = self.editor_wasm.replace(checkout, checkout + override)
            with self.assertRaises(AssertionError):
                self._assert_editor_wasm_source_checkout(changed)

    def test_editor_wasm_steps_each_run_one_tested_script_call(self):
        """Each Bazel or staging step is one call of a tested script, in the original order."""
        runs = {name: commands for name, commands in self._editor_wasm_step_runs(self.editor_wasm)}
        names = [name for name, _ in self._editor_wasm_step_runs(self.editor_wasm)]
        for name, command in EDITOR_WASM_SCRIPT_STEPS:
            with self.subTest(step=name):
                self.assertEqual(runs.get(name), [command])
        self.assertEqual([name for name in names if name in dict(EDITOR_WASM_SCRIPT_STEPS)],
                         [name for name, _ in EDITOR_WASM_SCRIPT_STEPS])
        # The workflow delegates Bazel commands to the shared lane script.
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
                "        run: tools/ci/editor_wasm_lane.sh quarantine\n",
                "        run: tools/ci/editor_wasm_lane.sh quarantine\n"
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
        self.assertEqual(1, fetch.count("bazelisk fetch"))
        self.assertNotRegex(fetch, r"bazelisk (?:build|test)")

        size_check = self._step_body(workflow, SIZE_CHECK_STEP.removeprefix("- name: "))
        self.assertNotIn("continue-on-error", size_check)
        build = self._size_check_phase()
        self.assertIn("graph_test ", build)
        self.assertEqual(1, self._lane_function("graph_test").count("bazelisk test"))
        self.assertNotIn("bazelisk fetch", build)
        self.assertRegex(build, r' "\$\{graph_roots\[@\]\}"$')
        # The complete graph includes the budget suite; only a proved browser subset replaces it,
        # as //tools/ci:editor_wasm_lane_tests pins.
        self.assertIn('\ngraph_roots+=("$budget_suite")\n', self.editor_wasm_lane)
        self.assertIn("//tools/ci:hosted_editor_wasm_graph", self.editor_wasm_lane)

    def test_hosted_graph_requires_both_browser_bridge_link_roots(self):
        graph = _rule_text(self.ci_target_definitions, "hosted_editor_wasm_graph")
        self.assertIn("//donner/svg/renderer/wasm:hosted_geode_browser_default_audit_test", graph)
        check = self.renderer_wasm_build.split('actual = "geode_browser_default_audit_test"', 1)[1].split("\n)", 1)[0]
        for target in ("browser_bridge_link_probes", "geode_wasm_browser_module"):
            self.assertIn("//tools/ci:" + target, check)
        for target in ("browser_bridge_link_probe", "browser_bridge_pthreads_link_probe"):
            self.assertIn("//donner/gpu/browser/link_probe:" + target, _rule_text(self.ci_target_definitions, "browser_bridge_link_probes"))
        self.assertIn("--noskip_incompatible_explicit_targets", self._lane_function("graph_test"))

    def _size_check_phase(self):
        """The lane commands the size-check step runs: its one call's `build` function."""
        self.assertIn("        run: tools/ci/editor_wasm_lane.sh build\n", self._step_body(
            self.editor_wasm, "Build and size-check Geode editor Wasm package"))
        return self._lane_function("build")

    def test_editor_wasm_payload_ceilings_are_enforced(self):
        """The size test declares each ceiling once under the explicit CI mode policy."""
        self.assertEqual(
            _declared_payload_ceilings(self.wasm_size_test_arguments),
            ENFORCED_PAYLOAD_CEILINGS,
            "%s and ENFORCED_PAYLOAD_CEILINGS must change together" % PAYLOAD_SIZE_TEST,
        )
        self.assertNotIn("--test_arg", self.editor_wasm)
        self.assertNotIn("--test_arg", self.editor_wasm_lane)
        self.assertIn('case "${PAYLOAD_BUDGET_MODE:-enforce}"', self.editor_wasm_lane)
        self.assertIn("report) budget_suite+=_report", self.editor_wasm_lane)
        self.assertIn("PAYLOAD_BUDGET_MODE must be enforce or report", self.editor_wasm_lane)

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

    def test_numeric_size_enforcement_is_retained_for_pr_nightly_and_manual_runs(self):
        step = self._step_body(self.editor_wasm, "Build and size-check Geode editor Wasm package")
        self.assertIn(
            "PAYLOAD_BUDGET_MODE: ${{ github.event_name == 'push' && "
            "github.ref == 'refs/heads/main' && 'report' || 'enforce' }}", step)
        self.assertIn("  pull_request:", self.editor_wasm)
        self.assertIn("  schedule:\n    - cron: \"18 7 * * *\"", self.editor_wasm)
        self.assertIn("  workflow_dispatch:", self.editor_wasm)

    def test_renderer_wasm_payload_ceilings_are_enforced(self):
        """The renderer module declares each ceiling enforced by PR/nightly/manual runs."""
        self.assertEqual(
            _declared_payload_ceilings(
                self._renderer_size_test_definition(), RENDERER_PAYLOAD_CEILINGS
            ),
            RENDERER_PAYLOAD_CEILINGS,
            "%s and RENDERER_PAYLOAD_CEILINGS must change together" % RENDERER_SIZE_TEST,
        )

    def test_renderer_wasm_size_test_runs_in_the_size_check_step(self):
        """The step runs renderer size and pixel targets separately; prefetch retains their suite."""
        self.assertIn('budget_suite=//tools/ci:hosted_wasm_budgets', self.editor_wasm_lane)
        self.assertIn('"//donner/svg/renderer/wasm:hosted_wasm_size_tests" + suffix', self.ci_target_definitions)
        self.assertIn('"//donner/editor/wasm:hosted_wasm_geode_package_size_tests" + suffix', self.ci_target_definitions)
        self.assertIn('for suffix in ["", "_report"]', self.ci_target_definitions)

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
