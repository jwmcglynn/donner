"""Keeps the GitHub-hosted and self-hosted macOS lanes on identical Bazel action keys.

The two lanes in main.yml can share one remote cache only if every key-affecting input matches:
the Bazel flags and configs, the action environment (PATH in particular, which Bazel copies from
the client unless it is pinned), and the Xcode version and build, which Apple toolchain actions
carry in their environment. Both lanes configure the remote cache only through the BuildBuddy
action, so their build flags must be identical. Test-only flags and environment (--test_env)
are deliberately left to each lane, and each lane names itself in its test environment: a test
result depends on the runner's GPU, so it must not be shared across runners.

Every GitHub-hosted job is also kept off the self-hosted lane's private cache and remote
execution configuration.
"""

from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import textwrap
import unittest

from python.runfiles import runfiles


HOSTED = "macos"
SELF_HOSTED = "macos-self-hosted"
# Remote cache and executor flags. The macOS lanes get their remote cache only from the
# BuildBuddy action, so neither passes any of these itself.
REMOTE_FLAG = re.compile(r"^--(?:no)?(?:remote_|incompatible_remote_|credential_helper)")
LANE_FLAGS = "$BAZEL_MACOS_BUILD_FLAGS"
BAZEL_INVOCATION = re.compile(
    r"\bbazel(?:isk)?(?:\s+--[a-z_]+)*\s+(build|test|fetch|query|cquery|coverage|run)\b")
# Flags that only select, run or report tests. They never change a build action's key.
TEST_ONLY_FLAG = re.compile(r"^--(?:no)?(?:test_|cache_test_results|local_test_jobs|"
                            r"flaky_test_attempts|runs_per_test|keep_going|build_event_json_file)")
# Job environment values a lane may set for itself; each may hold only test-only flags.
TEST_ONLY_VARIABLES = ("BAZEL_MACOS_TEST_FLAGS", "BROWSER_STALL_TEST_FLAGS",
                       "HOSTED_MACOS_QUARANTINE_TEST_FLAGS")
# Each lane names itself in its test environment, so test results never cross runners.
LANE_TEST_ENV = {"macos": "--test_env=DONNER_CI_MACOS_LANE=hosted",
                 "macos-self-hosted": "--test_env=DONNER_CI_MACOS_LANE=self-hosted"}
# The toolchain repository refresh runs before any build and takes no build flags.
TOOLCHAIN_REFRESH = "fetch --force --lockfile_mode=off"
SELECT_SCRIPT = "tools/ci_select_macos_xcode.sh"
PIN_FILE = "tools/ci_macos_xcode.txt"


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


def _build_flags(command):
    """The key-affecting words of a Bazel command: build flags and environment lists after the
    command, without test-only flags, test-only lists or targets."""
    words = command[BAZEL_INVOCATION.search(command).end():].split()
    test_lists = {"$" + name for name in TEST_ONLY_VARIABLES}
    return sorted(word for word in words if (word.startswith("--") and
                                             not TEST_ONLY_FLAG.match(word)) or
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


def _test_list_problems(name, job):
    """Problems of a lane's test-only environment lists and its lane name."""
    problems = []
    for variable in TEST_ONLY_VARIABLES:
        value = _job_env(job, variable)
        if value is not None and not all(TEST_ONLY_FLAG.match(flag)
                                         for flag in shlex.split(value)):
            problems.append("%s %s holds a flag that is not test-only" % (name, variable))
    tests = shlex.split(_job_env(job, "BAZEL_MACOS_TEST_FLAGS") or "")
    if LANE_TEST_ENV[name] not in tests:
        problems.append("%s does not name its lane with %s" % (name, LANE_TEST_ENV[name]))
    return problems


def _record_commands(name, commands, by_step):
    """Record each build command's configs by step; problems for commands without the lane flags."""
    problems = []
    for step_name, command in commands:
        if TOOLCHAIN_REFRESH in command:
            continue
        if LANE_FLAGS not in command:
            problems.append("%s step %r runs Bazel without %s" % (name, step_name, LANE_FLAGS))
        by_step.setdefault(step_name, {}).setdefault(name, set()).add(
            tuple(_build_flags(command)))
    return problems


def _select_problems(name, steps, commands, mode):
    """Problems of a lane's pinned-Xcode selection and where it runs."""
    names = [step_name for step_name, _ in steps]
    if "Select pinned Xcode" not in names:
        return ["%s does not select the pinned Xcode" % name]
    problems = []
    select_index = names.index("Select pinned Xcode")
    select = steps[select_index][1]
    if (_logical_commands(select) != ["%s %s" % (SELECT_SCRIPT, mode)] or
            "id: xcode" not in select):
        problems.append("%s selects Xcode other than with %s %s" % (name, SELECT_SCRIPT, mode))
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


def key_parity_problems(workflow, bazelrc):
    """Every way the two macOS lanes in main.yml could stop computing identical action keys."""
    jobs = _jobs(workflow)
    hosted, self_hosted = jobs[HOSTED], jobs[SELF_HOSTED]
    hosted_flags = _job_env(hosted, "BAZEL_MACOS_BUILD_FLAGS")
    self_flags = _job_env(self_hosted, "BAZEL_MACOS_BUILD_FLAGS")
    if hosted_flags is None or self_flags is None:
        return ["a macOS lane does not declare BAZEL_MACOS_BUILD_FLAGS"]
    problems = _flag_problems(hosted_flags, self_flags)
    by_step = {}
    for name, job, mode in ((HOSTED, hosted, "--required"),
                            (SELF_HOSTED, self_hosted, "--optional")):
        commands = _bazel_commands(job)
        problems += _record_commands(name, commands, by_step)
        problems += _test_list_problems(name, job)
        problems += _select_problems(name, _steps(job), commands, mode)
        if "Select pinned Xcode" in [step_name for step_name, _ in _steps(job)]:
            problems += _refresh_problems(name, commands, by_step)
    for step_name, lanes_seen in sorted(by_step.items()):
        if len(lanes_seen) == 2 and lanes_seen[HOSTED] != lanes_seen[SELF_HOSTED]:
            problems.append("step %r differs between the lanes: %s" % (step_name, lanes_seen))
    return problems + _bazelrc_problems(bazelrc)


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

    def test_key_parity_policy_rejects_divergence(self):
        hosted_flags = 'BAZEL_MACOS_BUILD_FLAGS: "--config=ci-macos --macos_minimum_os=13.3"\n'
        # Both jobs carry the same line; the self-hosted job's is the last one.
        before, _, after = self.workflow.rpartition(hosted_flags)
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
            "a lane shares its test results": (self.workflow.replace(
                LANE_TEST_ENV["macos-self-hosted"], "--test_env=HOME", 1),
                self.bazelrc, "does not name its lane"),
            "a step drops the lane flags": (self.workflow.replace(
                "bazelisk build --config=ci $BAZEL_MACOS_BUILD_FLAGS $TARGETS",
                "bazelisk build --config=ci $TARGETS", 1),
                self.bazelrc, "runs Bazel without"),
            "a step adds a config on one lane": (self.workflow.replace(
                "bazelisk build --config=ci $BAZEL_MACOS_BUILD_FLAGS $TARGETS",
                "bazelisk build --config=ci --config=geode $BAZEL_MACOS_BUILD_FLAGS $TARGETS", 1),
                self.bazelrc, "step 'Build' differs"),
            "hosted skips the Xcode pin": (self.workflow.replace(
                "run: tools/ci_select_macos_xcode.sh --required",
                "run: echo skipped", 1), self.bazelrc, "selects Xcode other than"),
            "self-hosted Xcode selection removed": (self.workflow.replace(
                "      - name: Select pinned Xcode\n        id: xcode\n        shell: bash\n"
                "        run: tools/ci_select_macos_xcode.sh --optional\n", "", 1),
                self.bazelrc, "does not select the pinned Xcode"),
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

    def test_pin_names_one_hosted_xcode_version_and_build(self):
        lines = [line for line in _read(PIN_FILE).splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        self.assertEqual(1, len(lines))
        self.assertRegex(lines[0], r"^\d+\.\d+(?:\.\d+)? \d+[A-Z]\d+[a-z]?$")

    def _select(self, mode, installed):
        """Runs the selection script against a fake Applications directory."""
        version, build = [line for line in _read(PIN_FILE).splitlines()
                          if line.strip() and not line.startswith("#")][0].split()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            if installed is not None:
                (root / ("Xcode_%s.app" % version) / "Contents" / "Developer").mkdir(parents=True)
            xcodebuild = root / "xcodebuild"
            reported = installed or ""
            xcodebuild.write_text("#!/bin/sh\nprintf '%%s' '%s'\n" % reported.replace(
                "{version}", version).replace("{build}", build), encoding="utf-8")
            xcodebuild.chmod(0o755)
            env_file, output_file = root / "env", root / "output"
            env_file.write_text("")
            output_file.write_text("")
            result = subprocess.run(
                ["/bin/bash", str(_path(SELECT_SCRIPT)), mode],
                env={"PATH": "/usr/bin:/bin", "GITHUB_ENV": str(env_file),
                     "GITHUB_OUTPUT": str(output_file), "RUNNER_ARCH": "ARM64",
                     "BAZEL_MACOS_BUILD_FLAGS": "--config=ci-macos",
                     "XCODE_APPLICATIONS_DIR": str(root), "XCODEBUILD": str(xcodebuild)},
                capture_output=True, text=True, timeout=60, check=False)
            return (result.returncode, env_file.read_text(), output_file.read_text(),
                    result.stdout + result.stderr, version, build, root)

    def test_select_script_exports_the_pinned_xcode_only_when_it_is_installed(self):
        exact = "Xcode {version}\nBuild version {build}"
        other_build = "Xcode {version}\nBuild version 99Z999"
        for mode in ("--required", "--optional"):
            with self.subTest(mode=mode, case="installed"):
                code, env, output, log, version, build, root = self._select(mode, exact)
                self.assertEqual(0, code, log)
                self.assertEqual(env.splitlines(), [
                    "DEVELOPER_DIR=%s/Xcode_%s.app/Contents/Developer" % (root, version),
                    "BAZEL_MACOS_BUILD_FLAGS=--config=ci-macos --xcode_version=%s" % version])
                self.assertEqual(output, "cache_partition=macos-ARM64-xcode-%s-%s\n" % (
                    version, build))
        for case, installed in (("absent", None), ("other build", other_build)):
            with self.subTest(mode="--required", case=case):
                code, env, output, log, *_ = self._select("--required", installed)
                self.assertEqual(1, code, log)
                self.assertEqual("", env)
                self.assertEqual("cache_partition=\n", output)
            with self.subTest(mode="--optional", case=case):
                code, env, output, log, *_ = self._select("--optional", installed)
                self.assertEqual(0, code, log)
                self.assertEqual("", env)
                self.assertEqual("cache_partition=\n", output)
                self.assertIn("::warning::", log)
                # The warning names only the public pin, never what the runner has.
                self.assertNotIn("99Z999", log)

    def test_select_script_rejects_an_unknown_mode(self):
        result = subprocess.run(["/bin/bash", str(_path(SELECT_SCRIPT))], capture_output=True,
                                text=True, timeout=60, check=False)
        self.assertEqual(2, result.returncode)


if __name__ == "__main__":
    unittest.main()
