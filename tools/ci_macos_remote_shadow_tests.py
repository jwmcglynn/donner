"""Pins the advisory macOS remote-execution shadow lane (tools/ci/macos_remote_shadow.py).

The lane builds and tests a trusted pull request's selection for macOS from a self-hosted Linux
runner, every action on a remote macOS executor. These tests keep it reachable only through the
self-hosted Linux lane's trust gate plus an opt-in, keep it from ever failing a pull request or
the coverage contract, keep private runner configuration out of the repository, and keep what it
runs a documented subset of the self-hosted macOS lane.

Each workflow rule is a function that returns the problems it finds, so the tests show it passes
on the real workflow and fails on a workflow that breaks it.
"""

import contextlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest

from python.runfiles import runfiles
from tools import bazelrc_configs
from tools.ci import macos_remote_shadow as lane
from tools.test_executable_fixtures import ExecutableFixtureDirectory

MAIN = ".github/workflows/main.yml"
JOB = "macos-remote-shadow"
TURNSTILE = JOB + "-turnstile"
LINUX_JOB = "linux-self-hosted"
LINUX_TURNSTILE = LINUX_JOB + "-turnstile"
MACOS_JOB = "macos-self-hosted"
LABEL = "ci:macos-remote-shadow"
VARIABLE = "MACOS_REMOTE_SHADOW_LANE"
# What the shadow lane adds to the self-hosted Linux lane's trust gate. It only narrows the gate.
OPT_IN = (" && github.event_name == 'pull_request' && (contains("
          "github.event.pull_request.labels.*.name, '%s') || vars.%s == 'true')"
          % (LABEL, VARIABLE))
# Before the trust gate: the lane runs after its run's Linux lane, whether that passed or failed.
AFTER_LINUX = ("!cancelled() && (needs.linux-self-hosted.result == 'success' || "
               "needs.linux-self-hosted.result == 'failure') && ")
# After the opt-in, on the lane job only: its own admission let it run.
ADMITTED = " && needs.%s.outputs.admitted == 'true'" % (JOB + "-turnstile")
RUN = ('python3 tools/ci/macos_remote_shadow.py --out "$RUNNER_TEMP/macos-remote-shadow" '
       '--output-user-root "$RUNNER_TOOL_CACHE/donner-macos-remote-shadow-output"')
# Private runner configuration that must come only from the runner's own rc files: endpoints,
# credentials, priority and per-host concurrency (as in //tools:remote_macos_config_tests).
PRIVATE_FLAG = re.compile(
    r"--(?:no)?(?:remote_executor|remote_cache|remote_instance_name|remote_header|"
    r"remote_cache_header|remote_exec_header|remote_downloader_header|remote_execution_priority|"
    r"remote_proxy|tls_|credential_helper|google_|bes_|local_resources)")
ENDPOINT = re.compile(r"\b(?:grpcs?|https?|unix)://")
# The reviewed test-artifact collector every test lane uploads its failed tests' outputs with.
COLLECTOR = "./.github/actions/upload-bazel-test-artifacts"
# The lane step continues on error, so failure() is never true after it; its outcome is.
FAILURE_UPLOAD_IF = "${{ !cancelled() && steps.shadow.outcome == 'failure' }}"
# Flags of `--config=ci` the lane leaves out, with why.
CI_OMISSIONS = {
    "--repository_cache=~/.cache/bazel-repo": "A runner path; the runner's rc can set its own.",
}


def _read(path):
    return Path(runfiles.Create().Rlocation("donner/%s" % path)).read_text(encoding="utf-8")


def _root():
    return Path(runfiles.Create().Rlocation("donner/.bazelrc")).parent


def _job(text, name):
    """A job's body in main.yml, without its header line."""
    match = re.search(r"(?m)^  %s:\n((?:(?:    .*|)\n)*)" % re.escape(name), text)
    return match.group(1) if match else None


def _key(body, key, indent=4):
    match = re.search(r"(?m)^ {%d}%s: (.*)$" % (indent, re.escape(key)), body or "")
    return match.group(1).strip() if match else None


def _steps(body):
    """Each step of a job body, from its `- ` line to the next."""
    return re.split(r"(?m)^(?=      - )", (body or "").split("    steps:\n", 1)[-1])[1:]


def _step(body, step_id):
    return next((step for step in _steps(body) if "        id: %s\n" % step_id in step), None)


def _needs(body):
    return [need.strip() for need in (_key(body, "needs") or "").strip("[]").split(",")]


def _with(step, key):
    match = re.search(r"(?m)^          %s: (.*)$" % re.escape(key), step or "")
    return match.group(1).strip().strip('"') if match else None


def _gate_problems(gate, job, turnstile):
    """Both shadow jobs' conditions are the Linux lane's gate, narrowed, and nothing else."""
    if not (gate.startswith("${{ ") and gate.endswith(" }}")):
        return ["the Linux lane's gate is not one expression"]
    inner = gate[len("${{ "):-len(" }}")]
    return ["%s is not the Linux lane's trust gate plus the opt-in" % name
            for name, body, extra in ((JOB, job, ADMITTED), (TURNSTILE, turnstile, ""))
            if _key(body, "if") != "${{ %s%s%s%s }}" % (AFTER_LINUX, inner, OPT_IN, extra)]


def trust_problems(text):
    """The shadow jobs run only where the self-hosted Linux lane may run, and only on opt-in."""
    linux, job, turnstile = (_job(text, name) for name in (LINUX_JOB, JOB, TURNSTILE))
    if not (linux and job and turnstile):
        return ["the shadow lane, its turnstile or the self-hosted Linux lane is missing"]
    problems = _gate_problems(_key(linux, "if") or "", job, turnstile)
    if _key(job, "runs-on") != _key(linux, "runs-on"):
        problems.append("%s does not run on the Linux lane's runner class" % JOB)
    if _key(turnstile, "runs-on") != "ubuntu-24.04":
        problems.append("%s must wait on a hosted runner" % TURNSTILE)
    if "gatekeeper" not in _needs(job) or "gatekeeper" not in _needs(turnstile):
        problems.append("the shadow jobs do not need the gatekeeper")
    if "secrets." in job or "environment:" in job:
        problems.append("%s reads a secret or binds an environment" % JOB)
    return problems


def _lane_step_problems(job):
    """The lane step runs the script alone, on the planner's selection, and cannot fail the job."""
    shadow = _step(job, "shadow")
    if shadow is None or _key(shadow, "continue-on-error", 8) != "true":
        return ["the shadow step can fail the job"]
    selection = (_key(shadow, "FALLBACK", 10), _key(shadow, "AFFECTED", 10))
    if _key(shadow, "run", 8) != RUN or selection != (
            "${{ needs.determine-targets.outputs.fallback }}",
            "${{ needs.determine-targets.outputs.affected }}"):
        return ["the shadow step must run the lane script alone on the planner's selection"]
    others = [step for step in _steps(job) if step != shadow]
    if any(re.search(r"(?m)^        run:", step) for step in others):
        return ["%s runs a script outside its lane step" % JOB]
    return []


def _contract_problems(text, manifest, job):
    """The coverage contract neither waits for nor reads the lane."""
    problems = []
    if any("coverage-receipt" in step or "ci_coverage_contract" in step for step in _steps(job)):
        problems.append("%s records a coverage receipt" % JOB)
    contract = _job(text, "coverage-contract") or ""
    if JOB in _needs(contract) or TURNSTILE in _needs(contract):
        problems.append("the coverage contract waits for the shadow lane")
    if JOB in json.dumps(manifest):
        problems.append("the coverage manifest names the shadow lane")
    return problems


def non_gating_problems(text, manifest):
    """A shadow failure can fail neither the run, nor the job's check, nor the coverage contract."""
    job = _job(text, JOB)
    if job is None:
        return ["%s is missing" % JOB]
    problems = []
    if _key(job, "continue-on-error") != "true":
        problems.append("%s lacks job-level continue-on-error" % JOB)
    # It holds a self-hosted Linux runner outside the Linux lane's accounting, so it may not take
    # longer than it needs.
    budget = int(_key(job, "timeout-minutes") or 0)
    if not lane.STARVED_SECONDS // 60 < budget <= 90 or \
            budget >= int(_key(_job(text, LINUX_JOB), "timeout-minutes") or 0):
        problems.append("%s needs a budget above the starvation bound and at most 90 minutes"
                        % JOB)
    return problems + _lane_step_problems(job) + _contract_problems(text, manifest, job)


def admission_problems(text):
    """One shadow run at a time, in a pool of its own, after this run's Linux lane is admitted."""
    job, turnstile, linux_turnstile = (_job(text, name) for name in (JOB, TURNSTILE,
                                                                       LINUX_TURNSTILE))
    if not (job and turnstile and linux_turnstile):
        return ["a turnstile or the shadow lane is missing"]
    wait = next((step for step in _steps(turnstile) if "re-turnstile" in step), None)
    linux_wait = next((step for step in _steps(linux_turnstile) if "re-turnstile" in step), None)
    problems = []
    if TURNSTILE not in _needs(job):
        problems.append("%s does not wait for its turnstile" % JOB)
    if LINUX_JOB not in _needs(job) or LINUX_JOB not in _needs(turnstile):
        problems.append("the shadow lane can start before this run's Linux lane has finished")
    if (_with(wait, "lane"), _with(wait, "pool"), _with(wait, "capacity")) != (JOB, JOB, "1"):
        problems.append("the shadow lane needs its own pool of capacity 1")
    if _with(wait, "pool-workflows") != _with(linux_wait, "pool-workflows"):
        problems.append("the shadow pool must count the same workflows as the Linux pool")
    return problems + _wait_problems(wait, linux_wait, turnstile)


def _wait_problems(wait, linux_wait, turnstile):
    """The Linux pool is untouched; the shadow wait is short, fail-closed and advisory."""
    problems = []
    if (_with(linux_wait, "pool") or "").split() != [LINUX_JOB]:
        problems.append("the Linux lane's admission pool changed")
    if _with(wait, "timeout-result") != "skip" or int(_with(wait, "timeout-minutes") or 999) > 30:
        problems.append("the shadow admission must fail closed within 30 minutes")
    if _with(linux_wait, "timeout-result") is not None:
        problems.append("the Linux lane's admission behavior changed")
    if _key(turnstile, "continue-on-error") != "true":
        problems.append("a failed shadow admission can fail the run")
    return problems


def privacy_problems(texts):
    """No endpoint, credential, priority or private flag in the lane's workflow jobs or script."""
    return ["%s names private runner configuration" % name for name, text in texts.items()
            if PRIVATE_FLAG.search(text) or ENDPOINT.search(text)]


def upload_problems(text):
    """The lane uploads derived files only, never a build event file or an execution log."""
    job = _job(text, JOB) or ""
    block = job.split("          path: |\n", 1)[1] if "          path: |\n" in job else ""
    lines = re.match(r"(?:            .*\n)*", block).group(0)
    paths = [line.strip() for line in lines.splitlines()]
    if not paths:
        return ["%s uploads no summary" % JOB]
    # A stall note can quote a Bazel message; it stays on the runner.
    allowed = ("report.md", "shadow-summary.json", "profile.gz")
    return ["%s uploads %s" % (JOB, path) for path in paths
            if path.rsplit("/", 1)[-1] not in allowed]


def _staged_root():
    """The workflow's spelling of the directory the script stages failed tests' outputs in."""
    out = RUN.split('--out "', 1)[1].split('"', 1)[0]
    return out.replace("$RUNNER_TEMP", "${{ runner.temp }}") + "/" + lane.ARTIFACTS


def failure_artifact_problems(text):
    """A failed test's log and undeclared outputs leave the runner, through the reviewed
    collector and from the directory the script stages them in, whenever the lane step failed."""
    steps = _steps(_job(text, JOB))
    uploads = [step for step in steps if "        uses: %s\n" % COLLECTOR in step]
    if len(uploads) != 1:
        return ["%s does not upload its failed tests' outputs through the collector" % JOB]
    upload, shadow = uploads[0], _step(_job(text, JOB), "shadow")
    problems = []
    if shadow is None or steps.index(upload) < steps.index(shadow):
        problems.append("the failed test outputs are uploaded before the lane step runs")
    if _key(upload, "if", 8) != FAILURE_UPLOAD_IF:
        problems.append("the failed test outputs are not uploaded on the lane step's outcome")
    if _with(upload, "testlogs") != _staged_root():
        problems.append("the collector does not read the script's staged failed test outputs")
    return problems


def _flags(command):
    return [word for word in command if word.startswith("--")]


def _preflights(out):
    return [command for command, _ in lane.preflight_commands("bazelisk", [], [], out).values()]


def _routing(out):
    """The routing probe's Bazel command, from the Bazel executable on."""
    return _bazel_part(lane.routing_command("bazelisk", [], [], out)[0])


def _bazel_part(command):
    """A phase command from the Bazel executable on."""
    return command[command.index("bazelisk"):]


def _tag_filter_sets(value):
    tags = value.split(",")
    return ({tag for tag in tags if not tag.startswith("-")},
            {tag[1:] for tag in tags if tag.startswith("-")})


class WorkflowTest(unittest.TestCase):
    maxDiff = None

    @classmethod
    def setUpClass(cls):
        cls.text = _read(MAIN)
        cls.manifest = json.loads(_read("tools/ci/coverage_contract.json"))

    def _mutated(self, old, new, job=JOB):
        body = _job(self.text, job)
        self.assertIn(old, body)
        return self.text.replace(body, body.replace(old, new, 1), 1)

    def test_shadow_lane_is_the_linux_trust_gate_plus_an_opt_in(self):
        self.assertEqual([], trust_problems(self.text))
        linux_gate = _key(_job(self.text, LINUX_JOB), "if")
        self.assertIn("needs.gatekeeper.outputs.use_self_hosted_linux == 'true'", linux_gate)
        for old, new in (
                # Opt-in dropped, so every trusted pull request runs it.
                (OPT_IN, ""),
                # The label alone, or the variable alone, admitting an untrusted pull request.
                ("needs.gatekeeper.outputs.use_self_hosted_linux == 'true' && ", ""),
                (OPT_IN, OPT_IN.replace(" && github.event_name", " || github.event_name", 1)),
                (LABEL, "ci:other"),
                ("runs-on: [self-hosted, linux, arm64]", "runs-on: [self-hosted, macOS, ARM64]"),
                ("    continue-on-error: true\n",
                 "    continue-on-error: true\n    environment: buildbuddy-write\n"),
                # Running while the Linux lane is still going, or without its own admission.
                (AFTER_LINUX, "!cancelled() && "),
                (ADMITTED, "")):
            with self.subTest(old=old, new=new):
                self.assertTrue(trust_problems(self._mutated(old, new)))
        self.assertTrue(trust_problems(self._mutated(OPT_IN, "", TURNSTILE)))
        self.assertTrue(trust_problems(self._mutated(AFTER_LINUX, "", TURNSTILE)))

    def test_shadow_lane_can_never_fail_a_pull_request(self):
        self.assertEqual([], non_gating_problems(self.text, self.manifest))
        job = _job(self.text, JOB)
        upload = next(step for step in _steps(job) if "upload-artifact" in step)
        receipt = ("      - name: Record coverage receipt\n        env:\n"
                   "          STEPS_JSON: ${{ toJSON(steps) }}\n"
                   "        run: python3 tools/ci_coverage_contract.py receipt %s\n\n" % JOB)
        for old, new in (("    continue-on-error: true\n", ""),
                         ("        continue-on-error: true\n", ""),
                         (RUN, RUN + " || true"),
                         ("outputs.affected", "outputs.fallback"),
                         ("timeout-minutes: 90", "timeout-minutes: 20"),
                         ("timeout-minutes: 90", "timeout-minutes: 210"),
                         (upload, receipt + upload)):
            with self.subTest(old=old[:40], new=new[:40]):
                self.assertTrue(non_gating_problems(self._mutated(old, new), self.manifest))
        contract = _key(_job(self.text, "coverage-contract"), "needs")
        self.assertTrue(non_gating_problems(self._mutated(
            contract, contract.replace("]", ", %s]" % JOB), "coverage-contract"), self.manifest))
        named = json.loads(json.dumps(self.manifest).replace(
            '"linux-self-hosted"', '"%s"' % JOB, 1))
        self.assertTrue(non_gating_problems(self.text, named))

    def test_shadow_lane_runs_after_its_linux_lane_with_a_pool_of_one(self):
        self.assertEqual([], admission_problems(self.text))
        linux_pool = "          pool: linux-self-hosted\n"
        for old, new, job in (('capacity: "1"', 'capacity: "2"', TURNSTILE),
                              ("pool: macos-remote-shadow", "pool: linux-self-hosted", TURNSTILE),
                              ("pool-workflows: main.yml pr_base_change.yml",
                               "pool-workflows: main.yml", TURNSTILE),
                              (", linux-self-hosted]", "]", TURNSTILE),
                              (", linux-self-hosted, macos-remote-shadow-turnstile]",
                               ", macos-remote-shadow-turnstile]", JOB),
                              (", macos-remote-shadow-turnstile]", "]", JOB),
                              (linux_pool, linux_pool.replace("\n", " macos-remote-shadow\n"),
                               LINUX_TURNSTILE),
                              ("          timeout-result: skip\n", "", TURNSTILE),
                              ('timeout-minutes: "30"', 'timeout-minutes: "150"', TURNSTILE),
                              ("    continue-on-error: true\n", "", TURNSTILE),
                              (linux_pool, linux_pool + "          timeout-result: skip\n",
                               LINUX_TURNSTILE)):
            with self.subTest(old=old, job=job):
                self.assertTrue(admission_problems(self._mutated(old, new, job)))

    def test_no_private_runner_configuration_in_the_lane(self):
        texts = {JOB: _job(self.text, JOB), TURNSTILE: _job(self.text, TURNSTILE),
                 "tools/ci/macos_remote_shadow.py": _read("tools/ci/macos_remote_shadow.py")}
        self.assertEqual([], privacy_problems(texts))
        for leak in ("--remote_executor=grpcs://executor.invalid:1", "--tls_client_key=k.pem",
                     "--remote_execution_priority=0", "grpcs://cache.invalid"):
            with self.subTest(leak=leak):
                self.assertTrue(privacy_problems(dict(texts, **{JOB: texts[JOB] + leak})))
        # Bazel never appears in the workflow: the script owns every flag.
        self.assertNotRegex(texts[JOB].replace(COLLECTOR, ""), r"\bbazel(?:isk)?\b|--config")

    def test_lane_uploads_derived_files_only(self):
        self.assertEqual([], upload_problems(self.text))
        for leak in ("native/bep.json", "native/exec.log.zst", "native/console.log"):
            with self.subTest(leak=leak):
                self.assertTrue(upload_problems(self._mutated(
                    "macos-remote-shadow/report.md\n",
                    "macos-remote-shadow/report.md\n            ${{ runner.temp }}/"
                    "macos-remote-shadow/%s\n" % leak)))

    def test_failed_test_outputs_are_uploaded_on_the_lane_steps_outcome(self):
        self.assertEqual([], failure_artifact_problems(self.text))
        upload = next(step for step in _steps(_job(self.text, JOB)) if COLLECTOR in step)
        for old, new in (
                # The lane step continues on error, so failure() would never upload anything.
                (FAILURE_UPLOAD_IF, "failure() && steps.shadow.outcome == 'failure'"),
                (FAILURE_UPLOAD_IF, "${{ !cancelled() && steps.shadow.conclusion == 'failure' }}"),
                # The workspace's bazel-testlogs is the last command's configuration and keeps
                # results of earlier runs in the lane's persistent output root.
                ("          testlogs: %s\n" % _staged_root(), ""),
                (_staged_root(), "${{ runner.temp }}/macos-remote-shadow"),
                (upload, "")):
            with self.subTest(old=old[:40], new=new[:40]):
                self.assertTrue(failure_artifact_problems(self._mutated(old, new)))

    def test_identity_comes_from_the_lane_script_not_the_workflow(self):
        # A workflow never names the toolchain identity (//tools:macos_cache_key_parity_tests).
        # The script exports the config's pinned Xcode to every Bazel command instead, and
        # rejects a caller's value that contradicts it.
        self.assertNotIn(lane.IDENTITY, _job(self.text, JOB))
        environ = {"FALLBACK": "true", lane.IDENTITY: "27.1-27A9275"}
        self.assertEqual(2, lane.main(["--dry-run", "--out", "/out"], environ))

    def test_watchdog_is_the_linux_lane_watchdog(self):
        # The shadow lane runs its Bazel commands under the self-hosted Linux lane's own stall
        # watchdog, which //tools:ci_runtime_workflow_tests exercises.
        self.assertEqual("tools/ci/bazel_watchdog.sh", lane.WATCHDOG)
        self.assertIn(lane.WATCHDOG + " ", _job(self.text, LINUX_JOB))

    def test_docs_describe_the_opt_in_and_the_runner_contract(self):
        docs = _read("docs/building.md")
        section = " ".join(docs.split("### Remote macOS shadow lane", 1)[1].split("\n## ", 1)[0]
                           .split())
        for needle in (LABEL, VARIABLE, "build:remote-macos-base", "--remote_executor",
                       "--jobs", "never fails", "remote-macos-gpu", "--jobs=4",
                       "not configured for the remote macOS lane",
                       "outside the Linux lane's admission accounting",
                       "dedicated runner capacity", "a rerun does not read labels",
                       "RUNNER_TOOL_CACHE", "within 10 minutes", "should prune",
                       "--max_idle_secs=600",
                       "checks stay pending", "fails closed", "the workflow run still succeeds",
                       "--remote_print_execution_messages=success",
                       lane.ROUTING_PROBE, lane.STARVED, "--config=ci-test-lane",
                       "20 minutes", "90-minute") + lane.EXCLUDED_TAGS + \
                lane.GPU_TAGS:
            with self.subTest(needle=needle):
                self.assertIn(needle, section)


class LaneCommandTest(unittest.TestCase):
    maxDiff = None
    OUT = "/runner/temp/macos-remote-shadow"
    TARGETS = ["//donner/base:base_tests", "//donner/svg/renderer/geode:geode_device_tests"]

    @classmethod
    def setUpClass(cls):
        cls.configs = bazelrc_configs.parse(_read(".bazelrc"))
        cls.commands = {phase: lane.phase_command(phase, "bazelisk", [], [], cls.OUT, cls.TARGETS)
                        for phase in lane.PHASES}

    def _expanded(self, phase):
        command = _bazel_part(self.commands[phase])
        return bazelrc_configs.expand("test", command[2:], self.configs)

    def test_phases_select_their_configs_and_cap_the_gpu_phase(self):
        native, gpu = (_bazel_part(self.commands[phase]) for phase in lane.PHASES)
        self.assertEqual(["bazelisk", "test", "--config=remote-macos"], native[:3])
        self.assertEqual(["bazelisk", "test", "--config=remote-macos-gpu"], gpu[:3])
        for command, config in ((native, "--config=remote-macos"),
                                (gpu, "--config=remote-macos-gpu")):
            self.assertEqual([config, "--config=ci-test-lane"],
                             [word for word in command if word.startswith("--config")])
        self.assertNotIn("--build_tests_only", native)
        self.assertIn("--build_tests_only", gpu)
        # The cap is the last --jobs of the expanded command, so neither the config nor anything
        # bound to it in a runner's rc can raise it; the native phase sets none.
        self.assertEqual("4", bazelrc_configs.last_value(self._expanded("gpu"), "jobs"))
        self.assertIsNone(bazelrc_configs.last_value(self._expanded("native"), "jobs"))
        rc_options = ["--jobs=64"]
        capped = lane.phase_command("gpu", "bazelisk", [], rc_options, self.OUT, self.TARGETS)
        self.assertLess(capped.index("--jobs=64"), capped.index("--jobs=4"))
        self.assertEqual(lane.GPU_JOBS, 4)
        self.assertEqual(self.TARGETS, native[-2:])
        self.assertEqual(self.TARGETS, gpu[-2:])

    def test_phases_run_under_the_empty_selection_wrapper_and_the_watchdog(self):
        for phase in lane.PHASES:
            with self.subTest(phase=phase):
                command = self.commands[phase]
                self.assertEqual([lane.TEST_WRAPPER, lane.WATCHDOG,
                                  "%s/%s/console.log" % (self.OUT, phase), "bazelisk"],
                                 command[:4])

    def test_lane_never_passes_the_ci_config_but_keeps_what_it_needs(self):
        ci = bazelrc_configs.expand("test", ["--config=ci"], self.configs)
        self.assertTrue(ci)
        lane_probe, plain_probe = _preflights(self.OUT)
        self.assertIn("--config=remote-macos", lane_probe)
        self.assertEqual([], [word for word in plain_probe if word.startswith("--config")])
        for name, command in [("preflight", lane_probe), ("preflight", plain_probe),
                              ("preflight", _routing(self.OUT))] + [
                (phase, _bazel_part(self.commands[phase])) for phase in lane.PHASES]:
            with self.subTest(command=name):
                words = " ".join(command)
                self.assertNotRegex(words, r"--config[= ]ci(?:\s|$)")
                expanded = bazelrc_configs.expand(command[1], command[2:], self.configs)
                if name == "preflight":
                    # It analyzes nothing, so it needs only the build event and target flags.
                    self.assertEqual([], [flag for flag in lane.CI_FLAGS if flag not in expanded])
                    continue
                # The lane states its own tag filters.
                missing = [flag for flag in ci if flag not in expanded and
                           not flag.startswith("--test_tag_filters=")]
                self.assertEqual(sorted(CI_OMISSIONS), sorted(missing))

    def test_lane_leaves_every_runner_setting_to_the_runners_rc(self):
        # The runner's rc binds these to remote-macos-base; a command-line value would override
        # it, and the `ci`, `re` and other lane configs carry another lane's executor settings.
        forbidden = re.compile(
            r"^--(?:config=(?:ci|re|buildbuddy-re|macos-vm|hosted-macos[a-z-]*)$|remote_executor|"
            r"remote_cache|tls_|remote_default_exec_properties|experimental_remote_downloader|"
            r"remote_execution_priority|remote_timeout|(?:no)?enable_platform_specific_config|"
            r"strategy|spawn_strategy|(?:no)?remote_local_fallback|"
            r"incompatible_remote_local_fallback|announce_rc)")
        lane_probe, plain_probe = _preflights(self.OUT)
        commands = {"preflight": lane_probe, "plain preflight": plain_probe,
                    "routing probe": _routing(self.OUT)}
        commands.update((phase, _bazel_part(self.commands[phase])) for phase in lane.PHASES)
        for name, command in commands.items():
            with self.subTest(command=name):
                self.assertEqual([], [flag for flag in command if forbidden.match(flag)])
                self.assertIn(command[1], ("build", "test"))
        self.assertEqual([], [flag for flag in commands["native"] if flag.startswith("--jobs")])
        self.assertNotRegex(_read("tools/ci/macos_remote_shadow.py"),
                            r"[\"'](?:query|coverage|info)[\"']")

    def test_remote_execution_messages_are_printed_only_for_successes(self):
        # Messages a backend attaches to failed remote actions can name it; the log is public.
        commands = dict((phase, self.commands[phase]) for phase in lane.PHASES)
        commands.update(lane_probe=_preflights(self.OUT)[0], plain_probe=_preflights(self.OUT)[1],
                        routing=_routing(self.OUT))
        for name, command in commands.items():
            values = [flag for flag in command
                      if flag.startswith("--remote_print_execution_messages")]
            self.assertEqual(["--remote_print_execution_messages=success"], values, name)

    def test_every_bazel_command_uses_the_lanes_own_output_root_and_idle_limit(self):
        environ = {"FALLBACK": "true"}
        lines = _capture_main(["--dry-run", "--out", self.OUT, "--output-user-root", "/root/x"],
                              environ)[1:]
        self.assertEqual(6, len(lines))  # Two probes, the routing probe, two phases, shutdown.
        for line in lines:
            words = shlex.split(line)
            with self.subTest(line=line[:60]):
                bazel = words.index("bazelisk")
                self.assertEqual(["--output_user_root=/root/x", "--max_idle_secs=600"],
                                 words[bazel + 1:bazel + 3])
        self.assertTrue(lines[-1].endswith(
            "bazelisk --output_user_root=/root/x --max_idle_secs=600 shutdown"))
        # The workflow keeps it out of the workspace, in the runner's persistent tool cache.
        self.assertIn('--output-user-root "$RUNNER_TOOL_CACHE/donner-macos-remote-shadow-output"',
                      RUN)

    def test_routing_probe_is_a_manual_uncached_test(self):
        probe = re.search(r'(?ms)^sh_test\(\n    name = "remote_macos_probe",\n(.*?)^\)$',
                          _read("tools/ci/BUILD.bazel"))
        self.assertIsNotNone(probe)
        tags = re.findall(r'"([^"]+)"', probe.group(1).split("tags = [", 1)[1].split("]", 1)[0])
        self.assertEqual(["external", "manual"], sorted(tags))
        routing = _routing(self.OUT)
        self.assertEqual("test", routing[1])
        self.assertIn("--nocache_test_results", routing)
        self.assertEqual(lane.ROUTING_PROBE, routing[-1])

    def test_editor_build_identity_comes_from_the_test_lane_config(self):
        # The narrow config `ci` composes for every non-release lane; never `ci` itself.
        for phase in lane.PHASES:
            with self.subTest(phase=phase):
                self.assertIn("--config=ci-test-lane", self.commands[phase])
                expanded = self._expanded(phase)
                self.assertEqual(["0" * 40], [flag.split("=", 1)[1] for flag in expanded
                                              if flag.startswith(
                                                  "--//build_defs:release_source_commit=")])
        rule = _read("build_defs/editor_build_info.bzl")
        self.assertIn('execution_requirements = {} if commit else {"local": "1"}', rule)

    def test_tag_filters_split_gpu_tests_from_everything_else(self):
        native_positive, native_negative = _tag_filter_sets(lane.tag_filters("native"))
        gpu_positive, gpu_negative = _tag_filter_sets(lane.tag_filters("gpu"))
        self.assertEqual(set(), native_positive)
        self.assertEqual(set(lane.GPU_TAGS), gpu_positive)
        self.assertEqual(set(lane.EXCLUDED_TAGS) | set(lane.GPU_TAGS), native_negative)
        self.assertEqual(set(lane.EXCLUDED_TAGS), gpu_negative)
        for phase in lane.PHASES:
            values = [flag.split("=", 1)[1] for flag in self.commands[phase]
                      if flag.startswith("--test_tag_filters=")]
            self.assertEqual([lane.tag_filters(phase)], values)
        for tag in ("launches-browser", "requires_desktop_session", "local", "no-remote",
                    "manual", "perf", "fuzz_soak"):
            self.assertIn(tag, lane.EXCLUDED_TAGS)

    def test_lane_runs_a_subset_of_the_self_hosted_macos_lane(self):
        test = _read(MAIN).split("  %s:\n" % MACOS_JOB, 1)[1].split("      - name: Test\n", 1)[1]
        filters = re.search(r"--test_tag_filters=(\S+)", test.split("\n      - ", 1)[0]).group(1)
        positive, negative = _tag_filter_sets(filters)
        self.assertEqual(set(), positive)
        self.assertEqual(set(), negative - set(lane.EXCLUDED_TAGS))

    def test_gpu_tags_are_the_tags_that_mark_gpu_opening_tests(self):
        rules = _read("build_defs/rules.bzl")
        constants = dict(re.findall(r'(?m)^([A-Z_]+) = "([^"]+)"$', rules))
        block = rules.split("and opens_gpu_device:\n", 1)[1].split("\n    _donner_multi", 1)[0]
        added = set()
        for condition, value in re.findall(
                r"(?m)^        if (.*):\n            tags = tags \+ \[([^\]]+)\]$", block):
            if "remote_parallel_ci" not in condition:
                added.add(constants.get(value, value.strip('"')))
        self.assertTrue(added)
        self.assertLessEqual(added, set(lane.GPU_TAGS))

    def test_xcode_identity_is_the_default_pinned_xcode_in_the_docs(self):
        identity = lane.xcode_identity(_root())
        self.assertEqual({identity}, set(re.findall(
            r"%s=([0-9.]+-[0-9A-Za-z]+)" % lane.IDENTITY, _read("docs/building.md"))))
        options = bazelrc_configs.expand("build", ["--config=remote-macos"], self.configs)
        self.assertEqual(identity.split("-")[0], bazelrc_configs.last_value(options,
                                                                            "xcode_version"))

    def test_selection_and_dry_run(self):
        self.assertEqual(["//..."], lane.selection("true", "//a:b"))
        self.assertEqual(["//a:b", "//c:d"], lane.selection("false", " //a:b  //c:d "))
        self.assertEqual([], lane.selection("false", "  "))
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [sys.executable, str(_root() / "tools/ci/macos_remote_shadow.py"), "--dry-run",
                 "--out", directory, "--fallback", "false", "--affected", " ".join(self.TARGETS)],
                capture_output=True, text=True, check=False,
                env=dict(os.environ, DONNER_CI_XCODE_IDENTITY=""))
        self.assertNotEqual(0, result.returncode, "an empty identity contradicts the config")
        environ = {"FALLBACK": "false", "AFFECTED": " ".join(self.TARGETS)}
        lines = _capture_main(["--dry-run", "--out", self.OUT], environ)
        self.assertEqual("%s=%s" % (lane.IDENTITY, lane.xcode_identity(_root())), lines[0])
        startup = [lane.MAX_IDLE]
        preflights = [command for command, _ in lane.preflight_commands(
            "bazelisk", startup, [], self.OUT).values()]
        routing = lane.routing_command("bazelisk", startup, [], self.OUT)[0]
        phases = [lane.phase_command(phase, "bazelisk", startup, [], self.OUT, self.TARGETS)
                  for phase in lane.PHASES]
        self.assertEqual([shlex.join(command) for command in preflights + [routing] + phases] +
                         ["bazelisk --max_idle_secs=600 shutdown"], lines[1:])


def _capture_main(argv, environ):
    import contextlib
    import io
    stream = io.StringIO()
    with contextlib.redirect_stdout(stream):
        status = lane.main(argv, environ)
    if status:
        raise AssertionError("main exited %d" % status)
    return stream.getvalue().splitlines()


def _events(*events):
    return [json.dumps(event) for event in events]


def _canonical(options):
    return {"id": {"structuredCommandLine": {"commandLineLabel": "canonical"}},
            "structuredCommandLine": {"sections": [{"sectionLabel": "command options",
                                                    "optionList": {"option": options}}]}}


_FAKE_BAZEL = r'''
import json, os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(" ".join(args) + "\n")
if "shutdown" in args:
    sys.exit(0)
events = next(a.split("=", 1)[1] for a in args if a.startswith("--build_event_json_file="))
os.makedirs(os.path.dirname(events), exist_ok=True)
lines = []
if "//tools/ci:remote_macos_probe" in args:
    # The routing probe test passes only on macOS arm64; its result says how it ran.
    passed = os.environ.get("FAKE_UNAME", "Darwin arm64") == "Darwin arm64"
    info = {"strategy": os.environ.get("FAKE_ROUTING_STRATEGY", "remote")}
    if os.environ.get("FAKE_ROUTING_CACHED"):
        info["cachedRemotely"] = True
    if not os.environ.get("FAKE_ROUTING_STATUS"):
        # FAKE_ROUTING_STATUS stands for a probe that did not build: it reports no result.
        lines.append({"id": {"testResult": {"label": "//tools/ci:remote_macos_probe"}},
                      "testResult": {"status": "PASSED" if passed else "FAILED",
                                     "executionInfo": info}})
    status = int(os.environ.get("FAKE_ROUTING_STATUS", "0" if passed else "3"))
elif "--nobuild" in args:
    # The lane probe sees FAKE_EXECUTOR, the probe without the config FAKE_PLAIN_EXECUTOR.
    executor = os.environ.get("FAKE_EXECUTOR" if "--config=remote-macos" in args
                              else "FAKE_PLAIN_EXECUTOR")
    options = [{"optionName": "spawn_strategy", "optionValue": "remote"}]
    if executor:
        options.append({"optionName": "remote_executor", "optionValue": executor})
    label = "original" if os.environ.get("FAKE_NO_CANONICAL") else "canonical"
    lines.append({"id": {"structuredCommandLine": {"commandLineLabel": label}},
                  "structuredCommandLine": {"sections": [{"optionList": {"option": options}}]}})
    status = int(os.environ.get("FAKE_PREFLIGHT_STATUS", "0"))
else:
    phase = "gpu" if "--config=remote-macos-gpu" in args else "native"
    status = int(os.environ.get("FAKE_STATUS_" + phase.upper(), "0"))
    label = "//fake:%s_test" % phase
    result = {}
    if os.environ.get("FAKE_TESTLOGS"):
        # The test's outputs as Bazel downloads them, and one it names but did not download.
        directory = os.path.join(os.environ["FAKE_TESTLOGS"], "execroot", "_main", "bazel-out",
                                 "darwin_arm64-fastbuild", "testlogs", "fake", phase + "_test")
        os.makedirs(os.path.join(directory, "test.outputs"), exist_ok=True)
        files = {"test.log": "%s log\n" % phase, "test.xml": "<testsuites/>\n",
                 "test.outputs/outputs.zip": "%s images\n" % phase}
        for name, text in files.items():
            with open(os.path.join(directory, name), "w") as handle:
                handle.write(text)
        result = {"status": "FAILED" if status else "PASSED", "testActionOutput": [
            {"name": name.replace("/", "__"), "uri": "file://" + os.path.join(directory, name)}
            for name in [*files, "test.outputs_manifest/MANIFEST"]]}
    lines += [{"id": {"targetCompleted": {"label": label}}, "completed": {"success": True}},
              {"id": {"testResult": {"label": label}}, "testResult": result},
              {"id": {"testSummary": {"label": label}},
               "testSummary": {"overallStatus": "FAILED" if status else "PASSED"}},
              {"id": {"buildFinished": {}},
               "finished": {"exitCode": {"name": "X", "code": status}}},
              {"id": {"buildMetrics": {}}, "buildMetrics": {
                  "actionSummary": {"actionsExecuted": "7", "runnerCount": [
                      {"name": "total", "count": 9}, {"name": "remote", "count": 7}]},
                  "timingMetrics": {"wallTimeInMs": "1500"}}}]
with open(events, "w") as handle:
    handle.write("".join(json.dumps(line) + "\n" for line in lines))
sys.exit(status)
'''


def _collect(case, testlogs, cwd=None):
    """What the workflow's collector uploads when its `testlogs` input is `testlogs` (GitHub
    passes an omitted input as empty), run in `cwd` or an empty directory."""
    action = _read(COLLECTOR[2:] + "/action.yml")
    body = re.search(r"(?m)^      run: \|\n((?:(?:        .*)?\n)+)", action).group(1)
    directory = tempfile.TemporaryDirectory()
    case.addCleanup(directory.cleanup)
    result = subprocess.run(["bash", "-c", textwrap.dedent(body)], cwd=cwd or directory.name,
                            env=dict(os.environ, RUNNER_TEMP=directory.name, TESTLOGS=testlogs),
                            capture_output=True, text=True, check=False)
    case.assertEqual(0, result.returncode, result.stdout + result.stderr)
    artifacts = Path(directory.name) / "bazel-test-artifacts"
    return {path.relative_to(artifacts).as_posix(): path.read_text(encoding="utf-8")
            for path in artifacts.rglob("*") if path.is_file()}


class LaneRunTest(unittest.TestCase):
    """Runs the lane end to end with a stand-in Bazel."""

    @classmethod
    def setUpClass(cls):
        cls.executables = ExecutableFixtureDirectory()
        cls.addClassCleanup(cls.executables.cleanup)
        script = cls.executables.path / "fake_bazel.py"
        script.write_text(_FAKE_BAZEL, encoding="utf-8")
        cls.bazel = str(cls.executables.write_python_entry("bazelisk", script))

    def _run(self, **environment):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        out = Path(directory.name) / "out"
        summary_file = Path(directory.name) / "step-summary.md"
        environ = dict(os.environ, FALLBACK="false", AFFECTED="//fake:all",
                       FAKE_LOG=str(Path(directory.name) / "calls"),
                       GITHUB_STEP_SUMMARY=str(summary_file), BAZEL_HEARTBEAT_INTERVAL_SECONDS="1",
                       **environment)
        environ.pop(lane.IDENTITY, None)
        # The fake Bazel's output tree stands for the lane's own output root.
        root = ["--output-user-root", environ["FAKE_TESTLOGS"]] if "FAKE_TESTLOGS" in environ \
            else []
        result = subprocess.run(
            [sys.executable, str(_root() / "tools/ci/macos_remote_shadow.py"), "--out", str(out),
             "--bazel", self.bazel, *root], cwd=_root(), env=environ, capture_output=True,
            text=True, check=False)
        calls = (Path(directory.name) / "calls").read_text(encoding="utf-8").splitlines()
        return result, out, summary_file, calls

    def _assert_stopped(self, result, out, calls, status, message):
        self.assertEqual(status, result.returncode, result.stdout + result.stderr)
        bazel = [call for call in calls if not call.endswith("shutdown")]
        self.assertLessEqual(len(bazel), 3, "only the preflight probes may run")
        self.assertTrue(calls[-1].endswith("shutdown"), "the lane leaves no Bazel server")
        self.assertIn(message, result.stdout)
        self.assertIn(message, (out / "report.md").read_text(encoding="utf-8"))
        self.assertEqual([], sorted(path.name for path in (out / "preflight").glob("*.json")))
        self.assertNotIn("invalid", result.stdout + result.stderr)

    def test_unconfigured_runner_fails_fast_without_building(self):
        result, out, summary_file, calls = self._run()
        self._assert_stopped(result, out, calls, 2,
                             "runner is not configured for the remote macOS lane")
        self.assertIn("build:remote-macos-base", summary_file.read_text(encoding="utf-8"))

    def test_executor_shared_with_a_build_without_the_config_stops_the_lane(self):
        endpoint = "grpcs://default-executor.invalid"
        result, out, _, calls = self._run(FAKE_EXECUTOR=endpoint, FAKE_PLAIN_EXECUTOR=endpoint)
        self._assert_stopped(result, out, calls, 2,
                             "resolves to the same remote executor as a build without it")

    def test_failed_preflight_stops_the_lane(self):
        result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                          FAKE_PREFLIGHT_STATUS="37")
        self._assert_stopped(result, out, calls, 37, "preflight Bazel command failed (exit 37)")

    def test_executor_that_is_not_macos_stops_the_lane(self):
        result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                          FAKE_UNAME="Linux aarch64")
        self._assert_stopped(result, out, calls, 2,
                             "reported a system other than macOS arm64 (exit 3)")

    def test_routing_probe_that_did_not_execute_remotely_is_not_proven(self):
        # A result kept by a persistent output root, or one that ran on the runner, proves nothing.
        for environment in ({"FAKE_ROUTING_CACHED": "1"}, {"FAKE_ROUTING_STRATEGY": "local"}):
            with self.subTest(**environment):
                result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                                  **environment)
                self._assert_stopped(result, out, calls, 2, "the routing probe is not proven")

    def test_routing_probe_that_cannot_run_stops_the_lane(self):
        result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                          FAKE_ROUTING_STATUS="1")
        self._assert_stopped(result, out, calls, 2, "did not build or run on the configured "
                             "executor (exit 1)")
        self.assertNotIn("other than macOS arm64", result.stdout)

    def test_unreadable_preflight_is_not_reported_as_unconfigured(self):
        result, out, _, calls = self._run(FAKE_EXECUTOR="grpcs://executor.invalid",
                                          FAKE_NO_CANONICAL="1")
        self._assert_stopped(result, out, calls, 2, "build event file could not be read")
        self.assertNotIn("not configured", result.stdout)

    def test_configured_runner_runs_both_phases_and_never_prints_the_endpoint(self):
        endpoint = "grpcs://private-executor.invalid:50051"
        result, out, summary_file, calls = self._run(FAKE_EXECUTOR=endpoint)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(6, len(calls))
        self.assertIn("--config=remote-macos-gpu --jobs=4", calls[4])
        self.assertTrue(calls[5].endswith("shutdown"))
        outputs = {"stdout": result.stdout + result.stderr,
                   "summary": summary_file.read_text(encoding="utf-8"),
                   "json": (out / "shadow-summary.json").read_text(encoding="utf-8"),
                   "report": (out / "report.md").read_text(encoding="utf-8")}
        for name, text in outputs.items():
            with self.subTest(output=name):
                self.assertNotIn("private-executor", text)
        self.assertEqual([], sorted(path.name for path in (out / "preflight").glob("*.json")))
        summary = json.loads(outputs["json"])
        for phase in lane.PHASES:
            entry = summary["phases"][phase]
            self.assertEqual(0, entry["status"])
            self.assertEqual({"total": 1, "passed": 1, "flaky": 0, "failed": 0, "executed": 1,
                              "cached": 0}, entry["tests"])
            self.assertEqual({"remote": 7}, entry["runners"])
            self.assertEqual(1.5, entry["wall_time_seconds"])
        self.assertIn("| native | 0 |", outputs["report"])

    def test_failed_phase_is_reported_and_the_other_phase_still_runs(self):
        result, out, summary_file, calls = self._run(
            FAKE_EXECUTOR="grpcs://executor.invalid", FAKE_STATUS_NATIVE="3")
        self.assertEqual(3, result.returncode)
        self.assertEqual(6, len(calls))
        summary = json.loads((out / "shadow-summary.json").read_text(encoding="utf-8"))
        self.assertEqual(["//fake:native_test"], summary["phases"]["native"]["failed_tests"])
        self.assertEqual(0, summary["phases"]["gpu"]["status"])
        self.assertIn("::warning::macOS remote shadow lane: the native phase exited 3",
                      result.stdout)
        self.assertIn("//fake:native_test", summary_file.read_text(encoding="utf-8"))

    def _collected(self, out):
        """What the workflow's collector uploads from the script's staged tree."""
        return _collect(self, str(out / lane.ARTIFACTS))

    def _run_with_outputs(self, **environment):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        testlogs = Path(directory.name) / "output-base"
        # A failed result an earlier run left in the lane's persistent output root.
        stale = testlogs / "execroot/_main/bazel-out/darwin_arm64-fastbuild/testlogs/fake/old_test"
        (stale / "test.outputs").mkdir(parents=True)
        (stale / "test.log").write_text("an earlier run\n", encoding="utf-8")
        (stale / "test.outputs" / "outputs.zip").write_text("old images\n", encoding="utf-8")
        return self._run(FAKE_EXECUTOR="grpcs://executor.invalid", FAKE_TESTLOGS=str(testlogs),
                         **environment)

    @staticmethod
    def _phase_outputs(phase):
        prefix = "%s/darwin_arm64-fastbuild/fake/%s_test/" % (phase, phase)
        return {prefix + "test.log": "%s log\n" % phase,
                prefix + "test.xml": "<testsuites/>\n",
                prefix + "test.outputs/outputs.zip": "%s images\n" % phase}

    def test_each_failed_phase_stages_its_failed_tests_outputs_for_the_collector(self):
        for failing in (("native", "gpu"), ("native",), ("gpu",)):
            with self.subTest(failing=failing):
                result, out, _, _ = self._run_with_outputs(**{
                    "FAKE_STATUS_" + phase.upper(): "3" for phase in failing})
                self.assertEqual(3, result.returncode, result.stdout + result.stderr)
                expected = {}
                for phase in failing:
                    expected.update(self._phase_outputs(phase))
                self.assertEqual(expected, self._collected(out))
                for phase in failing:
                    self.assertIn("staged 3 output files (", result.stdout)
                    self.assertIn("of the %s phase's failed tests" % phase, result.stdout)

    def test_nothing_is_staged_when_every_test_passes(self):
        result, out, _, _ = self._run_with_outputs()
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual([], [path for path in (out / lane.ARTIFACTS).rglob("*")
                              if path.is_file()])
        self.assertEqual({}, self._collected(out))
        self.assertNotIn("staged", result.stdout)

    def test_empty_selection_fails_before_bazel(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        result = subprocess.run(
            [sys.executable, str(_root() / "tools/ci/macos_remote_shadow.py"), "--out",
             directory.name, "--bazel", self.bazel, "--affected", "  "],
            cwd=_root(), capture_output=True, text=True, check=False,
            env=dict(os.environ, FALLBACK="false", FAKE_LOG=str(Path(directory.name) / "calls")))
        self.assertEqual(2, result.returncode)
        self.assertIn("Empty target selection", result.stdout)
        self.assertFalse((Path(directory.name) / "calls").exists())


# Writes Bazel-like progress lines to a console log, as the watchdog's tee does, in one of three
# shapes: a starved executor (the counter frozen, only the elapsed time growing), slow but steady
# progress, and one long action that finishes just inside the bound.
_FAKE_CONSOLE = r'''
import signal, sys, time
mode, console, step = sys.argv[1], sys.argv[2], float(sys.argv[3])
interrupts = []
signal.signal(signal.SIGINT, lambda *_: interrupts.append(1))
def line(done, total, elapsed):
    with open(console, "a") as handle:
        handle.write("[%d / %d] Compiling a.cc; %.2fs remote, remote-cache ... (3 actions, "
                     "0 running)\n" % (done, total, elapsed))
start = time.monotonic()
if mode == "starved":
    # Bounded, so a guard that never fires fails the test instead of hanging it.
    while len(interrupts) < (3 if len(sys.argv) > 4 else 1) and time.monotonic() - start < 30:
        line(137, 150, time.monotonic() - start)
        time.sleep(step)
    sys.exit(8)
if mode == "progressing":
    for done in range(10):
        line(done, 10, time.monotonic() - start)
        time.sleep(step)
    sys.exit(0)
if mode == "long":
    line(5, 10, 0)
    while time.monotonic() - start < step:
        line(5, 10, time.monotonic() - start)
        time.sleep(0.05)
    line(10, 10, time.monotonic() - start)
    sys.exit(0)
'''


class StarvationGuardTest(unittest.TestCase):
    """The lane stops a phase whose executor completes nothing, and only that."""

    LIMIT = 1.0

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.console = self.directory / "console.log"
        self.script = self.directory / "fake_console.py"
        self.script.write_text(_FAKE_CONSOLE, encoding="utf-8")

    def _phase(self, mode, step, *extra):
        import contextlib
        import io
        command = [sys.executable, str(self.script), mode, str(self.console), str(step), *extra]
        started = time.monotonic()
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            status, starved = lane.run_phase(command, dict(os.environ), self.console,
                                             limit=self.LIMIT, poll=0.05, grace=(2, 2, 2))
        return status, starved, time.monotonic() - started, printed.getvalue()

    def test_frozen_counter_with_a_growing_elapsed_time_is_starved(self):
        status, starved, seconds, printed = self._phase("starved", 0.05)
        self.assertEqual((lane.STARVED_STATUS, True), (status, starved))
        self.assertLess(seconds, self.LIMIT + 5)
        self.assertIn(lane.STARVED, printed)
        # The elapsed time kept changing, so a whole-line comparison saw progress throughout.
        lines = [line for line in self.console.read_text().splitlines() if line.startswith("[")]
        self.assertGreater(len(set(lines)), 5)
        self.assertEqual({(137, 150)}, {lane.progress(line) for line in lines})

    def test_a_client_that_ignores_the_first_interrupts_is_still_stopped(self):
        status, starved, seconds, _ = self._phase("starved", 0.05, "stubborn")
        self.assertEqual((lane.STARVED_STATUS, True), (status, starved))
        self.assertLess(seconds, self.LIMIT + 8)

    def test_slow_but_steady_progress_is_not_starved(self):
        status, starved, _, _ = self._phase("progressing", self.LIMIT * 0.4)
        self.assertEqual((0, False), (status, starved))

    def test_one_long_action_inside_the_bound_is_not_starved(self):
        status, starved, _, _ = self._phase("long", self.LIMIT * 0.8)
        self.assertEqual((0, False), (status, starved))

    def test_starved_phase_skips_the_next_phase_and_is_reported(self):
        from unittest import mock
        calls = []

        def fake_phase(command, environment, console):
            calls.append(command)
            return lane.STARVED_STATUS, True

        args = lane.parse_args(["--out", str(self.directory / "out"), "--fallback", "true"], {})
        with mock.patch.object(lane, "_preflight", return_value=(None, 0)), \
                mock.patch.object(lane, "run_phase", side_effect=fake_phase):
            import contextlib
            import io
            with contextlib.redirect_stdout(io.StringIO()):
                status = lane.run(args, {})
        self.assertEqual(lane.STARVED_STATUS, status)
        self.assertEqual(1, len(calls))
        summary = json.loads((self.directory / "out" / "shadow-summary.json").read_text())
        self.assertEqual(lane.STARVED, summary["phases"]["native"]["outcome"])
        self.assertIsNone(summary["phases"]["gpu"])
        report = (self.directory / "out" / "report.md").read_text()
        self.assertIn(lane.STARVED, report)
        self.assertIn("| gpu | - | not run |", report)

    def test_routing_probe_runs_under_its_own_bound(self):
        from unittest import mock
        seen = {}

        def fake_phase(command, environment, console, limit):
            seen.update(limit=limit, console=console)
            return lane.STARVED_STATUS, True

        args = lane.parse_args(["--out", str(self.directory / "out")], {})
        with mock.patch.object(lane, "run_phase", side_effect=fake_phase):
            problem = lane._routing_problem(args, {})
        self.assertEqual(lane.ROUTING_STARVED_SECONDS, seen["limit"])
        self.assertLessEqual(lane.ROUTING_STARVED_SECONDS, 10 * 60)
        self.assertIn("not scheduled within 10 minutes", problem)
        self.assertEqual("routing.log", Path(seen["console"]).name)

    def test_guard_rules(self):
        guard = lane.StarvationGuard(0, limit=10)
        self.assertFalse(guard.starved(None, 100))  # Before the first progress line.
        self.assertFalse(guard.starved((5, 9), 101))
        self.assertFalse(guard.starved((5, 9), 110))
        self.assertTrue(guard.starved((5, 9), 111))
        self.assertFalse(guard.starved((6, 9), 112))  # Advancing resets the clock.
        self.assertFalse(guard.starved((9, 9), 200))  # Nothing left to run.
        self.assertEqual((1234, 5678), lane.progress("x\n[1,234 / 5,678] Testing //a:b\n"))
        # A runner rc may turn on --show_timestamps; the guard must still read the counter.
        self.assertEqual((7, 9), lane.progress("(12:34:56) [7 / 9] Compiling a.cc; 3s remote\n"))
        self.assertEqual((7, 9), lane.progress("(12:34:56.789) [7 / 9] Testing //a:b\n"))
        self.assertIsNone(lane.progress("Loading: 3 packages loaded\n"))

    def test_bound_exceeds_the_longest_test_timeout(self):
        # `long` (900 s) is the longest effective test timeout in the tree; see STARVED_SECONDS.
        self.assertGreater(lane.STARVED_SECONDS, 900)
        self.assertLessEqual(lane.STARVED_SECONDS, 30 * 60)


class BuildEventTest(unittest.TestCase):
    def test_effective_executor_reads_only_the_canonical_command_line(self):
        set_executor = _canonical([{"optionName": "remote_executor", "optionValue": "x"},
                                   {"optionName": "remote_executor", "optionValue": "y"}])
        original = dict(set_executor,
                        id={"structuredCommandLine": {"commandLineLabel": "original"}})
        self.assertEqual("y", lane.effective_executor(_events(original, set_executor)))
        self.assertEqual("", lane.effective_executor(_events(_canonical([
            {"optionName": "remote_cache", "optionValue": "x"}]))))
        self.assertEqual("", lane.effective_executor(_events(_canonical([
            {"optionName": "remote_executor", "optionValue": ""}]))))
        with self.assertRaises(ValueError):
            lane.effective_executor(_events(original))

    def test_probe_must_have_executed_remotely_in_this_invocation(self):
        def result(**fields):
            return {"id": {"testResult": {"label": lane.ROUTING_PROBE}}, "testResult": fields}
        remote = {"strategy": "remote"}
        self.assertTrue(lane.probe_executed_remotely(_events(
            result(status="PASSED", executionInfo=remote))))
        for fields in ({"status": "FAILED", "executionInfo": remote},
                       {"status": "PASSED", "executionInfo": dict(remote, cachedRemotely=True)},
                       {"status": "PASSED", "cachedLocally": True, "executionInfo": remote},
                       {"status": "PASSED", "executionInfo": {"strategy": "linux-sandbox"}}):
            with self.subTest(fields=fields):
                self.assertFalse(lane.probe_executed_remotely(_events(result(**fields))))
        self.assertFalse(lane.probe_executed_remotely(_events(_canonical([]))))

    def test_preflight_problem(self):
        ok = {"lane": 0, "plain": 0}
        self.assertIsNone(lane.preflight_problem(ok, {"lane": "a", "plain": ""}))
        self.assertIsNone(lane.preflight_problem(ok, {"lane": "a", "plain": "b"}))
        for statuses, executors, needle in (
                (ok, {"lane": "", "plain": ""}, "set no remote executor"),
                (ok, {"lane": "a", "plain": "a"}, "same remote executor"),
                (ok, {"lane": None, "plain": "b"}, "could not be read"),
                ({"lane": 0, "plain": 1}, {"lane": "a", "plain": "b"}, "exit 1")):
            with self.subTest(needle=needle):
                self.assertIn(needle, lane.preflight_problem(statuses, executors))

    def test_probe_readers_skip_fields_of_an_unexpected_type(self):
        junk = _events({"id": "x"}, {"id": {"testResult": "x"}}, {"id": ["x"]},
                       {"id": {"testResult": {"label": lane.ROUTING_PROBE}}, "testResult": "x"},
                       {"id": {"testResult": {"label": lane.ROUTING_PROBE}},
                        "testResult": {"status": "PASSED", "executionInfo": "remote"}})
        self.assertFalse(lane.probe_executed_remotely(junk))
        self.assertFalse(lane.probe_ran_and_failed(junk))

    def test_summary_counts_targets_tests_cache_and_runners(self):
        events = _events(
            {"id": {"targetCompleted": {"label": "//a:lib"}}, "completed": {"success": True}},
            {"id": {"targetCompleted": {"label": "//a:bad"}}, "aborted": {}},
            {"id": {"testResult": {"label": "//a:t1"}}, "testResult": {}},
            {"id": {"testResult": {"label": "//a:t2"}},
             "testResult": {"executionInfo": {"cachedRemotely": True}}},
            {"id": {"testResult": {"label": "//a:t3"}}, "testResult": {"cachedLocally": True}},
            {"id": {"testSummary": {"label": "//a:t1"}},
             "testSummary": {"overallStatus": "PASSED"}},
            {"id": {"testSummary": {"label": "//a:t2"}}, "testSummary": {"overallStatus": "FLAKY"}},
            {"id": {"testSummary": {"label": "//a:t3"}},
             "testSummary": {"overallStatus": "TIMEOUT"}},
            {"id": {"buildFinished": {}}, "finished": {"exitCode": {"name": "TESTS_FAILED",
                                                                         "code": 3}}},
            {"id": {"buildMetrics": {}}, "buildMetrics": {
                "actionSummary": {"actionsExecuted": "12", "actionCacheStatistics": {
                    "hits": 30, "misses": "4"}, "runnerCount": [
                        {"name": "total", "count": 20}, {"name": "remote", "count": 10},
                        {"name": "remote cache hit", "count": "8"}]},
                "timingMetrics": {"wallTimeInMs": "65432"}}})
        summary = lane.summarize(events)
        self.assertEqual({"succeeded": 1, "failed": 1}, summary["targets"])
        self.assertEqual({"total": 3, "passed": 1, "flaky": 1, "failed": 1, "executed": 1,
                          "cached": 2}, summary["tests"])
        self.assertEqual(["//a:t3"], summary["failed_tests"])
        self.assertEqual("TESTS_FAILED (3)", summary["exit"])
        self.assertEqual((12, 30, 4), (summary["actions_executed"], summary["action_cache_hits"],
                                       summary["action_cache_misses"]))
        self.assertEqual({"remote": 10, "remote cache hit": 8}, summary["runners"])
        self.assertEqual(65.4, summary["wall_time_seconds"])


class StagingTest(unittest.TestCase):
    """What the lane stages from a phase's build events, and what it refuses to stage."""

    CFG = "darwin_arm64-fastbuild"

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.dir = Path(directory.name).resolve()
        self.root = self.dir / "output-root"
        self.execroot = self.root / "base" / "execroot" / "_main"
        self.testlogs = self.execroot / "bazel-out" / self.CFG / "testlogs"
        self.out = self.dir / "out"

    def _file(self, path, data=b"failure details"):
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    @staticmethod
    def _result(label, *uris, status="FAILED", outputs=None):
        fields = {"testActionOutput": outputs if outputs is not None else
                  [{"name": "test.log", "uri": uri} for uri in uris]}
        if status:
            fields["status"] = status
        return {"id": {"testResult": {"label": label}}, "testResult": fields}

    def _stage(self, *events, budget=lane.MAX_ARTIFACT_TOTAL_BYTES, root=None):
        with contextlib.redirect_stdout(io.StringIO()) as printed:
            files, used = lane.stage_failed_test_outputs(
                _events(*events), self.out, "native", str(root or self.root), budget)
        stage = self.out / lane.ARTIFACTS / "native"
        staged = {path.relative_to(stage).as_posix(): path.read_bytes()
                  for path in stage.rglob("*") if path.is_file()}
        self.assertEqual((len(staged), sum(map(len, staged.values()))), (files, used))
        return staged, printed.getvalue()

    def _assert_refused(self, uri, control=None):
        """Nothing is staged from `uri`, while a valid output after it still is."""
        control = control or self._file(self.testlogs / "ok" / "test.log", b"ok")
        staged, _ = self._stage(self._result("//a:bad", uri, control.as_uri()))
        self.assertEqual({self.CFG + "/ok/test.log": b"ok"}, staged)

    def test_selection_reads_only_this_invocations_results_that_did_not_pass(self):
        logs = "/r/base/execroot/_main/bazel-out/%s/testlogs" % self.CFG
        other = "/r/base/execroot/_main/bazel-out/k8-opt/testlogs"
        lines = _events(
            self._result("//a:passed", "file://%s/a/passed/test.log" % logs, status="PASSED"),
            self._result("//a:failed", outputs=[
                {"uri": "file://%s/a/failed/%s" % (logs, name)} for name in (
                    "test.log", "test.xml", "test.outputs/outputs.zip",
                    # Not collected: the manifest and an earlier attempt's log.
                    "test.outputs_manifest/MANIFEST", "test_attempts/attempt_1.log")]),
            self._result("//a:timeout", "file://%s/a/timeout/test.log" % logs, status="TIMEOUT"),
            # Bazel omits a NO_STATUS result's status.
            self._result("//a:unknown", "file://localhost%s/a/unknown/test.log" % logs,
                         status=None),
            # The same test in a second configuration keeps its own key; a repeat does not.
            self._result("//a:failed", "file://%s/a/failed/test.log" % other,
                         "file://%s/a/failed/test.log" % other),
            {"id": {"testSummary": {"label": "//a:failed"}},
             "testSummary": {"overallStatus": "FAILED"}}) + [
            # A Bazel server stopped mid-write leaves a partial last line.
            '{"id": {"testResult": {"label": "//a:cut']
        selected = [(str(key), path, label) for key, path, label in lane.failed_test_outputs(lines)]
        self.assertEqual([
            (self.CFG + "/a/failed/test.log", logs + "/a/failed/test.log", "//a:failed"),
            (self.CFG + "/a/failed/test.xml", logs + "/a/failed/test.xml", "//a:failed"),
            (self.CFG + "/a/failed/test.outputs/outputs.zip",
             logs + "/a/failed/test.outputs/outputs.zip", "//a:failed"),
            (self.CFG + "/a/timeout/test.log", logs + "/a/timeout/test.log", "//a:timeout"),
            (self.CFG + "/a/unknown/test.log", logs + "/a/unknown/test.log", "//a:unknown"),
            ("k8-opt/a/failed/test.log", other + "/a/failed/test.log", "//a:failed")], selected)
        self.assertEqual(1, lane.summarize(lines)["tests"]["failed"])

    def test_a_downloaded_failed_output_is_staged_and_an_earlier_stage_is_replaced(self):
        log = self._file(self.testlogs / "a" / "failed" / "test.log")
        earlier = self._file(self.out / lane.ARTIFACTS / "native" / "earlier" / "test.log")
        missing = self.testlogs / "a" / "failed" / "test.xml"
        staged, printed = self._stage(self._result("//a:failed", log.as_uri(), missing.as_uri()))
        self.assertEqual({self.CFG + "/a/failed/test.log": b"failure details"}, staged)
        self.assertFalse(earlier.exists())
        self.assertNotIn("::warning::", printed)

    def test_only_a_local_file_uri_is_read(self):
        log = self._file(self.testlogs / "a" / "test.log")
        for uri in ("http://%s" % log, "file://cache.invalid%s" % log,
                    "bytestream://cache.invalid/blobs/0/1"):
            with self.subTest(uri=uri):
                self._assert_refused(uri)

    def test_an_empty_dot_or_dot_dot_segment_is_refused(self):
        # Each names a real file inside the output root once normalized, so only the segment
        # rule refuses it; an empty segment after testlogs would otherwise make a key that
        # starts at the root of the file system.
        self._file(self.testlogs / "etc" / "x" / "test.log")
        for uri in ("file://%s//etc/x/test.log" % self.testlogs,
                    "file://%s/./etc/x/test.log" % self.testlogs,
                    "file://%s/x/../base/execroot/_main/bazel-out/%s/testlogs/etc/x/test.log"
                    % (self.root, self.CFG)):
            with self.subTest(uri=uri):
                self._assert_refused(uri)

    def test_malformed_or_nul_uris_and_odd_fields_are_skipped(self):
        control = self._file(self.testlogs / "ok" / "test.log", b"ok")
        self._file(self.testlogs / "a" / "test.log")
        staged, _ = self._stage(
            self._result("//a:bad", "file://[", "file://%s/a%%00/test.log" % self.testlogs),
            self._result("//a:odd", outputs="file://%s/a/test.log" % self.testlogs),
            self._result("//a:odd", outputs=5),
            self._result("//a:odd", outputs=["x", {"uri": 7}, {"uri": None}]),
            {"id": {"testResult": "x"}, "testResult": {"status": "FAILED"}},
            {"id": {"testResult": {"label": "//a:odd"}}, "testResult": ["x"]},
            self._result("//a:ok", control.as_uri()))
        self.assertEqual({self.CFG + "/ok/test.log": b"ok"}, staged)

    def test_a_symbolic_link_anywhere_below_the_output_root_is_refused(self):
        inside = self._file(self.root / "elsewhere" / "a" / "test.log")
        outside = self._file(self.dir / "outside" / "a" / "test.log")
        self.testlogs.mkdir(parents=True, exist_ok=True)
        (self.testlogs / "inside").symlink_to(inside.parent, target_is_directory=True)
        (self.testlogs / "outside").symlink_to(outside.parent, target_is_directory=True)
        (self.testlogs / "leaf").mkdir()
        (self.testlogs / "leaf" / "test.log").symlink_to(inside)
        for name in ("inside", "outside", "leaf"):
            with self.subTest(link=name):
                self._assert_refused((self.testlogs / name / "test.log").as_uri())

    def test_a_source_outside_the_output_root_is_refused(self):
        elsewhere = self.dir / "other-root" / "base/execroot/_main/bazel-out" / self.CFG
        self._assert_refused(self._file(elsewhere / "testlogs" / "a" / "test.log").as_uri())

    def test_an_output_root_reached_through_a_link_still_stages(self):
        log = self._file(self.testlogs / "a" / "test.log")
        link = self.dir / "root-link"
        link.symlink_to(self.root, target_is_directory=True)
        uri = "file://%s" % (link / log.relative_to(self.root))
        staged, _ = self._stage(self._result("//a:failed", uri), root=link)
        self.assertEqual({self.CFG + "/a/test.log": b"failure details"}, staged)

    def test_only_a_regular_file_is_opened_and_a_final_link_is_not_followed(self):
        regular = self._file(self.dir / "regular", b"12345")
        link = self.dir / "link"
        link.symlink_to(regular)
        fifo = self.dir / "fifo"
        os.mkfifo(fifo)
        opened = lane._open_regular(str(regular))
        self.addCleanup(os.close, opened[0])
        self.assertEqual(5, opened[1])
        for path in (link, self.dir, fifo, self.dir / "absent"):
            with self.subTest(path=path.name):
                self.assertIsNone(lane._open_regular(str(path)))

    def test_a_staged_file_stays_inside_the_stage_root(self):
        stage = self.out / "stage"
        outside = self.dir / "outside-stage"
        outside.mkdir()
        stage.mkdir(parents=True)
        (stage / "link").symlink_to(outside, target_is_directory=True)
        self.assertEqual(stage / "cfg" / "a" / "test.log",
                         lane._destination(stage, "cfg/a/test.log"))
        # "a/../b" stays inside, so only the lexical rule refuses it; "link/x" passes that rule,
        # so only the resolved containment check refuses it.
        for key in ("/etc/x", "a/../b", "a/../../x", "link/x"):
            with self.subTest(key=key):
                with self.assertRaises(ValueError):
                    lane._destination(stage, key)
        self.assertEqual([], list(outside.iterdir()))

    def test_a_file_over_the_per_file_cap_is_skipped_and_named(self):
        from unittest import mock
        big = self._file(self.testlogs / "big" / "test.log", b"x" * 11)
        small = self._file(self.testlogs / "small" / "test.log", b"x" * 10)
        with mock.patch.object(lane, "MAX_ARTIFACT_FILE_BYTES", 10):
            staged, printed = self._stage(self._result("//a:big", big.as_uri()),
                                          self._result("//a:small", small.as_uri()))
        self.assertEqual([self.CFG + "/small/test.log"], list(staged))
        self.assertIn("::warning::", printed)
        self.assertIn("big/test.log of //a:big is not uploaded: 11 bytes is over the per-file "
                      "cap of 10 bytes", printed)

    def test_files_over_the_per_run_cap_are_skipped_and_named(self):
        first = self._file(self.testlogs / "first" / "test.log", b"x" * 6)
        second = self._file(self.testlogs / "second" / "test.log", b"x" * 6)
        staged, printed = self._stage(self._result("//a:first", first.as_uri()),
                                      self._result("//a:second", second.as_uri()), budget=10)
        self.assertEqual([self.CFG + "/first/test.log"], list(staged))
        self.assertIn("second/test.log of //a:second is not uploaded: 6 bytes is over the "
                      "per-run cap of 4 bytes", printed)

    def test_the_copy_itself_is_bounded(self):
        # A file that grows after it was measured: the copy stops at the cap plus one byte.
        source = self._file(self.dir / "growing", b"x" * 20)
        for limit, expected in ((20, 20), (19, None), (25, 20)):
            with self.subTest(limit=limit):
                target = self.dir / ("copy-%d" % limit)
                read = os.open(source, os.O_RDONLY)
                write = os.open(target, os.O_WRONLY | os.O_CREAT)
                try:
                    self.assertEqual(expected, lane._copy_bounded(read, write, limit))
                finally:
                    os.close(read)
                    os.close(write)
                self.assertLessEqual(target.stat().st_size, limit)

    def test_staging_and_summary_failures_never_cost_the_report(self):
        from unittest import mock
        log = self._file(self.testlogs / "a" / "test.log")
        bep = _events({"id": {"testSummary": {"label": "//a:t"}},
                       "testSummary": {"overallStatus": "FAILED"}},
                      self._result("//a:t", "file://[", "file://%s/a%%00/test.log" % self.testlogs,
                                   log.as_uri()),
                      self._result("//a:t", outputs="not a list"))

        def fake_phase(command, environment, console):
            events = next(word.split("=", 1)[1] for word in command
                          if word.startswith("--build_event_json_file="))
            Path(events).write_text("\n".join(bep) + "\n", encoding="utf-8")
            return 3, False

        for name, patch in (
                ("malformed events", None),
                ("staging raises", mock.patch.object(lane, "stage_failed_test_outputs",
                                                     side_effect=RuntimeError("x"))),
                ("summary raises", mock.patch.object(lane, "summarize",
                                                     side_effect=self._raising_summarize()))):
            with self.subTest(case=name):
                out = self.dir / ("out-" + name.replace(" ", "-"))
                args = lane.parse_args(["--out", str(out), "--fallback", "true",
                                        "--output-user-root", str(self.root)], {})
                with contextlib.ExitStack() as stack:
                    stack.enter_context(mock.patch.object(lane, "_preflight",
                                                          return_value=(None, 0)))
                    stack.enter_context(mock.patch.object(lane, "run_phase",
                                                          side_effect=fake_phase))
                    if patch is not None:
                        stack.enter_context(patch)
                    printed = stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    status = lane.run(args, {})
                self.assertEqual(3, status)
                self.assertTrue((out / "report.md").is_file())
                summary = json.loads((out / "shadow-summary.json").read_text(encoding="utf-8"))
                self.assertEqual(3, summary["phases"]["native"]["status"])
                if name == "malformed events":
                    self.assertEqual(
                        [self.CFG + "/a/test.log"],
                        [path.relative_to(out / lane.ARTIFACTS / "native").as_posix()
                         for path in (out / lane.ARTIFACTS / "native").rglob("*")
                         if path.is_file()])
                else:
                    self.assertIn("::warning::", printed.getvalue())

    @staticmethod
    def _raising_summarize():
        real = lane.summarize

        def summarize(lines):
            if lines:
                raise TypeError("malformed")
            return real(lines)
        return summarize

    def test_the_collector_reads_bazel_testlogs_when_its_input_is_empty(self):
        workspace = self.dir / "workspace"
        workspace.mkdir()
        logs = self.dir / "logs"
        self._file(logs / "pkg" / "test" / "test.log", b"from bazel-testlogs")
        self._file(logs / "pkg" / "test" / "test.outputs" / "actual.png", b"image")
        (workspace / "bazel-testlogs").symlink_to(logs, target_is_directory=True)
        self.assertEqual({"pkg/test/test.log": "from bazel-testlogs",
                          "pkg/test/test.outputs/actual.png": "image"},
                         _collect(self, "", cwd=str(workspace)))


if __name__ == "__main__":
    unittest.main()
