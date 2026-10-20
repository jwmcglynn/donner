"""Keeps the GitHub-hosted and self-hosted macOS lanes on identical Bazel action keys.

The two lanes in main.yml can share one remote cache only if every key-affecting input matches:
the Bazel flags and configs, the action environment (PATH in particular, which Bazel copies from
the client unless it is pinned), and the Xcode version and build, which Apple toolchain actions
carry in their environment. Both lanes configure the remote cache only through the BuildBuddy
action, so their build flags must be identical. Test-only flags and environment (--test_env)
are deliberately left to each lane, and each lane names itself in its test environment: a test
result depends on the runner's GPU, so it must not be shared across runners.

Every GitHub-hosted job is also kept off any private remote cache and remote
execution configuration.
"""

from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile
import textwrap
import unittest

from python.runfiles import runfiles
from tools import test_timing


HOSTED = "macos"
SELF_HOSTED = "macos-self-hosted"
# Reruns the hosted job's runner-sensitive tests on the self-hosted runner under the hosted
# runner's resource limits, with the hosted job's action keys.
PARITY = "macos-hosted-parity"
# Remote cache and executor flags. The macOS lanes get their remote cache only from the
# BuildBuddy action, so neither passes any of these itself.
REMOTE_FLAG = re.compile(r"^--(?:no)?(?:remote_|incompatible_remote_|credential_helper)")
LANE_FLAGS = "$BAZEL_MACOS_BUILD_FLAGS"
BAZEL_INVOCATION = re.compile(
    r"\bbazel(?:isk)?(?:\s+--[a-z_]+)*\s+(build|test|fetch|query|cquery|coverage|run)\b")
# Flags that only select, run or report tests. They never change a build action's key.
TEST_ONLY_FLAG = re.compile(r"^--(?:no)?(?:test_|cache_test_results|local_test_jobs|"
                            r"flaky_test_attempts|runs_per_test|keep_going|build_event_json_file|"
                            r"run_under)")
# Flags that only limit how many actions Bazel runs at once, or which requested targets it
# builds. They never change a build action's key.
SCHEDULING_ONLY_FLAG = re.compile(r"^--(?:jobs|local_resources|build_tests_only)(?:=|$)")
# Job environment values a lane may set for itself; each may hold only test-only flags.
TEST_ONLY_VARIABLES = ("BAZEL_MACOS_TEST_FLAGS", "BROWSER_STALL_TEST_FLAGS")
_RC_LINE = re.compile(r"^(?:build|test|common):([A-Za-z0-9_-]+)\s+(.*)$")
# Each lane names itself in its test environment, so test results never cross runners.
LANE_TEST_ENV = {"macos": "--test_env=DONNER_CI_MACOS_LANE=hosted",
                 "macos-self-hosted": "--test_env=DONNER_CI_MACOS_LANE=self-hosted",
                 "macos-hosted-parity": "--test_env=DONNER_CI_MACOS_LANE=hosted-parity"}
# The main.yml lanes that must compute identical action keys. Every macOS job, in any workflow,
# selects the pinned Xcode and fails without it.
LANES = (HOSTED, SELF_HOSTED, PARITY)
# The toolchain repository refresh runs before any build and takes no build flags.
TOOLCHAIN_REFRESH = "fetch --force --lockfile_mode=off"
SELECT_SCRIPT = "tools/ci_select_macos_xcode.sh"
PIN_FILE = "tools/ci_macos_xcode.txt"
# The GitHub-hosted image that ships the pinned Xcode. Every GitHub-hosted macOS job runs on it.
HOSTED_XCODE_IMAGE = "xcode-27"


def _path(path):
    return Path(runfiles.Create().Rlocation("donner/%s" % path))


def _read(path):
    return _path(path).read_text(encoding="utf-8")


def _jobs(workflow):
    """Top-level jobs of a workflow, by name, as their raw text."""
    body = workflow.split("\njobs:\n", 1)[1]
    jobs = {}
    for match in re.finditer(r"(?m)^  ([A-Za-z0-9_-]+):\n((?:(?:    .*|[ \t]*)\n)*)", body):
        jobs[match.group(1)] = match.group(2)
    return jobs


def _job_env(job, name):
    match = re.search(r'(?m)^      %s: "([^"]*)"$' % re.escape(name), job)
    return match.group(1) if match else None


def _workflow_env(workflow, name):
    match = re.search(r'(?m)^  %s: "([^"]*)"$' % re.escape(name),
                      workflow.split("\njobs:\n", 1)[0])
    return match.group(1) if match else None


def _steps(job):
    """(name, text) for each step, in order."""
    steps_text = job.split("\n    steps:\n", 1)[1]
    chunks = re.split(r"(?m)^      - ", steps_text)[1:]
    steps = []
    for chunk in chunks:
        name = re.search(r"(?m)^(?:name: |        name: )(.+)$", chunk)
        steps.append((name.group(1).strip() if name else "", chunk))
    return steps


def _logical_commands(step):
    """Shell commands of a step, with backslash continuations and folded blocks joined."""
    commands = []
    folded = re.search(r"(?ms)^ *command: >-\n(.*)", step)
    if folded:
        commands.append(" ".join(line.strip() for line in folded.group(1).splitlines()
                                 if line.strip() and not line.strip().startswith("#")))
        return commands
    run = re.search(r"(?ms)^ *run: (\|\n.*|.*?$)", step)
    if not run:
        return commands
    current = ""
    for line in run.group(1).splitlines():
        line = line.strip()
        if line in ("|", "") or line.startswith("#"):
            continue
        if line.endswith("\\"):
            current += line[:-1] + " "
            continue
        commands.append(current + line)
        current = ""
    if current:
        commands.append(current)
    return commands


def _bazel_commands(job):
    """(step name, command) for each Bazel invocation in a job."""
    return [(name, command) for name, step in _steps(job) for command in _logical_commands(step)
            if BAZEL_INVOCATION.search(command)]


def neutral_configs(bazelrc):
    """Repository configs whose flags, through any configs they name, are all test-only or
    scheduling-only, so passing one never changes a build action's key."""
    flags = {}
    for line in bazelrc.splitlines():
        match = _RC_LINE.match(line)
        if match:
            flags.setdefault(match.group(1), []).extend(shlex.split(match.group(2)))

    def neutral(name, seen):
        return name in flags and name not in seen and all(
            neutral(flag.split("=", 1)[1], seen | {name}) if flag.startswith("--config=")
            else TEST_ONLY_FLAG.match(flag) or SCHEDULING_ONLY_FLAG.match(flag)
            for flag in flags[name])

    return frozenset(name for name in flags if neutral(name, frozenset()))


def _key_affecting(word, neutral):
    if word.startswith("--config=") and word.split("=", 1)[1] in neutral:
        return False
    return not TEST_ONLY_FLAG.match(word) and not SCHEDULING_ONLY_FLAG.match(word)


def _build_flags(command, neutral=frozenset()):
    """The key-affecting words of a Bazel command: build flags and environment lists after the
    command, without test-only flags, test-only lists, test-only configs or targets."""
    words = command[BAZEL_INVOCATION.search(command).end():].split()
    test_lists = {"$" + name for name in TEST_ONLY_VARIABLES}
    return sorted(word for word in words if (word.startswith("--") and
                                             _key_affecting(word, neutral)) or
                  (word.startswith("$") and word not in test_lists and
                   word.startswith("$BAZEL")))


def _runs_self_hosted(job):
    match = re.search(r"(?m)^    runs-on: (.+)$", job)
    return bool(match) and "self-hosted" in match.group(1)


def _flag_problems(hosted_flags, self_flags):
    """Problems of the two lanes' BAZEL_MACOS_BUILD_FLAGS."""
    problems = []
    if shlex.split(hosted_flags) != shlex.split(self_flags):
        problems.append("key-affecting BAZEL_MACOS_BUILD_FLAGS differ: %s vs %s" % (
            shlex.split(hosted_flags), shlex.split(self_flags)))
    if [flag for flag in shlex.split(hosted_flags + " " + self_flags) if REMOTE_FLAG.match(flag)]:
        problems.append("a macOS lane sets remote cache flags outside BuildBuddy")
    if "--config=ci-macos" not in shlex.split(hosted_flags):
        problems.append("the macOS lanes do not pass --config=ci-macos")
    return problems


def _flag_list_problems(owner, values, pattern, kind):
    """Problems of flag lists (name to value or None) that may hold only `kind` flags."""
    return ["%s %s holds a flag that is not %s" % (owner, variable, kind)
            for variable, value in values.items()
            if value is not None and not all(pattern.match(flag) for flag in shlex.split(value))]


def _test_list_problems(name, job):
    """Problems of a lane's test-only environment lists and its lane name."""
    problems = _flag_list_problems(
        name, {variable: _job_env(job, variable) for variable in TEST_ONLY_VARIABLES},
        TEST_ONLY_FLAG, "test-only")
    tests = shlex.split(_job_env(job, "BAZEL_MACOS_TEST_FLAGS") or "")
    if LANE_TEST_ENV[name] not in tests:
        problems.append("%s does not name its lane with %s" % (name, LANE_TEST_ENV[name]))
    return problems


def _record_commands(name, commands, by_step, neutral=frozenset()):
    """Record each build command's configs by step; problems for commands without the lane flags."""
    problems = []
    for step_name, command in commands:
        if (TOOLCHAIN_REFRESH in command or
                all(match.group(1) == "query" for match in BAZEL_INVOCATION.finditer(command))):
            continue
        if LANE_FLAGS not in command:
            problems.append("%s step %r runs Bazel without %s" % (name, step_name, LANE_FLAGS))
        by_step.setdefault(step_name, {}).setdefault(name, set()).add(
            tuple(_build_flags(command, neutral)))
    return problems


def _select_problems(name, steps, commands, step_id=True):
    """Problems of a job's pinned-Xcode selection and where it runs."""
    names = [step_name for step_name, _ in steps]
    if "Select pinned Xcode" not in names:
        return ["%s does not select the pinned Xcode" % name]
    problems = []
    select_index = names.index("Select pinned Xcode")
    select = steps[select_index][1]
    if (_logical_commands(select) != [SELECT_SCRIPT] or
            re.search(r"(?m)^\s*continue-on-error:", select) or
            (step_id and "id: xcode" not in select)):
        problems.append("%s selects Xcode other than with %s" % (name, SELECT_SCRIPT))
    first_bazel = min((names.index(step_name) for step_name, _ in commands), default=len(names))
    if first_bazel < select_index:
        problems.append("%s runs Bazel before selecting the pinned Xcode" % name)
    return problems


def _refresh_problems(name, commands, by_step):
    """Problems of a lane's toolchain repository refresh, which must run exactly once."""
    refresh = [command for _, command in commands if TOOLCHAIN_REFRESH in command]
    if len(refresh) != 1:
        return ["%s does not refresh the Xcode toolchain repositories once" % name]
    by_step.setdefault("<toolchain refresh>", {}).setdefault(name, set()).add(
        tuple(sorted(re.findall(r"--repo=(\S+)", refresh[0]))))
    return []


def _bazelrc_problems(bazelrc):
    """Problems of the ci-macos config, which must pin one PATH for actions and tools."""
    rc = [line.split(None, 1)[1] for line in bazelrc.splitlines()
          if re.match(r"^(?:build|common|test):ci-macos\s", line)]
    paths = [re.fullmatch(r"--(action_env|host_action_env)=PATH=(\S+)", line) for line in rc]
    if (len(rc) != 2 or not all(paths) or
            {match.group(1) for match in paths} != {"action_env", "host_action_env"} or
            paths[0].group(2) != paths[1].group(2)):
        return ["--config=ci-macos must pin exactly one PATH for actions and tools"]
    if "/Users/" in paths[0].group(2) or "$" in paths[0].group(2):
        return ["the pinned action PATH depends on a runner's home or environment"]
    return []


# Test hooks of tools/ci_select_macos_xcode.sh, and the variable it sets itself. A workflow
# that sets any of them (job or workflow env, or a GITHUB_ENV write) can silently replace the
# pinned Xcode, so no macOS job and no workflow-level env may name them.
XCODE_OVERRIDES = re.compile(r"\b(?:XCODEBUILD|XCODE_APPLICATIONS_DIR|XCODE_LOCATOR|DEVELOPER_DIR)\b")
MACOS_JOBS = (HOSTED, SELF_HOSTED, PARITY, "macos-browser")


def _xcode_override_problems(workflow):
    """Problems where a workflow could override the pinned Xcode selection."""
    problems = []
    if XCODE_OVERRIDES.search(workflow.split("\njobs:\n", 1)[0]):
        problems.append("main.yml sets an Xcode selection override outside a job")
    jobs = _jobs(workflow)
    for name in MACOS_JOBS:
        if name in jobs and XCODE_OVERRIDES.search(jobs[name]):
            problems.append("%s sets an Xcode selection override" % name)
    return problems


# A Starlark action that runs a host tool by absolute path depends on the host OS, which is not in
# its action key or in the macOS cache partition, so its output must stay out of the remote cache.
HOST_TOOL = re.compile(r"(?<![!\w])/usr/bin/(?!env\b)[A-Za-z]")


def host_tool_problems(files):
    """Starlark files that run a host tool by path without keeping the action out of the cache."""
    return ["%s runs a host tool without no-remote-cache" % path
            for path, text in sorted(files.items())
            if HOST_TOOL.search(text) and '"no-remote-cache"' not in text]


def key_parity_problems(workflow, bazelrc):
    """Every way the two macOS lanes in main.yml could stop computing identical action keys."""
    jobs = _jobs(workflow)
    lane_flags = {name: _job_env(jobs[name], "BAZEL_MACOS_BUILD_FLAGS") for name in LANES}
    if None in lane_flags.values():
        return ["a macOS lane does not declare BAZEL_MACOS_BUILD_FLAGS"]
    problems = _xcode_override_problems(workflow)
    for name in LANES[1:]:
        problems += _flag_problems(lane_flags[HOSTED], lane_flags[name])
    problems += _flag_list_problems(
        "main.yml", {variable: _workflow_env(workflow, variable)
                     for variable in TEST_ONLY_VARIABLES}, TEST_ONLY_FLAG, "test-only")
    neutral = neutral_configs(bazelrc)
    by_step = {}
    for name in LANES:
        job = jobs[name]
        commands = _bazel_commands(job)
        problems += _record_commands(name, commands, by_step, neutral)
        problems += _test_list_problems(name, job)
        problems += _select_problems(name, _steps(job), commands)
        if "Select pinned Xcode" in [step_name for step_name, _ in _steps(job)]:
            problems += _refresh_problems(name, commands, by_step)
    return problems + _step_divergence(by_step) + _bazelrc_problems(bazelrc)


def _step_divergence(by_step):
    """Steps whose key-affecting flags differ from the hosted lane's copy of the step."""
    return ["step %r differs between the lanes: %s" % (step_name, lanes_seen)
            for step_name, lanes_seen in sorted(by_step.items())
            if HOSTED in lanes_seen
            and any(flags != lanes_seen[HOSTED] for flags in lanes_seen.values())]


def private_backend_problems(files):
    """GitHub-hosted jobs, and the actions they use, never reach the private cache or executor."""
    problems = []
    private = re.compile(r"BAZEL_REMOTE_CACHE|--config[= ]re\b|\bbuild:re\b")
    for path, text in sorted(files.items()):
        if path.startswith(".github/actions/"):
            if private.search(text):
                problems.append("%s references the private remote cache or executor" % path)
            continue
        head = text.split("\njobs:\n", 1)[0]
        if private.search(head):
            problems.append("%s references the private backend outside a job" % path)
        for name, job in _jobs(text).items():
            if not _runs_self_hosted(job) and private.search(job):
                problems.append("%s:%s is GitHub-hosted and references the private remote "
                                "cache or executor" % (path, name))
    return problems


# Reviewed `runs-on` expressions that name a macOS label no job run can select.
UNREACHABLE_MACOS_RUNNERS = {
    (".github/workflows/codeql.yml", "analyze"):
        "Only a `swift` matrix entry would run on macOS, and the matrix has none.",
}
SELECT_CONDITIONS = ("", "runner.os == 'macOS'")


def _runner_labels(job):
    """(labels, self_hosted) for a job's runner: literal labels, or the literal values of the
    matrix key or expression branches its `runs-on` names."""
    runs_on = re.search(r"(?m)^    runs-on: (.+)$", job)
    if not runs_on:
        return set(), False
    value = runs_on.group(1).strip()
    matrix = re.fullmatch(r"\$\{\{ matrix\.([a-z_]+) \}\}", value)
    if matrix:
        key = re.escape(matrix.group(1))
        labels = set()
        for listed in re.findall(r"(?m)^ +%s: \[([^\]]*)\]$" % key, job):
            labels.update(label.strip() for label in listed.split(","))
        labels.update(re.findall(r"(?m)^ +- %s: ([A-Za-z0-9.-]+)$" % key, job))
        return labels, False
    if value.startswith("${{"):
        return set(re.findall(r"'([A-Za-z0-9.-]+)'", value)), False
    labels = {label.strip() for label in value.strip("[]").split(",")}
    return labels, "self-hosted" in labels


def _is_macos(label):
    return label == "macOS" or label.startswith(("macos", "xcode-"))


def _macos_jobs(files):
    """(owner, job, self-hosted, macOS labels) for every workflow job that can run on macOS."""
    jobs = []
    for path, text in sorted(files.items()):
        if not path.startswith(".github/workflows/"):
            continue
        for name, job in _jobs(text).items():
            labels, self_hosted = _runner_labels(job)
            macos = {label for label in labels if _is_macos(label)}
            if macos and (path, name) not in UNREACHABLE_MACOS_RUNNERS:
                jobs.append(("%s:%s" % (path, name), job, self_hosted, macos))
    return jobs


def _select_order_problems(owner, steps):
    """Problems of when and under which condition a job selects the pinned Xcode."""
    names = [step_name for step_name, _ in steps]
    if "Select pinned Xcode" not in names:
        return []
    select_index = names.index("Select pinned Xcode")
    problems = []
    condition = re.search(r"(?m)^\s*if: (.+)$", steps[select_index][1])
    if (condition.group(1).strip() if condition else "") not in SELECT_CONDITIONS:
        problems.append("%s selects the pinned Xcode only conditionally" % owner)
    problems += ["%s runs %r before selecting the pinned Xcode" % (
        owner, step_name or step.splitlines()[0]) for step_name, step in steps[:select_index]
        if "uses: actions/checkout@" not in step and step_name != "Check runner disk space"]
    return problems


def macos_job_problems(files):
    """Every macOS job selects the pinned Xcode before it builds, and every GitHub-hosted one runs
    on the image that ships it."""
    problems = []
    for owner, job, self_hosted, macos in _macos_jobs(files):
        if not self_hosted and macos != {HOSTED_XCODE_IMAGE}:
            problems.append("%s runs on %s, not the %s image that ships the pinned Xcode" % (
                owner, sorted(macos), HOSTED_XCODE_IMAGE))
        steps = _steps(job)
        problems += _select_problems(owner, steps, _bazel_commands(job),
                                     step_id="steps.xcode.outputs" in job)
        problems += _select_order_problems(owner, steps)
    return problems


def _workflow_files():
    main = _path(".github/workflows/main.yml")
    github = main.parent.parent
    paths = sorted((github / "workflows").glob("*.y*ml"))
    paths.extend(sorted((github / "actions").glob("*/action.y*ml")))
    return {str(path.relative_to(github.parent)): path.read_text(encoding="utf-8")
            for path in paths}


class MacosCacheKeyParityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = _read(".github/workflows/main.yml")
        cls.bazelrc = _read(".bazelrc")

    def test_reader_understands_both_macos_lanes(self):
        jobs = _jobs(self.workflow)
        for name in (HOSTED, SELF_HOSTED):
            steps = [step_name for step_name, _ in _steps(jobs[name])]
            self.assertIn("Build", steps)
            self.assertIn("Test (Geode editor lane)", steps)
            self.assertGreaterEqual(len(_bazel_commands(jobs[name])), 6)
        self.assertFalse(_runs_self_hosted(jobs[HOSTED]))
        self.assertTrue(_runs_self_hosted(jobs[SELF_HOSTED]))

    def test_macos_lanes_pass_identical_key_affecting_flags(self):
        self.assertEqual([], key_parity_problems(self.workflow, self.bazelrc))

    def test_only_unconfigured_queries_are_exempt_from_build_flags(self):
        recorded = {}
        self.assertEqual([], _record_commands(HOSTED, [
            ("Selection", "bazelisk --nohome_rc --nosystem_rc query //...")], recorded))
        self.assertEqual({}, recorded)
        for operation in ("build", "test", "cquery"):
            with self.subTest(operation=operation):
                for command in ("bazelisk %s //..." % operation,
                                "bazelisk query //... && bazelisk %s //..." % operation):
                    problems = _record_commands(HOSTED, [("Configured", command)], {})
                    self.assertEqual(1, len(problems), problems)
                    self.assertIn("runs Bazel without", problems[0])

    def test_key_parity_policy_rejects_divergence(self):
        hosted_flags = 'BAZEL_MACOS_BUILD_FLAGS: "--config=ci-macos --macos_minimum_os=13.3"\n'
        # Every macOS lane carries the same line; split at the self-hosted job's.
        start = self.workflow.index("\n  %s:\n" % SELF_HOSTED)
        index = self.workflow.index(hosted_flags, start)
        before, after = self.workflow[:index], self.workflow[index + len(hosted_flags):]
        rc_line = "build:ci-macos --host_action_env=PATH="
        cases = {
            "self-hosted drops the config": (
                before + 'BAZEL_MACOS_BUILD_FLAGS: "--macos_minimum_os=13.3"\n' + after,
                self.bazelrc, "key-affecting BAZEL_MACOS_BUILD_FLAGS differ"),
            "hosted adds a key-affecting flag": (self.workflow.replace(
                hosted_flags, hosted_flags.replace("13.3", "13.3 --copt=-O1"), 1),
                self.bazelrc, "key-affecting BAZEL_MACOS_BUILD_FLAGS differ"),
            "hosted carries a private cache flag": (self.workflow.replace(
                hosted_flags, hosted_flags.replace("13.3", "13.3 --remote_timeout=5s"), 1),
                self.bazelrc, "remote cache flags outside BuildBuddy"),
            "self-hosted adds a remote executor": (
                before + hosted_flags.replace("13.3", "13.3 --remote_executor=grpcs://x") + after,
                self.bazelrc, "remote cache flags outside BuildBuddy"),
            "a step adds an inline build flag on one lane": (self.workflow.replace(
                "bazelisk build --config=ci $BAZEL_MACOS_BUILD_FLAGS $TARGETS",
                "bazelisk build --config=ci --copt=-O0 $BAZEL_MACOS_BUILD_FLAGS $TARGETS", 1),
                self.bazelrc, "step 'Build' differs"),
            "a plain bazel step drops the lane flags": (self.workflow.replace(
                "bazelisk build --config=ci $BAZEL_MACOS_BUILD_FLAGS $TARGETS",
                "bazel build --config=ci $TARGETS", 1), self.bazelrc, "runs Bazel without"),
            "a build flag hidden in the test list": (self.workflow.replace(
                'BAZEL_MACOS_TEST_FLAGS: "--test_env=HOME',
                'BAZEL_MACOS_TEST_FLAGS: "--copt=-O0 --test_env=HOME', 1),
                self.bazelrc, "not test-only"),
            "self-hosted job env overrides xcodebuild": (
                before + hosted_flags + "      XCODEBUILD: /usr/bin/true\n" + after,
                self.bazelrc, "macos-self-hosted sets an Xcode selection override"),
            "self-hosted job env overrides the locator": (
                before + hosted_flags + "      XCODE_LOCATOR: /usr/bin/true\n" + after,
                self.bazelrc, "macos-self-hosted sets an Xcode selection override"),
            "an earlier step redirects the Xcode lookup": (
                before + hosted_flags + after.replace(
                    "      - name: Checkout\n",
                    "      - name: Redirect\n"
                    "        run: echo XCODE_APPLICATIONS_DIR=/x >> \"$GITHUB_ENV\"\n\n"
                    "      - name: Checkout\n", 1),
                self.bazelrc, "macos-self-hosted sets an Xcode selection override"),
            "a lane shares its test results": (self.workflow.replace(
                LANE_TEST_ENV["macos-self-hosted"], "--test_env=HOME", 1),
                self.bazelrc, "does not name its lane"),
            "the parity lane shares the hosted lane's test results": (self.workflow.replace(
                LANE_TEST_ENV[PARITY], LANE_TEST_ENV[HOSTED], 1),
                self.bazelrc, "does not name its lane"),
            "the parity configuration hides a build flag": (self.workflow, self.bazelrc.replace(
                "build:hosted-macos --jobs=3\n", "build:hosted-macos --jobs=3 --copt=-O0\n", 1),
                "step 'Test (Geode editor lane)' differs"),
            "the parity lane changes a key-affecting flag": (self.workflow.replace(
                "--config=geode --config=hosted-parity-runtime \\\n",
                "--config=geode --config=hosted-parity-runtime --copt=-O0 \\\n", 1),
                self.bazelrc, "step 'Test (Geode editor lane)' differs"),
            "the parity lane drops the Xcode pin": (self.workflow[:self.workflow.index(
                "\n  %s:\n" % PARITY)] + self.workflow[self.workflow.index(
                    "\n  %s:\n" % PARITY):].replace(
                    "run: tools/ci_select_macos_xcode.sh\n", "run: echo skipped\n",
                    1), self.bazelrc, "macos-hosted-parity selects Xcode other than"),
            "a step drops the lane flags": (self.workflow.replace(
                "bazelisk build --config=ci $BAZEL_MACOS_BUILD_FLAGS $TARGETS",
                "bazelisk build --config=ci $TARGETS", 1),
                self.bazelrc, "runs Bazel without"),
            "a step adds a config on one lane": (self.workflow.replace(
                "bazelisk build --config=ci $BAZEL_MACOS_BUILD_FLAGS $TARGETS",
                "bazelisk build --config=ci --config=geode $BAZEL_MACOS_BUILD_FLAGS $TARGETS", 1),
                self.bazelrc, "step 'Build' differs"),
            "hosted skips the Xcode pin": (self.workflow.replace(
                "run: tools/ci_select_macos_xcode.sh\n",
                "run: echo skipped\n", 1), self.bazelrc, "macos selects Xcode other than"),
            "self-hosted Xcode selection removed": (before + hosted_flags + after.replace(
                "      - name: Select pinned Xcode\n        id: xcode\n        shell: bash\n"
                "        run: tools/ci_select_macos_xcode.sh\n", "", 1),
                self.bazelrc, "macos-self-hosted does not select the pinned Xcode"),
            "self-hosted makes the pin optional again": (before + hosted_flags + after.replace(
                "run: tools/ci_select_macos_xcode.sh\n",
                "run: tools/ci_select_macos_xcode.sh --optional\n", 1),
                self.bazelrc, "macos-self-hosted selects Xcode other than"),
            "self-hosted tolerates a failed selection": (before + hosted_flags + after.replace(
                "        run: tools/ci_select_macos_xcode.sh\n",
                "        continue-on-error: true\n        run: tools/ci_select_macos_xcode.sh\n",
                1), self.bazelrc, "macos-self-hosted selects Xcode other than"),
            "unpinned host tool PATH": (self.workflow, self.bazelrc.replace(
                rc_line, "build:ci-macos --host_action_env=PATH=/usr/bin:", 1),
                "pin exactly one PATH"),
            "PATH from a runner home": (self.workflow, re.sub(
                r"(?m)^(build:ci-macos --(?:host_)?action_env=PATH=)", r"\g<1>/Users/runner/bin:",
                self.bazelrc), "runner's home"),
        }
        for name, (workflow, bazelrc, expected) in cases.items():
            with self.subTest(case=name):
                self.assertNotEqual((workflow, bazelrc), (self.workflow, self.bazelrc))
                problems = key_parity_problems(workflow, bazelrc)
                self.assertTrue(any(expected in problem for problem in problems), problems)

    def test_host_tool_actions_stay_out_of_the_remote_cache(self):
        bundle = _read("build_defs/macos_app_bundle.bzl")
        self.assertEqual([], host_tool_problems({"build_defs/macos_app_bundle.bzl": bundle}))
        self.assertIn("/usr/bin/codesign", bundle)
        unguarded = bundle.replace('execution_requirements = {"no-remote-cache": "1"},', "")
        self.assertNotEqual(unguarded, bundle)
        self.assertEqual(["build_defs/macos_app_bundle.bzl runs a host tool without "
                          "no-remote-cache"],
                         host_tool_problems({"build_defs/macos_app_bundle.bzl": unguarded}))
        self.assertEqual([], host_tool_problems({"x.bzl": "#!/usr/bin/env bash\n"}))

    def test_github_hosted_jobs_never_reach_the_private_backend(self):
        files = _workflow_files()
        self.assertEqual([], private_backend_problems(files))
        main = ".github/workflows/main.yml"
        hosted_step = ("      - name: Fetch Metal validation dependencies\n"
                       "        env:\n          URL: ${{ secrets.BAZEL_REMOTE_CACHE }}\n")
        cases = {
            "hosted secret": files[main].replace(
                "      - name: Fetch Metal validation dependencies\n", hosted_step, 1),
            "hosted remote execution": files[main].replace(
                "bazelisk build --config=ci $BAZEL_MACOS_BUILD_FLAGS $TARGETS",
                "bazelisk build --config=ci --config=re $BAZEL_MACOS_BUILD_FLAGS $TARGETS", 1),
            "workflow-level secret": files[main].replace(
                "\njobs:\n", "\n  CACHE: ${{ secrets.BAZEL_REMOTE_CACHE }}\njobs:\n", 1),
        }
        for name, text in cases.items():
            with self.subTest(case=name):
                self.assertNotEqual(text, files[main])
                self.assertTrue(private_backend_problems({**files, main: text}))

    def test_every_macos_job_selects_the_pin_on_the_image_that_ships_it(self):
        files = _workflow_files()
        self.assertEqual([], macos_job_problems(files))
        # Each reviewed exception still names a macOS label, and its reason still holds.
        for path, name in UNREACHABLE_MACOS_RUNNERS:
            labels, _ = _runner_labels(_jobs(files[path])[name])
            self.assertTrue(any(_is_macos(label) for label in labels), (path, name))
        codeql = _jobs(files[".github/workflows/codeql.yml"])["analyze"]
        self.assertEqual(set(re.findall(r"(?m)^\s*-?\s*language:\s*['\"]?([A-Za-z-]+)", codeql)),
                         {"c-cpp", "javascript-typescript", "python"})
        self.assertEqual({owner for owner, *_ in _macos_jobs(files)}, {
            ".github/workflows/" + job for job in (
                "main.yml:" + HOSTED, "main.yml:macos-browser", "main.yml:" + SELF_HOSTED,
                "main.yml:" + PARITY, "cmake.yml:macos", "editor_wasm.yml:build",
                "bcr_preflight.yml:consumer", "bcr_preflight.yml:cli-macos", "fuzz.yml:macos",
                "perf.yml:perf")})
        cmake, wasm = ".github/workflows/cmake.yml", ".github/workflows/editor_wasm.yml"
        fuzz, bcr = ".github/workflows/fuzz.yml", ".github/workflows/bcr_preflight.yml"
        perf = ".github/workflows/perf.yml"
        split = files[fuzz].index("\n  macos:\n")
        fuzz_head, fuzz_macos = files[fuzz][:split], files[fuzz][split:]
        select = ("      - name: Select pinned Xcode\n        shell: bash\n"
                  "        run: tools/ci_select_macos_xcode.sh\n")
        cases = {
            "the previous hosted image": (cmake, files[cmake].replace(
                "runs-on: xcode-27", "runs-on: macos-26", 1), "not the xcode-27 image"),
            "a matrix entry on another image": (perf, files[perf].replace(
                "- os: xcode-27", "- os: macos-latest", 1), "not the xcode-27 image"),
            "no selection": (wasm, files[wasm].replace(select, "", 1), "does not select"),
            "selection after a build tool": (fuzz, fuzz_head + fuzz_macos.replace(
                select, "", 1).replace("      - name: Soak", select + "\n      - name: Soak", 1),
                "runs 'Setup Bazel' before selecting"),
            "selection that can be skipped": (bcr, files[bcr].replace(
                "if: runner.os == 'macOS'", "if: github.event_name == 'push'", 1),
                "only conditionally"),
            "selection with an argument": (cmake, files[cmake].replace(
                "run: tools/ci_select_macos_xcode.sh\n",
                "run: tools/ci_select_macos_xcode.sh --optional\n", 1),
                "selects Xcode other than"),
        }
        for name, (path, text, expected) in cases.items():
            with self.subTest(case=name):
                self.assertNotEqual(text, files[path])
                problems = macos_job_problems({**files, path: text})
                self.assertTrue(any(expected in problem for problem in problems), problems)

    def test_pin_names_one_hosted_xcode_version_and_build(self):
        lines = [line for line in _read(PIN_FILE).splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        self.assertEqual(1, len(lines))
        self.assertRegex(lines[0], r"^\d+\.\d+(?:\.\d+)? \d+[A-Z]\d+[a-z]?$")

    def _select(self, discovered=("{root}/Xcode-any-name.app/Contents/Developer",),
                reported="Xcode {version}\nBuild version {build}", locator_status=0,
                missing_xcrun=False, args=(), runner="github-hosted"):
        """Runs the selection script with a fake LaunchServices probe and xcodebuild.

        `discovered` lists the developer directories the probe reports for the pinned version;
        `locator_status=None` makes the script build the real probe with a failing fake xcrun.
        """
        version, build = [line for line in _read(PIN_FILE).splitlines()
                          if line.strip() and not line.startswith("#")][0].split()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def fill(text):
                return text.format(root=root, version=version, build=build)

            xcodebuild = root / "xcodebuild"
            # xcodebuild can print unrelated warnings on stderr; only stdout is the version.
            xcodebuild.write_text("#!/bin/sh\necho 'objc[1]: simulated noise' >&2\n"
                                  "printf '%%s' '%s'\n" % fill(reported), encoding="utf-8")
            xcodebuild.chmod(0o755)
            locator = root / "xcode-locator"
            locator.write_text("#!/bin/sh\nprintf '%s'\nexit %d\n" % (
                "".join(fill(line) + "\\n" for line in discovered), locator_status or 0),
                encoding="utf-8")
            locator.chmod(0o755)
            xcrun = root / "xcrun"
            if not missing_xcrun:
                xcrun.write_text("#!/bin/sh\necho simulated-sdk-failure >&2\nexit 1\n",
                                 encoding="utf-8")
                xcrun.chmod(0o755)
            env_file, output_file = root / "env", root / "output"
            env_file.write_text("")
            output_file.write_text("")
            result = subprocess.run(
                ["/bin/bash", str(_path(SELECT_SCRIPT)), *args],
                # The test's own PATH: a fixed /usr/bin:/bin lacks coreutils on hosts that
                # keep them elsewhere, such as NixOS remote-execution workers.
                env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "GITHUB_ENV": str(env_file),
                     "GITHUB_OUTPUT": str(output_file), "RUNNER_ARCH": "ARM64",
                     "BAZEL_MACOS_BUILD_FLAGS": "--config=ci-macos", "XCODEBUILD": str(xcodebuild),
                     "XCODE_LOCATOR": str(locator) if locator_status is not None else "",
                     "XCRUN": str(xcrun), "XCODE_PROBE_PYTHON": sys.executable,
                     "RUNNER_ENVIRONMENT": runner},
                capture_output=True, text=True, timeout=test_timing.STALL_GUARD_SECONDS,
                check=False)
            return (result.returncode, env_file.read_text(), output_file.read_text(),
                    result.stdout + result.stderr, version, build, root)

    def test_select_script_exports_the_one_discovered_pinned_xcode(self):
        code, env, output, log, version, build, root = self._select()
        self.assertEqual(0, code, log)
        self.assertEqual(env.splitlines(), [
            "DEVELOPER_DIR=%s/Xcode-any-name.app/Contents/Developer" % root,
            "BAZEL_MACOS_BUILD_FLAGS=--config=ci-macos --xcode_version=%s" % version])
        self.assertEqual(output, "cache_partition=macos-ARM64-xcode-%s-%s\n" % (version, build))
        self.assertIn("Selected the pinned Xcode %s (%s)." % (version, build), log)
        self.assertNotIn("simulated noise", log)

    def test_select_script_fails_without_exactly_the_pinned_xcode(self):
        cases = {
            "not discoverable": (dict(discovered=()), "LaunchServices lists no Xcode"),
            "ambiguous": (dict(discovered=("{root}/A.app/Contents/Developer",
                                           "{root}/B.app/Contents/Developer")),
                          "is ambiguous: 2 installations"),
            "other build": (dict(reported="Xcode {version}\nBuild version 99Z999"),
                            "is not the Xcode that Bazel discovers"),
            "other version": (dict(reported="Xcode {version}.1\nBuild version {build}"),
                              "is not the Xcode that Bazel discovers"),
            "probe failure": (dict(locator_status=2), "probe failed (exit 2)"),
        }
        for name, (arguments, expected) in cases.items():
            with self.subTest(case=name):
                code, env, output, log, *_ = self._select(**arguments)
                self.assertEqual(1, code, log)
                self.assertEqual("", env)
                self.assertEqual("", output)
                self.assertIn("::error::", log)
                self.assertIn(expected, log)
        # A mismatch names what the runner has, so the failing job is diagnosable.
        *_, log, _, _, _ = self._select(reported="Xcode {version}\nBuild version 99Z999")
        self.assertIn("Build version 99Z999", log)
        self.assertIn("simulated noise", log)
        # A self-hosted runner's paths never reach its public log.
        for arguments in (dict(reported="Xcode {version}\nBuild version 99Z999"),
                          dict(discovered=("{root}/A.app/Contents/Developer",
                                           "{root}/B.app/Contents/Developer"))):
            with self.subTest(runner="self-hosted", **{k: str(v) for k, v in arguments.items()}):
                code, _, _, log, _, _, root = self._select(runner="self-hosted", **arguments)
                self.assertEqual(1, code, log)
                self.assertNotIn(str(root), log)

    def test_probe_sdk_failure_keeps_diagnostics_before_scratch_cleanup(self):
        code, env, output, log, *_ = self._select(locator_status=None)
        self.assertEqual(1, code, log)
        self.assertEqual(("", ""), (env, output))
        self.assertRegex(log, r"Xcode discovery probe diagnostics \(sdk.log\):\n[^\n]+")
        self.assertIn("simulated-sdk-failure", log)
        self.assertIn("could not be built", log)

    def test_probe_exec_failure_is_diagnosed(self):
        code, env, output, log, *_ = self._select(locator_status=None, missing_xcrun=True)
        self.assertEqual(1, code, log)
        self.assertEqual(("", ""), (env, output))
        self.assertIn("Cannot execute", log)
        self.assertIn("Xcode discovery probe diagnostics (sdk.log):", log)

    def test_select_script_takes_no_mode(self):
        for args in (("--optional",), ("--required",)):
            with self.subTest(args=args):
                code, env, output, log, *_ = self._select(args=args)
                self.assertEqual(2, code, log)
                self.assertEqual(("", ""), (env, output))

if __name__ == "__main__":
    unittest.main()
