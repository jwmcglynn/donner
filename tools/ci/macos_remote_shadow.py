#!/usr/bin/env python3
"""Runs the advisory macOS remote-execution shadow lane and summarizes what it did.

The lane builds and tests the pull request's selected targets for macOS from a Linux runner, with
every action on a remote macOS executor (`--config=remote-macos`, docs/building.md), so its speed,
cache reuse and failures can be compared with the self-hosted macOS lane before anything depends
on it. It never gates a pull request: the workflow job ignores its result, and this script reports
what happened in the job summary and in a derived summary file instead.

It runs, in order:

  preflight  `bazel build --nobuild` with and without the config, then an uncached run of a
             one-line routing probe test under the config. Fails at once, with a message that
             names no endpoint, when the runner's rc files configure no remote executor for the
             config, the same executor as without it, or one that does not run the probe on
             macOS arm64 in this invocation.
  native     `bazel test --config=remote-macos` of the selection: builds every selected target and
             runs every selected test except the ones listed in EXCLUDED_TAGS and the GPU-opening
             tests (GPU_TAGS).
  gpu        `bazel test --config=remote-macos-gpu --jobs=4 --build_tests_only` of the GPU-opening
             tests of the selection, on executors that have a GPU.

Bazel's local GPU scheduling (the local resource tag and --local_test_jobs) does not apply to
remote tests, so the gpu phase caps the whole invocation at GPU_JOBS concurrent actions instead,
which bounds how many GPU-opening tests run on the executors at once. `--jobs` comes after the
config, so it wins over any `--jobs` the runner's rc binds to it.

The repository supplies every flag that affects what is built and run. The runner's own rc files
supply, under `build:remote-macos-base`, the executor and cache endpoint, credentials, queue
priority and a `--jobs` cap; see "Remote macOS shadow lane" in docs/building.md. This script never
passes `--config=ci`: on a self-hosted Linux runner that config also carries the Linux lane's
executor. It passes the parts of `ci` that this lane needs itself (CI_FLAGS).

Usage:
  tools/ci/macos_remote_shadow.py [--dry-run] --out DIR [--fallback true|false]
      [--affected 'LABEL ...'] [--bazel PATH] [--startup-option OPT]... [--rc-option OPT]...

--fallback and --affected default to the FALLBACK and AFFECTED environment variables, which the
workflow sets from the planner's outputs: a fallback selects //..., otherwise the affected labels.
--rc-option stands in for runner rc configuration in a manual run: each one is placed right after
the config, where the config's own rc lines apply. --dry-run prints each Bazel command line and
runs nothing.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time

# Not resolved: under Bazel the script runs from its runfiles tree, beside its data files.
ROOT = Path(os.path.abspath(__file__)).parents[2]

CONFIG = "remote-macos"
GPU_CONFIG = "remote-macos-gpu"
# At most this many actions, and so GPU-opening tests, run at once in the gpu phase.
GPU_JOBS = 4

# The tag that marks a test that opens a GPU device: one unit of the declared local GPU resource
# (LOCAL_GPU_TEST_TAG in build_defs/rules.bzl), which `opens_gpu_device` adds and hand-written GPU
# tests carry themselves.
GPU_TAGS = ("resources:local_gpu:1",)

# Tests this lane never runs, with why. The self-hosted macOS lane's exclusions come first; this
# lane runs a subset of that lane's tests.
EXCLUDED_TAGS = (
    # The self-hosted macOS lane's own exclusions.
    "manual",                       # Opt-in tests.
    "perf",                         # Wall-clock budgets run nightly.
    "requires_shader_validation",   # Need the dedicated real-GPU Metal job.
    "requires_metal_toolchain",     # Need the Metal toolchain component installed.
    "ci-remote-gpu",                # Remote twins of GPU tests the gpu phase already runs.
    "launches-browser",             # Browser tests run in their own serial phase or job.
    "fuzz_soak",                    # Time-budgeted mutation runs only in the Fuzz workflow.
    # What a remote executor cannot run.
    "requires_desktop_session",     # Need a window server or a logged-in user.
    "requires_xvfb",                # Need the Linux runner's own X display.
    "local",                        # Must run on the client; the config has no local strategy.
    "no-remote",
    "no-remote-exec",
)

# Messages a remote backend attaches to failed actions can name it, and this job's log is public.
# Bazel still reports each failure and the action's own output. Every lane command passes it.
QUIET_REMOTE_MESSAGES = "--remote_print_execution_messages=success"
SHUTDOWN_SECONDS = 120
MAX_IDLE = "--max_idle_secs=600"


def shutdown_command(bazel, startup):
    """Stops the lane's Bazel server, which runs under its own output root."""
    return [bazel, *startup, "shutdown"]


# The parts of `--config=ci` this lane needs. The rest of `ci` is the runner's repository cache
# path and the lane tag filter, which this lane states itself.
CI_FLAGS = (
    "--verbose_failures",
    # The planner's affected list can name targets incompatible with macOS.
    "--skip_incompatible_explicit_targets",
    # A local build event file must not depend on the remote cache (see `common:ci`).
    "--nobuild_event_json_file_path_conversion",
    "--nobuild_event_binary_file_path_conversion",
    "--nobuild_event_text_file_path_conversion",
)

# The editor's build identity action reads the checkout's commit and must then run on the client,
# which this config cannot do. An explicit commit makes it an ordinary remote action. This lane
# ships nothing, so it takes the fixed placeholder commit from the narrow test-lane config that
# `ci` composes for every non-release lane.
TEST_LANE_CONFIG = "--config=ci-test-lane"

# Remote timeouts, fallback and every other remote setting come from the config and the runner's
# rc; a command-line value here would override the runner's.
LANE_FLAGS = CI_FLAGS + (
    # Tests run remotely; only their logs come back.
    "--remote_download_outputs=minimal",
    r"--remote_download_regex=.*(test\.log|test\.xml|outputs\.zip)$",
    TEST_LANE_CONFIG,
    "--test_output=errors",
    QUIET_REMOTE_MESSAGES,
    # Test results of this lane are its own; build actions are shared.
    "--test_env=DONNER_CI_MACOS_LANE=remote-shadow",
)

IDENTITY = "DONNER_CI_XCODE_IDENTITY"
PIN_FILE = "tools/ci_macos_xcode.txt"
WATCHDOG = "tools/ci/bazel_watchdog.sh"
TEST_WRAPPER = "tools/ci_bazel_test.sh"

PHASES = ("native", "gpu")


def tag_filters(phase):
    """The --test_tag_filters value of a phase."""
    excluded = ["-" + tag for tag in EXCLUDED_TAGS]
    if phase == "native":
        return ",".join(excluded + ["-" + tag for tag in GPU_TAGS])
    return ",".join(list(GPU_TAGS) + excluded)


def selection(fallback, affected):
    """The target patterns: //... on a fallback, otherwise the affected labels."""
    if fallback == "true":
        return ["//..."]
    return affected.split()


def xcode_identity(root=ROOT):
    """`<version>-<build>` of the Xcode the config pins, from the config and the pin file."""
    rc = (root / ".bazelrc").read_text(encoding="utf-8")
    versions = re.findall(r"(?m)^build:remote-macos-base --xcode_version=(\S+)$", rc)
    if len(versions) != 1:
        raise ValueError("remote-macos-base must pin exactly one --xcode_version")
    pins = [line.split() for line in (root / PIN_FILE).read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")]
    builds = [build for version, build in pins if version == versions[0]]
    if len(builds) != 1:
        raise ValueError("%s does not pin Xcode %s exactly once" % (PIN_FILE, versions[0]))
    return "%s-%s" % (versions[0], builds[0])


def _phase_files(out, phase):
    directory = Path(out) / phase
    return {"console": directory / "console.log", "bep": directory / "bep.json",
            "profile": directory / "profile.gz", "exec_log": directory / "exec.log.zst"}


PROBES = ("lane", "plain")
# Run on the executor the config selects, to prove where the lane's actions really run: an
# executor bound under a config the remote macOS config expands, such as `re`, passes the probes
# above but sends the actions to another backend. A manual, external test that passes only on
# macOS arm64; it is run uncached, and its result must show it executed remotely in this
# invocation, so a result kept by a persistent output root cannot stand in for it.
ROUTING_PROBE = "//tools/ci:remote_macos_probe"
# The probe is one tiny action; an executor that schedules nothing for this long is unavailable.
ROUTING_STARVED_SECONDS = 10 * 60


def preflight_commands(bazel, startup, rc_options, out):
    """{probe: (command, build event file)}: `bazel build --nobuild` with the config (`lane`) and
    without it (`plain`), each writing its effective options to a build event file.

    A no-build probe analyzes nothing and runs no action, but Bazel may still connect to a
    configured executor, for example to read its capabilities.
    """
    commands = {}
    for probe in PROBES:
        events = Path(out) / "preflight" / ("%s.json" % probe)
        config = ["--config=" + CONFIG, *rc_options] if probe == "lane" else []
        commands[probe] = ([bazel, *startup, "build", *config, *CI_FLAGS, QUIET_REMOTE_MESSAGES,
                            "--nobuild", "--build_event_json_file=%s" % events], events)
    return commands


def phase_command(phase, bazel, startup, rc_options, out, targets):
    """The full command line of a test phase: the empty-selection wrapper, the stall watchdog
    and the Bazel command."""
    files = _phase_files(out, phase)
    config = CONFIG if phase == "native" else GPU_CONFIG
    command = [bazel, *startup, "test", "--config=" + config, *rc_options]
    if phase == "gpu":
        # After the config and the rc options, so it wins over a runner's own --jobs.
        command += ["--jobs=%d" % GPU_JOBS, "--build_tests_only"]
    command += [*LANE_FLAGS, "--test_tag_filters=" + tag_filters(phase),
                "--build_event_json_file=%s" % files["bep"],
                "--profile=%s" % files["profile"],
                "--execution_log_compact_file=%s" % files["exec_log"],
                *targets]
    return [TEST_WRAPPER, WATCHDOG, str(files["console"]), *command]


def routing_command(bazel, startup, rc_options, out):
    """(command, build event file, console) that runs the routing probe under the config."""
    directory = Path(out) / "preflight"
    events, console = directory / "routing.json", directory / "routing.log"
    return ([WATCHDOG, str(console), bazel, *startup, "test", "--config=" + CONFIG, *rc_options,
             *CI_FLAGS, QUIET_REMOTE_MESSAGES, "--nocache_test_results", "--test_tag_filters=",
             "--test_output=errors", "--build_event_json_file=%s" % events, ROUTING_PROBE],
            events, console)


def _probe_results(lines):
    """Every testResult payload of the routing probe in a build event file."""
    results = []
    for line in lines:
        if not line.strip():
            continue
        event = json.loads(line)
        if event.get("id", {}).get("testResult", {}).get("label") == ROUTING_PROBE:
            results.append(event.get("testResult", {}))
    return results


def _ran_remotely(result):
    info = result.get("executionInfo", {})
    return (not result.get("cachedLocally") and not info.get("cachedRemotely") and
            info.get("strategy") == "remote")


def probe_executed_remotely(lines):
    """Whether a build event file shows the routing probe passing in an uncached remote run."""
    return any(result.get("status") == "PASSED" and _ran_remotely(result)
               for result in _probe_results(lines))


def probe_ran_and_failed(lines):
    """Whether the routing probe ran remotely in this invocation and failed its check."""
    return any(result.get("status") == "FAILED" and _ran_remotely(result)
               for result in _probe_results(lines))


def _routing_problem(args, environment):
    """Why the routing probe does not prove the lane's actions run on macOS arm64, or None."""
    command, events, console = routing_command(args.bazel, args.startup_option, args.rc_option,
                                               args.out)
    status, starved = run_phase(command, environment, console, limit=ROUTING_STARVED_SECONDS)
    lines = _read_lines(events) or []
    events.unlink(missing_ok=True)
    if starved:
        return ("this runner is not configured for the remote macOS lane: the routing probe was "
                "not scheduled within %d minutes (%s)." % (ROUTING_STARVED_SECONDS // 60, STARVED))
    if status != 0 and probe_ran_and_failed(lines):
        return ("this runner is not configured for the remote macOS lane: the routing probe ran "
                "on the configured executor and reported a system other than macOS arm64 (exit "
                "%d)." % status)
    if status != 0:
        return ("the routing probe did not build or run on the configured executor (exit %d), so "
                "where the lane's actions run is not proven." % status)
    if not probe_executed_remotely(lines):
        return ("the routing probe is not proven: it did not run uncached on the remote executor "
                "in this invocation.")
    return None


def effective_executor(lines):
    """The remote executor a build event file's effective options set, or "" for none. Raises
    ValueError when the file has no canonical command line. Callers compare it and never print
    it."""
    for line in lines:
        if not line.strip():
            continue
        event = json.loads(line)
        if event.get("id", {}).get("structuredCommandLine", {}).get(
                "commandLineLabel") != "canonical":
            continue
        values = [option.get("optionValue") or ""
                  for section in event.get("structuredCommandLine", {}).get("sections", [])
                  for option in section.get("optionList", {}).get("option", [])
                  if option.get("optionName") == "remote_executor"]
        return values[-1] if values else ""
    raise ValueError("the build event file has no canonical command line")


_NOT_CONFIGURED = ("this runner is not configured for the remote macOS lane: its rc files set no "
                   "remote executor for --config=%s. Bind one under build:remote-macos-base "
                   "(docs/building.md, Remote macOS shadow lane)." % CONFIG)
_SAME_EXECUTOR = ("this runner is not configured for the remote macOS lane: --config=%s resolves "
                  "to the same remote executor as a build without it, so macOS actions would go "
                  "to the runner's other backend. Bind the macOS executor under "
                  "build:remote-macos-base only." % CONFIG)


def preflight_problem(statuses, executors):
    """Why the lane must not run, from both probes' exit statuses and executors, or None.
    `executors` maps a probe to its executor, or to None when its event file was unreadable."""
    failed = [status for status in statuses.values() if status != 0]
    if failed:
        return "the preflight Bazel command failed (exit %d)." % failed[0]
    if any(value is None for value in executors.values()):
        return "the preflight build event file could not be read."
    if not executors["lane"]:
        return _NOT_CONFIGURED
    if executors["lane"] == executors["plain"]:
        return _SAME_EXECUTOR
    return None


def _preflight(args, environment):
    """Runs both probes and returns (problem or None, exit status for a problem)."""
    statuses, executors = {}, {}
    for probe, (command, events) in preflight_commands(
            args.bazel, args.startup_option, args.rc_option, args.out).items():
        events.parent.mkdir(parents=True, exist_ok=True)
        statuses[probe] = _run(command, environment)
        lines = _read_lines(events)
        try:
            executors[probe] = None if lines is None else effective_executor(lines)
        except ValueError:
            executors[probe] = None
        # The event file records the runner's effective options; drop it at once.
        events.unlink(missing_ok=True)
    problem = preflight_problem(statuses, executors)
    failed = [status for status in statuses.values() if status != 0]
    if problem:
        return problem, (failed[0] if failed else 2)
    return _routing_problem(args, environment), 2


def _int(value):
    return int(value or 0)


_PASSED = ("PASSED",)
_FLAKY = ("FLAKY",)


def _metrics(metrics):
    """Action, cache, runner and wall-time counts of a buildMetrics event."""
    actions = metrics.get("actionSummary", {})
    cache = actions.get("actionCacheStatistics", {})
    wall = metrics.get("timingMetrics", {}).get("wallTimeInMs")
    return {"actions_executed": _int(actions.get("actionsExecuted")),
            "action_cache_hits": _int(cache.get("hits")),
            "action_cache_misses": _int(cache.get("misses")),
            "runners": {runner["name"]: _int(runner.get("count"))
                        for runner in actions.get("runnerCount", [])
                        if runner.get("name") not in (None, "", "total")},
            "wall_time_seconds": None if wall is None else round(_int(wall) / 1000.0, 1)}


def _on_target(event, state):
    succeeded = (event.get("completed") or {}).get("success")
    state["targets"]["succeeded" if succeeded else "failed"] += 1


def _on_test_summary(event, state):
    state["tests"][event["id"]["testSummary"]["label"]] = event.get("testSummary", {}).get(
        "overallStatus", "NO_STATUS")


def _on_test_result(event, state):
    result = event.get("testResult")
    cached = result is None or result.get("cachedLocally") or result.get(
        "executionInfo", {}).get("cachedRemotely")
    if not cached:
        state["executed"].add(event["id"]["testResult"]["label"])


def _on_finished(event, state):
    exit_code = event.get("finished", {}).get("exitCode", {})
    state["result"]["exit"] = "%s (%d)" % (exit_code.get("name", "SUCCESS"),
                                           _int(exit_code.get("code")))


def _on_metrics(event, state):
    state["result"].update(_metrics(event.get("buildMetrics", {})))


_HANDLERS = {"targetCompleted": _on_target, "testSummary": _on_test_summary,
             "testResult": _on_test_result, "buildFinished": _on_finished,
             "buildMetrics": _on_metrics}


def summarize(lines):
    """Counts of one phase's build event file. Option values are never read into it."""
    state = {"targets": {"succeeded": 0, "failed": 0}, "tests": {}, "executed": set(),
             "result": dict(_metrics({}), exit=None)}
    for event in (json.loads(line) for line in lines if line.strip()):
        handler = _HANDLERS.get(next(iter(event.get("id", {})), None))
        if handler:
            handler(event, state)
    tests, result = state["tests"], state["result"]
    statuses = list(tests.values())
    executed = len(state["executed"] & set(tests))
    result["targets"] = state["targets"]
    result["tests"] = {
        "total": len(statuses),
        "passed": sum(status in _PASSED for status in statuses),
        "flaky": sum(status in _FLAKY for status in statuses),
        "failed": sum(status not in _PASSED + _FLAKY for status in statuses),
        "executed": executed,
        "cached": len(statuses) - executed,
    }
    result["failed_tests"] = sorted(label for label, status in tests.items()
                                    if status not in _PASSED + _FLAKY)
    return result


_HEADER = ("| Phase | Exit status | Bazel result | Wall time (s) | Targets built | "
           "Targets failed | Tests | Passed | Flaky | Failed | Executed | Cached | Actions run | "
           "Action cache hits |")


def _row(phase, entry):
    if not entry:
        return "| %s | - | not run |%s" % (phase, " |" * 11)
    tests, targets = entry["tests"], entry["targets"]
    wall = entry["wall_time_seconds"]
    return "| %s | %s | %s | %s | %s |" % (
        phase, entry["status"], entry["exit"] or "-", "-" if wall is None else wall,
        " | ".join(str(value) for value in (
            targets["succeeded"], targets["failed"], tests["total"], tests["passed"],
            tests["flaky"], tests["failed"], tests["executed"], tests["cached"],
            entry["actions_executed"], entry["action_cache_hits"])))


def _details(phase, entry):
    lines = []
    if entry.get("runners"):
        lines.append("- %s spawns by runner: %s" % (phase, ", ".join(
            "%s %d" % (name, count) for name, count in sorted(entry["runners"].items()))))
    if entry.get("failed_tests"):
        lines.append("- %s failed tests: %s" % (phase, " ".join(entry["failed_tests"])))
    return lines


def report(summary):
    """The job summary: one row per phase, then the runner breakdown and any failed tests."""
    lines = ["## macOS remote-execution shadow lane (advisory)", "",
             "Builds and tests this pull request's selection for macOS from a Linux runner, "
             "every action on a remote macOS executor. Its result never fails the pull request.",
             ""]
    if summary.get("problem"):
        lines += ["**Stopped:** %s" % summary["problem"], ""]
    lines += [_HEADER, "|" + "---|" * 14]
    lines += [_row(phase, summary["phases"].get(phase)) for phase in PHASES]
    lines.append("")
    for phase in PHASES:
        lines += _details(phase, summary["phases"].get(phase) or {})
    return "\n".join(lines).rstrip() + "\n"


# A phase whose Bazel progress counter (`[done / total]`) has not advanced for this long while
# actions remain is stopped as "executor starved or unavailable". The shared stall watchdog cannot
# see this: Bazel rewrites its progress line with a growing elapsed time, so the line keeps
# changing while nothing completes. 20 minutes is longer than the slowest legitimate single action
# here (the longest test timeout in the tree is `long`, 900 seconds, plus queueing), and short
# enough that a starved executor releases its self-hosted Linux runner in minutes, not hours.
STARVED_SECONDS = 20 * 60
# The phase status after a starvation stop: the shell's timeout convention.
STARVED_STATUS = 124
POLL_SECONDS = 15
STARVED = "executor starved or unavailable"
# An optional `(HH:MM:SS)` prefix: a runner rc may turn on --show_timestamps.
_PROGRESS = re.compile(r"^(?:\(\d\d:\d\d:\d\d(?:\.\d+)?\) )?\[([\d,]+) / ([\d,]+)\]")


def progress(text):
    """(done, total) of the last Bazel progress line in console text, or None."""
    found = None
    for line in text.splitlines():
        match = _PROGRESS.match(line)
        if match:
            found = tuple(int(group.replace(",", "")) for group in match.groups())
    return found


class StarvationGuard:
    """Says when the done count has not advanced for `limit` seconds while actions remain."""

    def __init__(self, now, limit=STARVED_SECONDS):
        self.limit = limit
        self.done = None
        self.since = now

    def starved(self, counter, now):
        if counter is None or counter[0] >= counter[1]:
            # No progress line yet (loading, analysis), or nothing left to run.
            self.done, self.since = None, now
            return False
        if counter[0] != self.done:
            self.done, self.since = counter[0], now
            return False
        return now - self.since >= self.limit


def _tail(path, size=65536):
    try:
        with open(path, "rb") as handle:
            handle.seek(0, os.SEEK_END)
            handle.seek(max(0, handle.tell() - size))
            return handle.read().decode("utf-8", "replace")
    except OSError:
        return ""


def _stop(process, grace=(60, 30, 30)):
    """Interrupts the phase's process group as Ctrl-C would, up to three times (Bazel cancels
    the invocation on the first and kills its server on the third), then kills what is left."""
    for seconds in grace:
        try:
            os.killpg(process.pid, signal.SIGINT)
        except ProcessLookupError:
            return
        try:
            process.wait(timeout=seconds)
            return
        except subprocess.TimeoutExpired:
            continue
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def run_phase(command, environment, console, limit=STARVED_SECONDS, poll=POLL_SECONDS,
              clock=time.monotonic, grace=(60, 30, 30)):
    """Runs one phase command; returns (status, starved). The process runs in a session of its
    own, so a starvation stop reaches the wrappers and the Bazel client together."""
    print("+ " + shlex.join(command), flush=True)
    process = subprocess.Popen(command, cwd=ROOT, env=environment, start_new_session=True)
    guard = StarvationGuard(clock(), limit)
    while True:
        try:
            return process.wait(timeout=poll), False
        except subprocess.TimeoutExpired:
            pass
        if guard.starved(progress(_tail(console)), clock()):
            print("::warning::macOS remote shadow lane: no Bazel action completed for %ds while "
                  "actions remained; stopping the phase (%s)." % (limit, STARVED), flush=True)
            _stop(process, grace)
            return STARVED_STATUS, True


def _run(command, environment):
    print("+ " + shlex.join(command), flush=True)
    return subprocess.run(command, cwd=ROOT, env=environment, check=False).returncode


def _read_lines(path):
    try:
        return Path(path).read_text(encoding="utf-8").splitlines()
    except OSError:
        return None


def _finish(out, summary):
    Path(out).mkdir(parents=True, exist_ok=True)
    (Path(out) / "shadow-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    text = report(summary)
    (Path(out) / "report.md").write_text(text, encoding="utf-8")
    print(text)
    step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as handle:
            handle.write(text)


def run(args, environment):
    """Runs the preflight and both phases; returns the first failing status, or 0."""
    targets = selection(args.fallback, args.affected)
    if not targets:
        print("::error::Empty target selection reached the macOS remote shadow lane.")
        return 2
    summary = {"version": 1, "targets": len(targets), "fallback": args.fallback == "true",
               "phases": {}}
    try:
        return _run_lane(args, environment, targets, summary)
    finally:
        _shutdown(args, environment)


def _shutdown(args, environment):
    """Leaves no Bazel server behind on the runner, even after a starvation stop."""
    try:
        subprocess.run(shutdown_command(args.bazel, args.startup_option), cwd=ROOT,
                       env=environment, check=False, timeout=SHUTDOWN_SECONDS)
    except (subprocess.TimeoutExpired, OSError) as error:
        print("::warning::macOS remote shadow lane: the Bazel server was not shut down (%s)."
              % type(error).__name__)


def _run_lane(args, environment, targets, summary):
    """The preflight and both phases; returns the first failing status, or 0."""
    problem, status = _preflight(args, environment)
    if problem:
        summary["problem"] = problem
        print("::error::macOS remote shadow lane: %s" % problem)
        _finish(args.out, summary)
        return status
    first_failure = 0
    for phase in PHASES:
        if first_failure == STARVED_STATUS:
            # The executor gave no progress; another phase would only wait again.
            summary["phases"][phase] = None
            continue
        files = _phase_files(args.out, phase)
        files["bep"].parent.mkdir(parents=True, exist_ok=True)
        started = time.monotonic()
        status, starved = run_phase(phase_command(phase, args.bazel, args.startup_option,
                                                  args.rc_option, args.out, targets),
                                    environment, files["console"])
        lines = _read_lines(files["bep"]) or []
        entry = summarize(lines)
        entry["status"] = status
        if starved:
            entry["outcome"] = STARVED
            summary["problem"] = "the %s phase was stopped: %s (no action completed for %d " \
                "minutes while actions remained)." % (phase, STARVED, STARVED_SECONDS // 60)
        if entry["wall_time_seconds"] is None:
            entry["wall_time_seconds"] = round(time.monotonic() - started, 1)
        summary["phases"][phase] = entry
        if status != 0:
            print("::warning::macOS remote shadow lane: the %s phase exited %d (advisory; the "
                  "pull request is not failed by it)." % (phase, status))
            first_failure = first_failure or status
    _finish(args.out, summary)
    return first_failure


def parse_args(argv, environ):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out", required=True)
    parser.add_argument("--fallback", default=environ.get("FALLBACK", ""))
    parser.add_argument("--affected", default=environ.get("AFFECTED", ""))
    parser.add_argument("--bazel", default="bazelisk")
    parser.add_argument("--startup-option", action="append", default=[])
    parser.add_argument("--output-user-root", default="",
                        help="the lane's own Bazel output root, passed to every Bazel command")
    parser.add_argument("--rc-option", action="append", default=[])
    return parser.parse_args(argv)


def main(argv=None, environ=None):
    environ = dict(os.environ if environ is None else environ)
    args = parse_args(sys.argv[1:] if argv is None else argv, environ)
    identity = xcode_identity()
    if environ.get(IDENTITY, identity) != identity:
        print("::error::%s=%s contradicts the config's Xcode %s." % (
            IDENTITY, environ[IDENTITY], identity))
        return 2
    environ[IDENTITY] = identity
    if args.output_user_root:
        args.startup_option = ["--output_user_root=" + args.output_user_root,
                               *args.startup_option]
    # A server left behind by a killed job must not hold a gating runner's memory for Bazel's
    # default three hours.
    args.startup_option = [*args.startup_option, MAX_IDLE]
    if args.dry_run:
        targets = selection(args.fallback, args.affected) or ["<empty selection>"]
        print("%s=%s" % (IDENTITY, identity))
        for command, _ in preflight_commands(args.bazel, args.startup_option, args.rc_option,
                                             args.out).values():
            print(shlex.join(command))
        print(shlex.join(routing_command(args.bazel, args.startup_option, args.rc_option,
                                         args.out)[0]))
        for phase in PHASES:
            print(shlex.join(phase_command(phase, args.bazel, args.startup_option,
                                           args.rc_option, args.out, targets)))
        print(shlex.join(shutdown_command(args.bazel, args.startup_option)))
        return 0
    return run(args, environ)


if __name__ == "__main__":
    sys.exit(main())
