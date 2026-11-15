"""Keeps the GitHub-hosted and self-hosted macOS lanes on identical Bazel action keys.

The two lanes in main.yml can share one remote cache only if every key-affecting input matches:
the Bazel flags and configs, the action environment (PATH in particular, which Bazel copies from
the client unless it is pinned), and the Xcode version and build, which Apple toolchain actions
carry in their environment. Both lanes configure the remote cache only through the BuildBuddy
action, so their build flags must be identical. Test-only flags and environment (--test_env)
are deliberately left to each lane, and each lane names itself in its test environment: a test
result depends on the runner's GPU, so it must not be shared across runners. Within a lane, no
action or test environment may be copied from the runner: runners of one lane can have
different home directories, and a copied value would keep each from reusing another's results.

Every GitHub-hosted job is also kept off any private remote cache and remote
execution configuration.
"""

from pathlib import Path
import os
import plistlib
import re
import shlex
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

from python.runfiles import runfiles
from tools import bazelrc_configs
from tools import test_timing
from tools.ci import lane
from tools.ci import lane_view
from tools.ci import lanes


HOSTED = "macos"
SELF_HOSTED = "macos-self-hosted"
# Remote cache and executor flags. The macOS lanes get their remote cache only from the
# BuildBuddy action, so neither passes any of these itself.
REMOTE_FLAG = re.compile(r"^--(?:no)?(?:remote_|incompatible_remote_|credential_helper)")
# Every macOS lane passes the lane table's key-affecting flags (lanes.MACOS_BUILD) together, ending
# with the flags the Xcode selection adds at run time. The workflow is read with each lane step's
# commands from its dry run, which prints those run-time flags as the variable that carries them.
RUNTIME_FLAGS = "BAZEL_MACOS_BUILD_FLAGS"
LANE_FLAGS = " ".join(flag if isinstance(flag, str) else "$" + flag.name
                      for flag in lanes.MACOS_BUILD)
BAZEL_INVOCATION = re.compile(
    r"\bbazel(?:isk)?(?:\s+--[a-z_]+)*\s+(build|test|fetch|query|cquery|coverage|run)\b")
# Flags that only select, run or report tests. They never change a build action's key. The Metal
# shader-validation setting qualifies because its only use selects the Metal device tests' `env`
# attribute: Bazel's visibility check keeps every other package from selecting on it
# (test_metal_shader_validation_setting_selects_only_test_environment), and an aquery of those
# tests' compile, archive and link actions gave identical action keys with it on and off.
TEST_ONLY_FLAG = re.compile(r"^--(?:no)?(?:test_|cache_test_results|local_test_jobs|"
                            r"flaky_test_attempts|runs_per_test|keep_going|build_event_json_file|"
                            r"run_under|//build_defs:metal_shader_validation=)")
# Flags that only limit how many actions Bazel runs at once, or which requested targets it
# builds. They never change a build action's key.
SCHEDULING_ONLY_FLAG = re.compile(r"^--(?:jobs|local_resources|build_tests_only)(?:=|$)")
# Job environment values a lane may set for itself; each may hold only test-only flags.
TEST_ONLY_VARIABLES = ("BAZEL_MACOS_TEST_FLAGS", "BROWSER_STALL_TEST_FLAGS")
_RC_LINE = re.compile(r"^(?:build|test|common):([A-Za-z0-9_-]+)\s+(.*)$")
# Each lane names itself in its test environment, so test results never cross runners.
LANE_TEST_ENV = {"macos-browser": "--test_env=DONNER_CI_MACOS_LANE=native-desktop-browser",
                 "macos": "--test_env=DONNER_CI_MACOS_LANE=hosted",
                 "macos-self-hosted": "--test_env=DONNER_CI_MACOS_LANE=self-hosted-vm",
                 "macos-metal": "--test_env=DONNER_CI_MACOS_LANE=native-metal",
                 "macos-vm-qualification": "--test_env=DONNER_CI_MACOS_LANE=vm-qualification"}
# A flag without a value copies the variable from the client's environment into every action or
# test key it applies to.
INHERITED_ENV_FLAG = re.compile(
    r"^--(?:test_env|action_env|host_action_env)=([A-Za-z_][A-Za-z0-9_]*)$")
# A value with `$` is expanded from the runner's shell or a workflow expression, so it can differ
# between runners just as an inherited value can.
EXPANDED_ENV_FLAG = re.compile(
    r"^--(?:test_env|action_env|host_action_env)=([A-Za-z_][A-Za-z0-9_]*)=.*\$")
# Bazel applies the `macos` config on every macOS client (--enable_platform_specific_config), so
# its flags reach every macOS lane without the lane naming it.
IMPLICIT_MACOS_CONFIG = "--config=macos"
# The only variables a macOS lane may copy from the runner, each with the same value on every
# runner of a lane.
RUNNER_INDEPENDENT_INHERITED = {
    # Exported by tools/ci_select_macos_xcode.sh from the pinned Xcode tuple it selects.
    "DONNER_CI_XCODE_IDENTITY",
    # Local opt-ins (.bazelrc); no CI job sets them, so they are absent from every CI key.
    "LLM",
    "DONNER_RENDERER_TEST_VERBOSE",
}
# --config=ci-macos sets HOME to an empty value, so every test key carries the same value. Bazel's
# test setup (tools/test/test-setup.sh) replaces a HOME that is not an absolute path with the
# test's own TEST_TMPDIR, so each test runs with a writable, private home.
CI_MACOS_HOME = "--test_env=HOME="
# --config=ci-macos also lets every main.yml macOS lane run up to four GPU-opening tests at a time
# (//build_defs:geode_test_scheduling_tests holds the reviewed reason). It replaces the
# unconditional local_gpu=1 for that key only and changes no action key.
MACOS_CI_LOCAL_GPU = "--local_resources=local_gpu=4"
_LOCAL_GPU_UNITS = re.compile(r"^--local_resources=local_gpu=(\S+)$")
# The main.yml lanes that must compute identical action keys. Every macOS job, in any workflow,
# selects the pinned Xcode and fails without it.
LANES = (HOSTED, SELF_HOSTED, "macos-metal", "macos-vm-qualification", "macos-browser")
# The toolchain repository refresh runs before any build and takes no build flags.
TOOLCHAIN_REFRESH = "fetch --force --lockfile_mode=off"
SELECT_SCRIPT = "tools/ci_select_macos_xcode.sh"
PIN_FILE = "tools/ci_macos_xcode.txt"
# The GitHub-hosted image that ships the pinned Xcode. Every GitHub-hosted macOS job runs on it.
HOSTED_XCODE_IMAGE = "xcode-27"


def lane_workflow(text):
    """A workflow's text with each lane step's commands (tools/ci/lane_view.py)."""
    return lane_view.with_dry_runs(text)


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


def _config_flags(bazelrc):
    """Flags of each named repository config, by config name."""
    flags = {}
    for line in bazelrc.splitlines():
        match = _RC_LINE.match(line)
        if match:
            flags.setdefault(match.group(1), []).extend(shlex.split(match.group(2)))
    return flags


def neutral_configs(bazelrc):
    """Repository configs whose flags, through any configs they name, are all test-only or
    scheduling-only, so passing one never changes a build action's key."""
    flags = _config_flags(bazelrc)

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


def _expanded_words(command, words, configs):
    return bazelrc_configs.expand(command, words, configs)


def _flag_problems(flags, commands, configs):
    """Problems of the lanes' shared key-affecting flags and of every macOS lane command, with
    the repository's configs expanded."""
    problems = []
    flags = " ".join(_expanded_words("build", flags.split(), configs))
    words = flags.split() + [word for _, command in commands
                             for word in _expanded_words("test", command.split(), configs)]
    if [flag for flag in words if REMOTE_FLAG.match(flag)]:
        problems.append("a macOS lane sets remote cache flags outside BuildBuddy")
    if not set(_expanded_words("build", ["--config=ci-macos"], configs)) <= set(flags.split()):
        problems.append("the macOS lanes do not pass --config=ci-macos")
    return problems


def _flag_list_problems(owner, values, pattern, kind):
    """Problems of flag lists (name to value or None) that may hold only `kind` flags."""
    return ["%s %s holds a flag that is not %s" % (owner, variable, kind)
            for variable, value in values.items()
            if value is not None and not all(pattern.match(flag) for flag in shlex.split(value))]


def _test_list_problems(name, commands, configs):
    """Problems of a lane's test-only environment lists (the lane table's, with their configs
    expanded), and of its test commands, each of which names the lane."""
    lists = {"MACOS_TEST[%r]" % name: " ".join(
        _expanded_words("test", list(lanes.MACOS_TEST.get(name, ())), configs))}
    if name == HOSTED:
        lists["BROWSER_STALL"] = " ".join(lanes.BROWSER_STALL)
    problems = _flag_list_problems(name, lists, TEST_ONLY_FLAG, "test-only")
    tests = [command for _, command in commands
             if [match.group(1) for match in BAZEL_INVOCATION.finditer(command)] == ["test"]]
    if not tests or any(LANE_TEST_ENV[name] not in _expanded_words("test", command.split(), configs)
                        for command in tests):
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
    """Problems of the ci-macos config, which must pin one PATH for actions and tools and give
    tests a HOME that does not depend on the runner."""
    rc = [line.split(None, 1)[1] for line in bazelrc.splitlines()
          if re.match(r"^(?:build|common|test):ci-macos\s", line)]
    problems = []
    if rc.count(CI_MACOS_HOME) != 1:
        problems.append("--config=ci-macos must set %s exactly once" % CI_MACOS_HOME)
    if rc.count(MACOS_CI_LOCAL_GPU) != 1:
        problems.append("--config=ci-macos must set %s exactly once" % MACOS_CI_LOCAL_GPU)
    rc = [line for line in rc if line not in (CI_MACOS_HOME, MACOS_CI_LOCAL_GPU)]
    paths = [re.fullmatch(r"--(action_env|host_action_env)=PATH=(\S+)", line) for line in rc]
    if (len(rc) != 2 or not all(paths) or
            {match.group(1) for match in paths} != {"action_env", "host_action_env"} or
            paths[0].group(2) != paths[1].group(2)):
        return problems + ["--config=ci-macos must pin exactly one PATH for actions and tools"]
    if "/Users/" in paths[0].group(2) or "$" in paths[0].group(2):
        problems.append("the pinned action PATH depends on a runner's home or environment")
    return problems


def _expand_configs(words, configs, seen=frozenset()):
    """Words with each --config=NAME replaced by that repository config's flags, recursively."""
    expanded = []
    for word in words:
        name = word.split("=", 1)[1] if word.startswith("--config=") else None
        if name is None or name in seen:
            expanded.append(word)
        else:
            expanded += _expand_configs(configs.get(name, []), configs, seen | {name})
    return expanded


def inherited_env_problems(workflow, bazelrc):
    """Environment a macOS lane copies from its runner into action or test keys.

    Runners of one lane can differ in such values (their home directories in particular), so a
    copied or shell-expanded value keeps each runner from reusing another's cached results.
    Covers the lane's flag lists, every Bazel command of the job, every config those name, the
    platform `macos` config and the repository's unconditional build, test and common flags."""
    jobs = _jobs(workflow)
    unconditional = [flag for line in bazelrc.splitlines()
                     if re.match(r"^(?:build|test|common)\s", line)
                     for flag in shlex.split(line.split(None, 1)[1])]
    configs = _config_flags(bazelrc)
    problems = []
    for name in LANES:
        words = _expand_configs(
            unconditional + [IMPLICIT_MACOS_CONFIG] + _lane_command_words(jobs[name]), configs)
        inherited = sorted({match.group(1) for match in map(INHERITED_ENV_FLAG.match, words)
                            if match})
        problems += ["%s copies %s from the runner into its keys" % (name, variable)
                     for variable in inherited if variable not in RUNNER_INDEPENDENT_INHERITED]
        expanded = sorted({match.group(1) for match in map(EXPANDED_ENV_FLAG.match, words)
                           if match})
        problems += ["%s sets %s from a shell or workflow expression" % (name, variable)
                     for variable in expanded]
    return problems


def _lane_test_command_words(job):
    """(step name, words) of each Bazel test command of a job, with its flag lists substituted."""
    lists = {"$" + variable: (_job_env(job, variable) or "").split()
             for variable in ("BAZEL_MACOS_BUILD_FLAGS",) + TEST_ONLY_VARIABLES}
    commands = []
    for step_name, command in _bazel_commands(job):
        match = BAZEL_INVOCATION.search(command)
        if match.group(1) in ("test", "coverage"):
            commands.append((step_name, [flag for word in command[match.end():].split()
                                         for flag in lists.get(word, [word])]))
    return commands


def local_gpu_unit_problems(workflow, bazelrc):
    """Test commands of a macOS lane that do not get the macOS CI local_gpu count.

    Bazel keeps the last --local_resources value for each resource, so the unconditional
    .bazelrc line comes first, then the platform `macos` config, then the command's own flags
    and the configs they name, in order."""
    jobs = _jobs(workflow)
    unconditional = [flag for line in bazelrc.splitlines()
                     if re.match(r"^(?:build|test|common)\s", line)
                     for flag in shlex.split(line.split(None, 1)[1])]
    configs = _config_flags(bazelrc)
    expected = MACOS_CI_LOCAL_GPU.rsplit("=", 1)[1]
    problems = []
    for name in LANES:
        for step_name, words in _lane_test_command_words(jobs[name]):
            expanded = _expand_configs(unconditional + [IMPLICIT_MACOS_CONFIG] + words, configs)
            units = [found.group(1) for found in map(_LOCAL_GPU_UNITS.match, expanded) if found]
            actual = units[-1] if units else "untracked"
            if actual != expected:
                problems.append("%s step %r runs GPU tests with local_gpu=%s, not %s" % (
                    name, step_name, actual, expected))
    return problems


def _lane_command_words(job):
    """Words of every Bazel command of a job, with the lane's flag lists substituted."""
    lists = {"$" + variable: (_job_env(job, variable) or "").split()
             for variable in ("BAZEL_MACOS_BUILD_FLAGS",) + TEST_ONLY_VARIABLES}
    return [flag for _, command in _bazel_commands(job)
            for word in command[BAZEL_INVOCATION.search(command).end():].split()
            for flag in lists.get(word, [word])]


# Test hooks of tools/ci_select_macos_xcode.sh, and the variable it sets itself. A workflow
# that sets any of them (job or workflow env, or a GITHUB_ENV write) can silently replace the
# pinned Xcode, so no macOS job and no workflow-level env may name them.
XCODE_OVERRIDES = re.compile(
    r"\b(?:XCODEBUILD|XCODE_APPLICATIONS_DIR|XCODE_LOCATOR|LSREGISTER|DEVELOPER_DIR|DONNER_CI_XCODE_IDENTITY|CCACHE_NAMESPACE)\b")
MACOS_JOBS = LANES


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
    problems = _xcode_override_problems(workflow)
    configs = bazelrc_configs.parse(bazelrc)
    problems += _flag_problems(LANE_FLAGS, [command for name in LANES
                                            for command in _bazel_commands(jobs[name])], configs)
    problems += _flag_list_problems(
        "main.yml", {variable: _workflow_env(workflow, variable)
                     for variable in TEST_ONLY_VARIABLES}, TEST_ONLY_FLAG, "test-only")
    neutral = neutral_configs(bazelrc)
    by_step = {}
    for name in LANES:
        job = jobs[name]
        commands = _bazel_commands(job)
        problems += _record_commands(name, commands, by_step, neutral)
        problems += _test_list_problems(name, commands, configs)
        problems += _select_problems(name, _steps(job), commands)
        if "Select pinned Xcode" in [step_name for step_name, _ in _steps(job)]:
            problems += _refresh_problems(name, commands, by_step)
    return (problems + _step_divergence(by_step) + _bazelrc_problems(bazelrc) +
            inherited_env_problems(workflow, bazelrc) + local_gpu_unit_problems(workflow, bazelrc))


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
    return label.lower().startswith(("macos", "xcode-"))


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
    return {str(path.relative_to(github.parent)): lane_workflow(path.read_text(encoding="utf-8"))
            for path in paths}


class MacosCacheKeyParityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = lane_workflow(_read(".github/workflows/main.yml"))
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

    def test_editor_wasm_cache_uses_the_selected_xcode_partition(self):
        job = _jobs(_read(".github/workflows/editor_wasm.yml"))["build"]
        steps = dict(_steps(job))
        self.assertIn("id: xcode", steps["Select pinned Xcode"])
        for name in ("Configure BuildBuddy remote cache", "Configure BuildBuddy read-only cache"):
            with self.subTest(step=name):
                self.assertIn("cache-partition: ${{ steps.xcode.outputs.cache_partition }}",
                              steps[name])

    def test_macos_lanes_pass_identical_key_affecting_flags(self):
        self.assertEqual([], key_parity_problems(self.workflow, self.bazelrc))

    def test_macos_lanes_build_one_editor_identity(self):
        # `--config=ci` names the test-lane editor commit (`ci-test-lane` in .bazelrc). It is a
        # key-affecting build setting, so every macOS lane must take it from that one config for
        # the editor actions, its Wasm package and the tests built on them to share keys.
        for command in ("build", "test", "coverage"):
            self.assertIn("--//build_defs:release_source_commit=" + "0" * 40,
                          bazelrc_configs.expand(command, ["--config=ci"],
                                                 bazelrc_configs.parse(self.bazelrc)))
        neutral = neutral_configs(self.bazelrc)
        self.assertNotIn("ci-test-lane", neutral)
        self.assertNotIn("ci", neutral)
        jobs = _jobs(self.workflow)
        built = 0
        for name in LANES:
            for step_name, command in _bazel_commands(jobs[name]):
                if BAZEL_INVOCATION.search(command).group(1) not in ("build", "test", "coverage"):
                    continue
                built += 1
                words = command.split()
                with self.subTest(lane=name, step=step_name):
                    self.assertIn("--config=ci", words)
                    self.assertEqual(
                        [], [word for word in words
                             if word.startswith("--//build_defs:release_source_commit=")])
        self.assertGreater(built, 10)
        # An editor identity set in one lane only is a key divergence the parity check reports.
        command = "bazelisk --nohome_rc --nosystem_rc build --config=ci --config=macos-vm"
        self.assertIn(command, self.workflow)
        diverged = self.workflow.replace(
            command, command + " --//build_defs:release_source_commit=" + "1" * 40)
        self.assertTrue(key_parity_problems(diverged, self.bazelrc))

    def test_metal_shader_validation_setting_selects_only_test_environment(self):
        build_defs = _read("build_defs/BUILD.bazel")
        metal = _read("donner/gpu/metal/tests/BUILD.bazel")

        def rule(text, kind, name):
            return text.split('%s(\n    name = "%s",' % (kind, name), 1)[1].split("\n)\n", 1)[0]

        # Only the Metal tests package can select on the setting; Bazel rejects any other use.
        self.assertIn('visibility = ["//visibility:private"]',
                      rule(build_defs, "string_flag", "metal_shader_validation"))
        self.assertIn('visibility = ["//donner/gpu/metal/tests:__pkg__"]',
                      rule(build_defs, "config_setting", "metal_shader_validation_off"))
        # Named only by the flag, the config_setting and its flag_values key.
        self.assertEqual(3, build_defs.count("metal_shader_validation"))
        # There, each select on it is a test's `env`, never a build attribute.
        uses = metal.split('"//build_defs:metal_shader_validation_off"')
        self.assertEqual(3, len(uses), "the shared device-test env and metal_device_loss_tests")
        self.assertTrue(uses[0].endswith("\n_METAL_API_VALIDATION_ENV = select({\n    "))
        self.assertTrue(uses[1].endswith("\n    env = select({\n        "))
        for line in metal.splitlines():
            if "_METAL_API_VALIDATION_ENV" in line and "= select(" not in line:
                self.assertEqual("    env = _METAL_API_VALIDATION_ENV,", line)

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
        # Every macOS job declares its env block; split at the self-hosted job's.
        start = self.workflow.index("\n  %s:\n" % SELF_HOSTED)
        index = self.workflow.index("    env:\n", start) + len("    env:\n")
        before, after = self.workflow[:index], self.workflow[index:]
        rc_line = "build:ci-macos --host_action_env=PATH="
        source = _read(".github/workflows/main.yml")
        lane_config = "--config=lane-macos"

        def lane(name, edit, *phases):
            return lane_view.edited_lane(name, edit, phases or None)

        def adds(*words):
            return lane_view.replacing(lane_config, lane_config, *words)

        build = lane_view.replacing("--config=ci", "--config=ci", "--copt=-O0")
        drop_lane_flags = (lambda args: [arg for arg in args if arg not in lanes.MACOS_BUILD])
        # (workflow, bazelrc, lane table edit, expected problem)
        cases = {
            "self-hosted drops the config": (
                None, None, lane(SELF_HOSTED, lane_view.replacing(lane_config)),
                "runs Bazel without"),
            "hosted adds a key-affecting flag": (
                None, None, lane(HOSTED, adds("--copt=-O1")), "step 'Build' differs"),
            "hosted carries a private cache flag": (
                None, None, lane(HOSTED, adds("--remote_timeout=5s")),
                "remote cache flags outside BuildBuddy"),
            "self-hosted adds a remote executor": (
                None, None, lane(SELF_HOSTED, adds("--remote_executor=grpcs://x")),
                "remote cache flags outside BuildBuddy"),
            "a step adds an inline build flag on one lane": (
                None, None, lane(HOSTED, build, "build"), "step 'Build' differs"),
            "a plain bazel step drops the lane flags": (
                None, None, lane(HOSTED, drop_lane_flags, "build"), "runs Bazel without"),
            "a build flag hidden in the test list": (
                None, None, mock.patch.dict(lanes.MACOS_TEST, {
                    HOSTED: ("--copt=-O0",) + lanes.MACOS_TEST[HOSTED]}), "not test-only"),
            "self-hosted job env overrides xcodebuild": (
                before + "      XCODEBUILD: /usr/bin/true\n" + after, None, None,
                "macos-self-hosted sets an Xcode selection override"),
            "self-hosted job env overrides the locator": (
                before + "      XCODE_LOCATOR: /usr/bin/true\n" + after, None, None,
                "macos-self-hosted sets an Xcode selection override"),
            "an earlier step redirects the Xcode lookup": (
                before + after.replace(
                    "      - name: Checkout\n",
                    "      - name: Redirect\n"
                    "        run: echo XCODE_APPLICATIONS_DIR=/x >> \"$GITHUB_ENV\"\n\n"
                    "      - name: Checkout\n", 1), None, None,
                "macos-self-hosted sets an Xcode selection override"),
            "a lane shares its test results": (
                None, None, lane(SELF_HOSTED, lane_view.replacing(
                    lanes.MACOS_TEST[SELF_HOSTED][0]), "test"),
                "does not name its lane"),
            "a lane copies HOME from the runner into test keys": (
                None, None, lane(SELF_HOSTED, lane_view.replacing(
                    lanes.MACOS_TEST[SELF_HOSTED][0], "--test_env=HOME",
                    lanes.MACOS_TEST[SELF_HOSTED][0]), "test"),
                "macos-self-hosted copies HOME from the runner"),
            "a step copies a runner variable into action keys": (
                None, None, lane(SELF_HOSTED, lane_view.replacing(
                    "--config=macos-vm", "--config=macos-vm", "--action_env=USER"), "build"),
                "macos-self-hosted copies USER from the runner"),
            "a lane's config copies a runner variable": (None, self.bazelrc.replace(
                "build:macos-vm --jobs=32\n",
                "build:macos-vm --jobs=32\nbuild:macos-vm --test_env=TMPDIR\n", 1), None,
                "macos-self-hosted copies TMPDIR from the runner"),
            "an unconditional rc line copies a runner variable": (None, self.bazelrc.replace(
                "build --test_env=LLM\n", "build --test_env=LLM\ntest --test_env=USER\n", 1),
                None, "macos copies USER from the runner"),
            "the implicit macos config copies a runner variable": (
                None, self.bazelrc + "build:macos --test_env=FOO\n", None,
                "macos-self-hosted copies FOO from the runner"),
            "a lane sets HOME from a shell expansion": (
                None, None, lane(SELF_HOSTED, lane_view.replacing(
                    lanes.MACOS_TEST[SELF_HOSTED][0], "--test_env=HOME=$HOME",
                    lanes.MACOS_TEST[SELF_HOSTED][0]), "test"),
                "macos-self-hosted sets HOME from a shell or workflow expression"),
            "a config sets a variable from a workflow expression": (
                None, self.bazelrc + "build:macos-vm --action_env=TMPDIR=${{ runner.temp }}\n",
                None, "macos-self-hosted sets TMPDIR from a shell or workflow expression"),
            "a macOS lane dropping ci-macos goes back to one GPU test at a time": (
                None, None, lane(SELF_HOSTED, lane_view.replacing(lanes.MACOS_BUILD[0])),
                "macos-self-hosted step 'Test' runs GPU tests with local_gpu=1"),
            "ci-macos stops raising the GPU test count": (None, self.bazelrc.replace(
                "build:ci-macos %s\n" % MACOS_CI_LOCAL_GPU, "", 1), None,
                "runs GPU tests with local_gpu=1"),
            "a lane lowers the GPU test count after ci-macos": (
                None, None, lane(SELF_HOSTED, lane_view.replacing(
                    lanes.MACOS_TEST[SELF_HOSTED][0], lanes.MACOS_TEST[SELF_HOSTED][0],
                    "--local_resources=local_gpu=1"), "test"),
                "macos-self-hosted step 'Test' runs GPU tests with local_gpu=1"),
            "ci-macos stops giving tests a fixed HOME": (None, self.bazelrc.replace(
                "build:ci-macos %s\n" % CI_MACOS_HOME, "", 1), None,
                "must set %s exactly once" % CI_MACOS_HOME),
            "ci-macos gives tests a runner's home": (None, self.bazelrc.replace(
                "build:ci-macos %s\n" % CI_MACOS_HOME,
                "build:ci-macos %s/Users/runner\n" % CI_MACOS_HOME, 1), None,
                "must set %s exactly once" % CI_MACOS_HOME),
            "a step drops the lane flags": (
                None, None, lane(HOSTED, lane_view.replacing(lanes.MACOS_BUILD[-1]), "build"),
                "runs Bazel without"),
            "a step adds a config on one lane": (
                None, None, lane(HOSTED, lane_view.replacing(
                    "--config=ci", "--config=ci", "--config=geode"), "build"),
                "step 'Build' differs"),
            "hosted skips the Xcode pin": (self.workflow.replace(
                "run: tools/ci_select_macos_xcode.sh\n",
                "run: echo skipped\n", 1), None, None, "macos selects Xcode other than"),
            "self-hosted Xcode selection removed": (before + after.replace(
                "      - name: Select pinned Xcode\n        id: xcode\n        shell: bash\n"
                "        run: tools/ci_select_macos_xcode.sh\n", "", 1), None, None,
                "macos-self-hosted does not select the pinned Xcode"),
            "self-hosted makes the pin optional again": (before + after.replace(
                "run: tools/ci_select_macos_xcode.sh\n",
                "run: tools/ci_select_macos_xcode.sh --optional\n", 1), None, None,
                "macos-self-hosted selects Xcode other than"),
            "self-hosted tolerates a failed selection": (before + after.replace(
                "        run: tools/ci_select_macos_xcode.sh\n",
                "        continue-on-error: true\n        run: tools/ci_select_macos_xcode.sh\n",
                1), None, None, "macos-self-hosted selects Xcode other than"),
            "unpinned host tool PATH": (None, self.bazelrc.replace(
                rc_line, "build:ci-macos --host_action_env=PATH=/usr/bin:", 1), None,
                "pin exactly one PATH"),
            "PATH from a runner home": (None, re.sub(
                r"(?m)^(build:ci-macos --(?:host_)?action_env=PATH=)", r"\g<1>/Users/runner/bin:",
                self.bazelrc), None, "runner's home"),
        }
        for name, (workflow, bazelrc, edit, expected) in cases.items():
            with self.subTest(case=name):
                self.assertTrue(workflow or bazelrc or edit)
                with edit or mock.patch.dict({}):
                    if workflow is None:
                        workflow = lane_workflow(source)
                    problems = key_parity_problems(workflow, bazelrc or self.bazelrc)
                self.assertTrue(any(expected in problem for problem in problems), problems)

    def test_local_opt_in_variables_are_never_set_in_ci(self):
        # They may be copied from the runner only because CI never sets them.
        local = RUNNER_INDEPENDENT_INHERITED - {"DONNER_CI_XCODE_IDENTITY"}
        self.assertEqual({"LLM", "DONNER_RENDERER_TEST_VERBOSE"}, local)
        files = _workflow_files()
        self.assertIn(".github/workflows/main.yml", files)
        for path, text in files.items():
            for variable in sorted(local):
                with self.subTest(path=path, variable=variable):
                    self.assertIsNone(re.search(r"\b%s\b" % variable, text))

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
        hosted_step = ("      - name: Install clang-tidy\n"
                       "        env:\n          URL: ${{ secrets.BAZEL_REMOTE_CACHE }}\n")
        with lane_view.edited_lane(HOSTED, lane_view.replacing(
                "--config=ci", "--config=ci", "--config=re"), ("build",)):
            remote = _workflow_files()[main]
        cases = {
            "hosted secret": files[main].replace(
                "      - name: Install clang-tidy\n", hosted_step, 1),
            "hosted remote execution": remote,
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
                "main.yml:macos-metal", "main.yml:macos-vm-qualification", "cmake.yml:macos",
                "cmake.yml:macos-self-hosted", "editor_wasm.yml:build",
                "bcr_preflight.yml:consumer", "bcr_preflight.yml:cli-macos", "fuzz.yml:macos",
                "perf.yml:perf", "sign_macos_release.yml:sign", "sign_macos_release.yml:verify")})
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
            "no selection": (wasm, files[wasm].replace(
                select.replace("        shell:", "        id: xcode\n        shell:"), "", 1),
                "does not select"),
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

    def _select_policy(self, policy, discovered, reports=None, statuses=None, report_status=0, registration=None):
        """Exercise an isolated source policy with deterministic discovery tools."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "ci_select_macos_xcode.sh"
            script.write_text(_read(SELECT_SCRIPT))
            (root / "ci_macos_xcode.txt").write_text(policy)
            trace = root / "trace"
            locator = root / "locator"
            branches = []
            registration_branches = []
            builds = dict(re.findall(r"(?m)^([0-9.]+) ([0-9A-Za-z]+)$", policy))
            for version, paths in discovered.items():
                registered = (registration or {}).get(version, {})
                marker = root / ("registered-" + version)
                output = "".join(str(root / path) + "\n" for path in paths)
                after = "".join(str(root / path) + "\n"
                                for path in registered.get("paths", paths))
                branches.append(
                    "%s) if [ -f %s ]; then printf '%%s' %s; exit %d; fi; "
                    "printf '%%s' %s; exit %d;;" % (
                        version, shlex.quote(str(marker)), shlex.quote(after),
                        registered.get("probe_status", (statuses or {}).get(version, 0)),
                        shlex.quote(output), (statuses or {}).get(version, 0)))
                registration_branches.append(
                    '%s) [ "$3" = %s ] || exit 41; touch %s; exit %d;;' % (
                        version, shlex.quote(builds.get(version, "invalid")),
                        shlex.quote(str(marker)), registered.get("status", 0)))
            locator.write_text(
                '#!/bin/sh\nif [ "${2:-}" = --register-installed ]; then\n'
                'printf "register %s %s\\n" "$1" "$3" >> ' + shlex.quote(str(trace)) +
                '\ncase "$1" in\n' + "\n".join(registration_branches) +
                '\nesac\nexit 2\nfi\nprintf "%s\\n" "$1" >> ' + shlex.quote(str(trace)) +
                '\ncase "$1" in\n' + "\n".join(branches) + "\nesac\nexit 2\n")
            locator.chmod(0o755)
            xcodebuild = root / "xcodebuild"
            reports = reports or {"Preferred": "Xcode 27.1\nBuild version 27A9275",
                                  "Fallback": "Xcode 27.2\nBuild version 27B52"}
            cases = ["*%s*) printf '%%s' %s;;" % (name, shlex.quote(value))
                     for name, value in reports.items()]
            finish = "kill -ALRM $$\n" if report_status == "alarm" else "exit %d\n" % report_status
            xcodebuild.write_text("#!/bin/sh\ncase \"$DEVELOPER_DIR\" in\n" +
                                  "\n".join(cases) + "\nesac\n" + finish)
            xcodebuild.chmod(0o755)
            env_file, outputs = root / "env", root / "outputs"
            env_file.write_text("")
            outputs.write_text("")
            result = subprocess.run(
                ["/bin/bash", str(script)], capture_output=True, text=True,
                timeout=test_timing.STALL_GUARD_SECONDS,
                env={"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                     "GITHUB_ENV": str(env_file), "GITHUB_OUTPUT": str(outputs),
                     "XCODE_LOCATOR": str(locator), "XCODEBUILD": str(xcodebuild),
                     "XCODE_PROBE_PYTHON": sys.executable, "RUNNER_ARCH": "ARM64"})
            return (result.returncode, env_file.read_text(), outputs.read_text(),
                    result.stdout + result.stderr, trace.read_text() if trace.exists() else "")

    def test_approved_fallback_is_used_only_when_preferred_is_absent(self):
        code, env, output, log, trace = self._select_policy(
            "27.1 27A9275\n27.2 27B52\n", {"27.1": [], "27.2": ["Fallback"]})
        self.assertEqual(0, code, log)
        self.assertIn("--xcode_version=27.2", env)
        self.assertIn("DONNER_CI_XCODE_IDENTITY=27.2-27B52", env)
        self.assertEqual("cache_partition=macos-ARM64-xcode-27.2-27B52\n", output)
        self.assertEqual("27.1\nregister 27.1 27A9275\n27.1\n27.2\n", trace)

    def test_unregistered_preferred_is_selected_before_installed_fallback(self):
        code, env, output, log, trace = self._select_policy(
            "27.1 27A9275\n27.2 27B52\n", {"27.1": [], "27.2": ["Fallback"]},
            registration={"27.1": {"paths": ["Preferred"]}})
        self.assertEqual(0, code, log)
        self.assertIn("DONNER_CI_XCODE_IDENTITY=27.1-27A9275", env)
        self.assertEqual("cache_partition=macos-ARM64-xcode-27.1-27A9275\n", output)
        self.assertEqual("27.1\nregister 27.1 27A9275\n27.1\n", trace)

    def test_unregistered_fallback_uses_its_own_approved_build(self):
        code, env, output, log, trace = self._select_policy(
            "27.1 27A9275\n27.2 27B52\n", {"27.1": [], "27.2": []},
            registration={"27.2": {"paths": ["Fallback"]}})
        self.assertEqual(0, code, log)
        self.assertIn("DONNER_CI_XCODE_IDENTITY=27.2-27B52", env)
        self.assertEqual("cache_partition=macos-ARM64-xcode-27.2-27B52\n", output)
        self.assertEqual(
            "27.1\nregister 27.1 27A9275\n27.1\n27.2\nregister 27.2 27B52\n27.2\n", trace)

    def test_invalid_preferred_registration_never_falls_through(self):
        for registered, reports in [
                ({"status": 3}, None),
                ({"status": 142}, None),
                ({"paths": ["Preferred"], "probe_status": 2}, None),
                ({"paths": ["Preferred", "Duplicate"]}, None),
                ({"paths": ["Preferred"]}, {"Preferred": "Xcode 27.1\nBuild version 99Z9"})]:
            with self.subTest(registered=registered, reports=reports):
                code, env, output, log, trace = self._select_policy(
                    "27.1 27A9275\n27.2 27B52\n", {"27.1": [], "27.2": ["Fallback"]},
                    reports=reports, registration={"27.1": registered})
                self.assertEqual((1, "", ""), (code, env, output), log)
                self.assertIn("register 27.1 27A9275\n", trace)
                self.assertNotIn("27.2", trace)

    def test_approved_preference_does_not_depend_on_installation_order(self):
        code, env, output, log, trace = self._select_policy(
            "27.1 27A9275\n27.2 27B52\n",
            {"27.1": ["Preferred"], "27.2": ["Fallback"]})
        self.assertEqual(0, code, log)
        self.assertIn("--xcode_version=27.1", env)
        self.assertEqual("cache_partition=macos-ARM64-xcode-27.1-27A9275\n", output)
        self.assertEqual("27.1\n", trace)

    def test_invalid_preferred_discovery_never_falls_through(self):
        policy = "27.1 27A9275\n27.2 27B52\n"
        for paths, reports, statuses in [
                (["Preferred", "Duplicate"], None, None),
                (["Preferred"], {"Preferred": "Xcode 27.1\nBuild version 99Z9"}, None),
                ([], None, {"27.1": 2})]:
            with self.subTest(paths=paths, reports=reports, statuses=statuses):
                code, env, output, log, trace = self._select_policy(
                    policy, {"27.1": paths, "27.2": ["Fallback"]}, reports, statuses)
                self.assertEqual(1, code, log)
                self.assertEqual("", env)
                self.assertEqual("", output)
                self.assertEqual("27.1\n", trace)

    def test_unapproved_fallback_build_is_rejected(self):
        code, env, output, log, trace = self._select_policy(
            "27.1 27A9275\n27.2 27B52\n", {"27.1": [], "27.2": ["Fallback"]},
            {"Fallback": "Xcode 27.2\nBuild version 27B48"})
        self.assertEqual(1, code, log)
        self.assertEqual("", env)
        self.assertEqual("", output)
        self.assertEqual("27.1\nregister 27.1 27A9275\n27.1\n27.2\n", trace)

    def test_actual_toolchain_identity_binds_all_bazel_cache_keys(self):
        lines = _read(".bazelrc").splitlines()
        for flag in ["build --action_env=DONNER_CI_XCODE_IDENTITY",
                     "build --host_action_env=DONNER_CI_XCODE_IDENTITY",
                     "test --test_env=DONNER_CI_XCODE_IDENTITY"]:
            self.assertEqual(1, lines.count(flag), flag)

    def test_failed_version_probe_cannot_publish_matching_stdout(self):
        for report_status in [2, "alarm"]:
            with self.subTest(report_status=report_status):
                code, env, output, log, trace = self._select_policy(
                    "27.1 27A9275\n27.2 27B52\n",
                    {"27.1": ["Preferred"], "27.2": ["Fallback"]}, report_status=report_status)
                self.assertEqual(1, code, log)
                self.assertEqual("", env)
                self.assertEqual("", output)
                self.assertEqual("27.1\n", trace)

    def test_policy_rejects_duplicate_versions_and_unbounded_entries(self):
        for policy in ["", "27.1 27A9275\n27.1 27A9275\n",
                       "27.1 27A9275\n27.1 27B52\n", "malformed\n",
                       "27.1 27A9275\n27.2 27B52\n27.3 27C1\n"]:
            with self.subTest(policy=policy):
                code, env, output, log, trace = self._select_policy(policy, {})
                self.assertEqual(1, code, log)
                self.assertEqual("", env)
                self.assertEqual("", output)
                self.assertEqual("", trace)

    def test_bcr_consumer_disk_cache_includes_actual_toolchain_identity(self):
        workflow = _workflow_files()[".github/workflows/bcr_preflight.yml"]
        consumer = _jobs(workflow)["consumer"]
        disk_cache = re.findall(r"(?m)^          disk-cache: (.+)$", consumer)
        self.assertEqual(disk_cache, [
            "bcr-${{ matrix.os }}-${{ matrix.bazel }}-${{ env.DONNER_CI_XCODE_IDENTITY }}"])

    def test_identity_has_no_config_or_workflow_override(self):
        identity = "DONNER_CI_XCODE_IDENTITY"
        lines = [line.split("#", 1)[0].strip() for line in _read(".bazelrc").splitlines()]
        self.assertEqual(3, sum(identity in line for line in lines))
        for path, text in _workflow_files().items():
            for line in text.splitlines():
                if identity in line:
                    self.assertIn("${{ env.DONNER_CI_XCODE_IDENTITY }}", line)
                    if path == ".github/workflows/bcr_preflight.yml":
                        self.assertEqual(line.strip(),
                                         "disk-cache: bcr-${{ matrix.os }}-${{ matrix.bazel }}-"
                                         "${{ env.DONNER_CI_XCODE_IDENTITY }}")
                    else:
                        self.assertEqual(path, ".github/workflows/cmake.yml", (path, line))
                        self.assertRegex(line.strip(), r"^(?:key:|ccache-macos-)")

    def test_workflow_cannot_replace_the_selected_identity(self):
        for name in ["DONNER_CI_XCODE_IDENTITY", "CCACHE_NAMESPACE"]:
            modified = self.workflow.replace("env:\n", "env:\n  " + name + ": unverified\n", 1)
            problems = key_parity_problems(modified, self.bazelrc)
            self.assertTrue(any("selection override" in problem for problem in problems), problems)

    def test_native_and_vm_qualification_cannot_override_xcode_selection(self):
        jobs = _jobs(self.workflow)
        for job in ("macos-metal", "macos-vm-qualification"):
            for name in ("XCODEBUILD", "XCODE_APPLICATIONS_DIR", "XCODE_LOCATOR", "LSREGISTER",
                         "DEVELOPER_DIR", "DONNER_CI_XCODE_IDENTITY", "CCACHE_NAMESPACE"):
                with self.subTest(job=job, name=name):
                    modified_job = jobs[job].replace(
                        "    env:\n", "    env:\n      " + name + ": unverified\n", 1)
                    self.assertNotEqual(jobs[job], modified_job)
                    workflow = self.workflow.replace(jobs[job], modified_job, 1)
                    self.assertIn(job + " sets an Xcode selection override",
                                  _xcode_override_problems(workflow))

    def test_cmake_cache_families_include_the_actual_toolchain(self):
        workflow = _workflow_files()[".github/workflows/cmake.yml"]
        for line in workflow.splitlines():
            if "cmake-deps-macos-" in line or "ccache-macos-" in line:
                self.assertIn("${{ env.DONNER_CI_XCODE_IDENTITY }}", line)

    def test_pin_names_one_hosted_xcode_version_and_build(self):
        lines = [line for line in _read(PIN_FILE).splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        self.assertEqual(["27.1 27A9275", "27.2 27B52"], lines)

    def _select(self, discovered=("{root}/Xcode-any-name.app/Contents/Developer",),
                reported="Xcode {version}\nBuild version {build}", locator_status=0,
                missing_xcrun=False, args=(), runner="github-hosted", registered=None,
                registration_status=0, post_locator_status=None, registration_error="",
                build_flags="--config=ci-macos"):
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
            marker = root / "registered"
            before = "".join(fill(line) + "\\n" for line in discovered)
            after = "".join(fill(line) + "\\n" for line in (
                discovered if registered is None else registered))
            locator.write_text(
                '#!/bin/sh\n[ "$1" = %s ] || exit 0\n'
                'if [ "${2:-}" = --register-installed ]; then\n'
                '  [ "$3" = %s ] || exit 41\n'
                '  touch %s\n  printf "%%s" %s >&2\n  exit %d\nfi\n'
                'if [ -f %s ]; then\n  printf %s\n  exit %d\nfi\n'
                'printf %s\nexit %d\n' % (
                    shlex.quote(version), shlex.quote(build), shlex.quote(str(marker)),
                    shlex.quote(fill(registration_error)), registration_status,
                    shlex.quote(str(marker)), shlex.quote(after),
                    locator_status or 0 if post_locator_status is None else post_locator_status,
                    shlex.quote(before), locator_status or 0), encoding="utf-8")
            locator.chmod(0o755)
            xcrun = root / "xcrun"
            if not missing_xcrun:
                xcrun.write_text("#!/bin/sh\necho simulated-sdk-failure >&2\nexit 1\n",
                                 encoding="utf-8")
                xcrun.chmod(0o755)
            env_file, output_file = root / "env", root / "output"
            env_file.write_text("")
            output_file.write_text("")
            env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "GITHUB_ENV": str(env_file),
                   "GITHUB_OUTPUT": str(output_file), "RUNNER_ARCH": "ARM64",
                   "BAZEL_MACOS_BUILD_FLAGS": build_flags, "XCODEBUILD": str(xcodebuild),
                   "XCODE_LOCATOR": str(locator) if locator_status is not None else "",
                   "XCRUN": str(xcrun), "XCODE_PROBE_PYTHON": sys.executable,
                   "RUNNER_ENVIRONMENT": runner}
            result = subprocess.run(
                ["/bin/bash", str(_path(SELECT_SCRIPT)), *args],
                # The test's own PATH: a fixed /usr/bin:/bin lacks coreutils on hosts that
                # keep them elsewhere, such as NixOS remote-execution workers.
                env={key: value for key, value in env.items() if value is not None},
                capture_output=True, text=True, timeout=test_timing.STALL_GUARD_SECONDS,
                check=False)
            return (result.returncode, env_file.read_text(), output_file.read_text(),
                    result.stdout + result.stderr, version, build, root)

    def test_select_script_exports_the_one_discovered_pinned_xcode(self):
        code, env, output, log, version, build, root = self._select()
        self.assertEqual(0, code, log)
        self.assertEqual(env.splitlines(), [
            "DEVELOPER_DIR=%s/Xcode-any-name.app/Contents/Developer" % root,
            "DONNER_CI_XCODE_IDENTITY=%s-%s" % (version, build),
            "CCACHE_NAMESPACE=donner-xcode-%s-%s" % (version, build),
            "BAZEL_MACOS_BUILD_FLAGS=--xcode_version=%s" % version])
        self.assertEqual(output, "cache_partition=macos-ARM64-xcode-%s-%s\n" % (version, build))
        self.assertIn("Selected the approved Xcode %s (%s)." % (version, build), log)
        self.assertNotIn("simulated noise", log)

    def test_lanes_append_the_selected_xcode_to_the_lane_flags(self):
        # main.yml's macOS jobs set no BAZEL_MACOS_BUILD_FLAGS; the selection writes only the
        # flag it adds, whatever the runner's environment holds, and each lane phase passes it
        # after the table's key-affecting flags, where the job's flags variable used to put it.
        for inherited in (None, "--config=stale --copt=-O0"):
            with self.subTest(inherited=inherited):
                code, env, _, log, version, _, _ = self._select(build_flags=inherited)
                self.assertEqual(0, code, log)
                written = env.splitlines()[-1]
                self.assertEqual(written, "BAZEL_MACOS_BUILD_FLAGS=--xcode_version=%s" % version)
        description = lane.dry_run("macos-self-hosted", "build",
                                   {RUNTIME_FLAGS: written.split("=", 1)[1]})
        self.assertEqual(description["actions"][0]["argv"][-3:], [
            "--config=lane-macos", "--xcode_version=" + version, "@selected_targets"])
        self.assertEqual(bazelrc_configs.expand("build", ["--config=lane-macos"],
                                                bazelrc_configs.parse(self.bazelrc))[-1],
                         "--macos_minimum_os=13.3")
        self.assertNotIn(RUNTIME_FLAGS, self.workflow.split("\njobs:\n", 1)[0])
        for name in LANES:
            self.assertNotIn(RUNTIME_FLAGS + ":", _jobs(self.workflow)[name], name)

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

    def test_select_registers_missing_xcode_then_uses_authoritative_lookup(self):
        code, env, output, log, version, build, root = self._select(
            discovered=(), registered=("{root}/Installed.app/Contents/Developer",))
        self.assertEqual(0, code, log)
        self.assertIn("DEVELOPER_DIR=%s/Installed.app/Contents/Developer" % root, env)
        self.assertEqual(output, "cache_partition=macos-ARM64-xcode-%s-%s\n" % (version, build))
        # Healthy discovery and ambiguity never trigger registration.
        self.assertEqual(0, self._select(registration_status=3)[0])
        code, env, output, log, *_ = self._select(
            discovered=("{root}/A.app/Contents/Developer", "{root}/B.app/Contents/Developer"),
            registration_status=3)
        self.assertIn("is ambiguous", log)
        self.assertEqual((1, "", ""), (code, env, output))

    def test_registration_failures_export_no_selection_or_private_diagnostics(self):
        cases = [
            dict(registration_status=3, registration_error="cannot access {root}/private.app"),
            dict(registration_status=142),
            dict(registered=("{root}/A.app/Contents/Developer",), post_locator_status=2),
            dict(registered=("{root}/A.app/Contents/Developer",), reported="Xcode {version}\nBuild version 99Z999"),
            dict(registered=("{root}/A.app/Contents/Developer", "{root}/B.app/Contents/Developer")),
        ]
        for case in cases:
            with self.subTest(case=case):
                code, env, output, log, _, _, root = self._select(
                    discovered=(), runner="self-hosted", **case)
                self.assertEqual((1, "", ""), (code, env, output))
                self.assertIn("::error::", log)
                self.assertNotIn(str(root), log)
        code, _, _, log, *_ = self._select(discovered=(), locator_status=2, registration_status=3)
        self.assertEqual(1, code)
        self.assertIn("probe failed (exit 2)", log)
        self.assertNotIn("could not be registered", log)

    @unittest.skipUnless(sys.platform == "darwin", "uses the native macOS Security framework")
    def test_registration_rejects_untrusted_and_mismatched_bundles(self):
        version, build = [line for line in _read(PIN_FILE).splitlines()
                          if line.strip() and not line.startswith("#")][0].split()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            driver = root / "registration-test.m"
            driver.write_text(
                '#define main locatorMain\n#import "%s"\n#undef main\n'
                'int main(int argc, const char* argv[]) { @autoreleasepool {\n'
                '  return RegisterInstalled(@(argv[2]), @(argv[3]),\n'
                '      [NSURL fileURLWithPath:@(argv[1]) isDirectory:YES]);\n}}\n' %
                _path("tools/ci_xcode_discoverable.m"), encoding="utf-8")
            binary = root / "registration-test"
            result = subprocess.run(
                ["/usr/bin/xcrun", "clang", "-fobjc-arc", "-framework", "Foundation",
                 "-framework", "CoreServices", "-framework", "Security", str(driver),
                 "-o", str(binary)], capture_output=True, timeout=test_timing.STALL_GUARD_SECONDS)
            self.assertEqual(0, result.returncode, "native registration test failed to compile")
            applications = root / "Applications"
            applications.mkdir()
            app = applications / "Candidate.app"
            contents = app / "Contents"
            executable = contents / "MacOS" / "Xcode"
            executable.parent.mkdir(parents=True)
            executable.write_bytes(binary.read_bytes())
            executable.chmod(0o755)
            info = {"CFBundleIdentifier": "com.apple.dt.Xcode", "CFBundleExecutable": "Xcode",
                    "CFBundlePackageType": "APPL", "CFBundleShortVersionString": version}
            metadata = {"CFBundleShortVersionString": version, "ProductBuildVersion": build}

            def probe():
                return subprocess.run([str(binary), str(applications), version, build],
                                      capture_output=True,
                                      timeout=test_timing.STALL_GUARD_SECONDS).returncode

            def write_metadata():
                (contents / "Info.plist").write_bytes(plistlib.dumps(info))
                (contents / "version.plist").write_bytes(plistlib.dumps(metadata))

            write_metadata()
            result = subprocess.run(["/usr/bin/codesign", "--force", "--sign", "-", str(app)],
                                    capture_output=True, timeout=test_timing.STALL_GUARD_SECONDS)
            self.assertEqual(0, result.returncode, "cannot create ad-hoc signed test fixture")
            self.assertEqual(3, probe(), "a valid ad-hoc signature must not claim Apple identity")
            metadata["ProductBuildVersion"] = "99Z999"
            write_metadata()
            self.assertEqual(0, probe(), "wrong builds must not be registered")
            metadata["ProductBuildVersion"] = build
            info["CFBundleShortVersionString"] = 123
            write_metadata()
            self.assertEqual(0, probe(), "malformed versions must not be registered")
            info["CFBundleShortVersionString"] = version
            info["CFBundleIdentifier"] = "org.example.other"
            write_metadata()
            self.assertEqual(0, probe(), "other apps must not be registered")

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
