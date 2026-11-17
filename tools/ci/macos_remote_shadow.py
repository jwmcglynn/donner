#!/usr/bin/env python3
"""Runs the advisory macOS remote-execution shadow lane and summarizes what it did.

The lane builds and tests the pull request's selected targets for macOS from a Linux runner, with
every action on a remote macOS executor (`--config=remote-macos`, docs/building.md), so its speed,
cache reuse and failures can be compared with the self-hosted macOS lane before anything depends
on it. It never gates a pull request: the workflow job ignores its result, and this script reports
what happened in the job summary and in a derived summary file instead.

It runs, in order:

  preflight  `bazel build --nobuild` with and without the config, then an uncached run of a
             one-line routing probe test under the config. Fails at once, with a message of its
             own, when the runner's rc files configure no remote executor for the
             config, the same executor as without it, or one that does not run the probe on
             macOS arm64 in this invocation.
  native     `bazel test --config=remote-macos` of the selection: builds every selected target and
             runs every selected test except the ones listed in EXCLUDED_TAGS and the GPU-opening
             tests (GPU_TAGS).
  gpu        `bazel test --config=remote-macos-gpu --jobs=4 --build_tests_only` of the GPU-opening
             tests of the selection, on executors that have a GPU.

After each phase it copies the logs and undeclared outputs of the tests that did not pass in that
phase, as its build event file names them, to OUT/test-artifacts/PHASE/ for the workflow's
test-artifact collector: only files inside the lane's output root and reached without symbolic
links, at most MAX_ARTIFACT_FILE_BYTES each and MAX_ARTIFACT_TOTAL_BYTES per run.

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
  tools/ci/macos_remote_shadow.py [--dry-run] [--out DIR] [--output-user-root DIR]
      [--budget-minutes N] [--targets PATTERN... | --fallback true|false --affected 'LABEL ...']
      [--bazel PATH] [--startup-option OPT]... [--rc-option OPT]...

CI runs it through the lane table (tools/ci/lanes.py, lane macos-remote-shadow), which passes the
planner's selection as --targets: //... on a fallback, otherwise the affected labels. Without
--targets, --fallback and --affected default to the FALLBACK and AFFECTED environment variables.
--out defaults to $RUNNER_TEMP/macos-remote-shadow and --output-user-root to
$RUNNER_TOOL_CACHE/donner-macos-remote-shadow-output.
--budget-minutes is the lane's own wall-clock budget (see REPORT_MARGIN_MINUTES): when it runs out,
the running command is stopped, the later phase skipped and the report written.
--rc-option stands in for runner rc configuration in a manual run: each one is placed right after
the config, where the config's own rc lines apply. --dry-run prints each Bazel command line and
runs nothing.
"""

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import time
from urllib.parse import unquote, urlparse

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

# Repository rules and module extensions run on this Linux client, never on the macOS executor.
# Bazel 8 otherwise also gives them every --action_env the remote macOS config sets: the macOS PATH
# of ci-macos, and any variable a runner's rc empties for the executor, such as a library path, so
# a local repository fetch can lose what the client's own tools need. With this flag they see the
# client's environment plus --repo_env. Every Bazel command of the lane passes it, after the
# config and the rc options, so the lane's output root never mixes two repository environments;
# no other lane passes it.
CLIENT_REPO_ENV = "--incompatible_repo_env_ignores_action_env"

# The editor's build identity action reads the checkout's commit and must then run on the client,
# which this config cannot do. An explicit commit makes it an ordinary remote action. This lane
# ships nothing, so it takes the fixed placeholder commit from the narrow test-lane config that
# `ci` composes for every non-release lane.
TEST_LANE_CONFIG = "--config=ci-test-lane"

LANE_TEST_ENV = "--test_env=DONNER_CI_MACOS_LANE=remote-shadow"

# Remote timeouts, fallback and every other remote setting come from the config and the runner's
# rc; a command-line value here would override the runner's.
LANE_FLAGS = (CLIENT_REPO_ENV,) + CI_FLAGS + (
    # Tests run remotely; only their logs come back.
    "--remote_download_outputs=minimal",
    r"--remote_download_regex=.*(test\.log|test\.xml|outputs\.zip)$",
    TEST_LANE_CONFIG,
    "--test_output=errors",
    # The lane is advisory: one failed action, even an executor-side error, must not hide the
    # result of everything that does not depend on it. The gating lanes stop at the first error.
    "--keep_going",
    # Test results of this lane are its own; build actions are shared.
    LANE_TEST_ENV,
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


def selection(fallback, affected, targets=None):
    """The target patterns: `targets` when given (the lane table passes the planner's selection),
    else //... on a fallback, otherwise the affected labels."""
    if targets is not None:
        return list(targets)
    if fallback == "true":
        return ["//..."]
    return affected.split()


# Where the lane writes, by default: its output directory under the runner's temporary directory,
# and its own Bazel output root in the runner's persistent tool cache, outside the workspace, so
# the Linux lane's server and analysis cache are left alone and the lane keeps its repository and
# analysis caches between runs.
OUT_NAME = "macos-remote-shadow"
OUTPUT_ROOT_NAME = "donner-macos-remote-shadow-output"


def _default_locations(args, environ):
    """Fills --out and --output-user-root from RUNNER_TEMP and RUNNER_TOOL_CACHE when unset."""
    if not args.out and environ.get("RUNNER_TEMP"):
        args.out = os.path.join(environ["RUNNER_TEMP"], OUT_NAME)
    if args.output_user_root is None:
        cache = environ.get("RUNNER_TOOL_CACHE")
        args.output_user_root = os.path.join(cache, OUTPUT_ROOT_NAME) if cache else ""
    return bool(args.out)


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
        commands[probe] = ([bazel, *startup, "build", *config, CLIENT_REPO_ENV, *CI_FLAGS,
                            "--nobuild",
                            "--build_event_json_file=%s" % events], events)
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
             CLIENT_REPO_ENV, *CI_FLAGS,
             # The native phase's analysis options, so that phase reuses this analysis.
             TEST_LANE_CONFIG, LANE_TEST_ENV,
             "--nocache_test_results", "--test_tag_filters=",
             "--test_output=errors", "--build_event_json_file=%s" % events, ROUTING_PROBE],
            events, console)


def _decoded(lines):
    """The events of a build event file. A line that is not a JSON object is skipped: a Bazel
    server stopped mid-write leaves a partial last line, and the lane must still report and stage
    what the rest of the file says."""
    for line in lines:
        try:
            event = json.loads(line)
        except (ValueError, RecursionError):
            continue
        if isinstance(event, dict):
            yield event


def _dict(value):
    """`value` when it is a JSON object, else an empty one: build event fields are read without
    trusting their types."""
    return value if isinstance(value, dict) else {}


def _probe_results(lines):
    """Every testResult payload of the routing probe in a build event file."""
    results = []
    for event in _decoded(lines):
        if _dict(_dict(event.get("id")).get("testResult")).get("label") == ROUTING_PROBE:
            results.append(_dict(event.get("testResult")))
    return results


def _ran_remotely(result):
    info = _dict(result.get("executionInfo"))
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
    """(why the routing probe does not prove the lane's actions run on macOS arm64, exit status)
    or (None, 0)."""
    command, events, console = routing_command(args.bazel, args.startup_option, args.rc_option,
                                               args.out)
    status, stop = run_phase(command, environment, console, limit=ROUTING_STARVED_SECONDS,
                             deadline=args.deadline)
    if stop is not None:
        # The server finishes the cancelled command and its build event file only now.
        _shutdown(args, environment)
    lines = _read_lines(events) or []
    events.unlink(missing_ok=True)
    if stop == OUT_OF_TIME:
        return ("the routing probe was stopped: %s (the lane's budget ran out)." % OUT_OF_TIME,
                OUT_OF_TIME_STATUS)
    return _routing_result(stop, status, lines), 2


def _routing_result(stop, status, lines):
    """Why the routing probe's result does not prove where the lane's actions run, or None."""
    if stop:
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


# What Bazel prints when a repository rule could not run on the client. The lane then stops before
# anything reached the executor.
_REPOSITORY_FAILURES = ("Error computing the main repository mapping",
                        "error while loading shared libraries")
_REPOSITORY_PROBLEM = (
    "a repository rule failed on this runner before anything ran remotely (exit %d). Repository "
    "rules run on the Linux client with the client's environment (the lane passes " +
    CLIENT_REPO_ENV + "); check that the runner's rc does not take an environment variable the "
    "client's own tools need, such as the library path, away from them, for example with "
    "--repo_env.")


def _preflight(args, environment):
    """Runs the probes in order, stopping at the first that fails, and returns (problem or None,
    exit status for a problem)."""
    statuses, executors = {}, {}
    for probe, (command, events) in preflight_commands(
            args.bazel, args.startup_option, args.rc_option, args.out).items():
        statuses[probe], executors[probe], stopped = _probe(command, events, environment,
                                                            args.deadline)
        if stopped:
            return stopped, statuses[probe]
        if statuses[probe] != 0:
            # A later probe would only repeat the failure, or run a no-target build for nothing.
            break
    problem = preflight_problem(statuses, executors)
    failed = [status for status in statuses.values() if status != 0]
    if problem:
        return problem, (failed[0] if failed else 2)
    return _routing_problem(args, environment)


def _probe(command, events, environment, deadline=None):
    """Runs one preflight probe: (exit status, executor or None when unreadable, a problem for a
    probe that ran out of time or whose repository rule failed on the client, or None). Its
    build event file is deleted at once, since it records the runner's effective options."""
    events.parent.mkdir(parents=True, exist_ok=True)
    status, output, stopped = _run_captured(command, environment, events.with_suffix(".log"),
                                            deadline)
    problem = ("the preflight was stopped: %s (the lane's budget ran out)." % OUT_OF_TIME
               if stopped else None)
    if problem is None and status != 0 and any(marker in output for marker in _REPOSITORY_FAILURES):
        problem = _REPOSITORY_PROBLEM % status
    lines = _read_lines(events)
    events.unlink(missing_ok=True)
    try:
        executor = None if lines is None else effective_executor(lines)
    except Exception:  # Any unreadable event file is reported as unreadable.
        executor = None
    return status, executor, problem


def _int(value):
    return int(value or 0)


_PASSED = ("PASSED",)
_FLAKY = ("FLAKY",)
# Tests that never ran: not built (a build failure, which with --keep_going fails only the targets
# that depend on the failed action) or never started.
_NOT_RUN = ("NO_STATUS", "FAILED_TO_BUILD", "TOOL_HALTED_BEFORE_TESTING")
# Exit codes Bazel gives an action that failed in remote execution or the remote cache rather than
# by its own exit status: REMOTE_ERROR (34) and REMOTE_CACHE_EVICTED (39).
_REMOTE_EXIT_CODES = (34, 39)


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


def _on_action(event, state):
    """Counts a failed action as a remote execution error or a failure of its own. Bazel reports
    an action that failed in remote execution with a remote exit code, and its failure detail as
    a remoteExecution one; an action whose own command failed has that command's exit status."""
    action = event.get("action")
    if not isinstance(action, dict) or action.get("success") is True:
        return
    detail = action.get("failureDetail")
    remote = action.get("exitCode") in _REMOTE_EXIT_CODES or (
        isinstance(detail, dict) and "remoteExecution" in detail)
    state["failed_actions"]["remote" if remote else "own"] += 1


def _configuration(identity):
    """The configuration id of a build event id, or "" when it has none."""
    return _dict(identity.get("configuration")).get("id") or ""


def _on_configured(event, state):
    # A test target is configured with a test size; Bazel reports no other kind with one.
    if "testSize" in _dict(event.get("configured")):
        state["test_labels"].add(_dict(_dict(event.get("id")).get("targetConfigured")).get("label"))


# Reasons Bazel aborts a target with that are failures of the target itself. SKIPPED is a target
# incompatible with the target platform; any other reason, such as INCOMPLETE or USER_INTERRUPTED,
# is a build that stopped before reaching the target.
_FAILED_ABORT_REASONS = ("ANALYSIS_FAILURE", "LOADING_FAILURE")


def _on_target(event, state):
    """A target is built (completed with success), failed (completed without it, or aborted as an
    analysis or loading failure), skipped (aborted as SKIPPED: incompatible with the target
    platform, which --skip_incompatible_explicit_targets skips without failing) or aborted for
    another reason, such as a stopped build."""
    identity = _dict(_dict(event.get("id")).get("targetCompleted"))
    key = (identity.get("label"), _configuration(identity))
    if "aborted" in event:
        reason = _dict(event.get("aborted")).get("reason")
        outcome = "skipped" if reason == "SKIPPED" else \
            "failed" if reason in _FAILED_ABORT_REASONS else "aborted"
    else:
        outcome = "succeeded" if _dict(event.get("completed")).get("success") is True \
            else "failed"
    state["targets"][outcome] += 1
    state["target_outcomes"][key] = outcome


def _on_test_summary(event, state):
    identity = _dict(_dict(event.get("id")).get("testSummary"))
    state["tests"][(identity.get("label"), _configuration(identity))] = _dict(
        event.get("testSummary")).get("overallStatus", "NO_STATUS")


def _on_test_result(event, state):
    result = event.get("testResult")
    cached = result is None or result.get("cachedLocally") or result.get(
        "executionInfo", {}).get("cachedRemotely")
    if not cached:
        identity = _dict(_dict(event.get("id")).get("testResult"))
        state["executed"].add((identity.get("label"), _configuration(identity)))


def _on_finished(event, state):
    exit_code = event.get("finished", {}).get("exitCode", {})
    state["result"]["exit"] = "%s (%d)" % (exit_code.get("name", "SUCCESS"),
                                           _int(exit_code.get("code")))


def _on_metrics(event, state):
    state["result"].update(_metrics(event.get("buildMetrics", {})))


_HANDLERS = {"actionCompleted": _on_action, "targetConfigured": _on_configured,
             "targetCompleted": _on_target, "testSummary": _on_test_summary,
             "testResult": _on_test_result, "buildFinished": _on_finished,
             "buildMetrics": _on_metrics}


def summarize(lines):
    """Counts of one phase's build event file. Option values are never read into it.

    Bazel reports a test that ran in a testSummary event. A test target that was skipped or failed
    to build has no testSummary, only its targetCompleted event: those are counted from the
    targets, as skipped and as not run, so the totals match Bazel's own test line."""
    state = {"targets": {"succeeded": 0, "failed": 0, "skipped": 0, "aborted": 0}, "tests": {},
             "executed": set(), "test_labels": set(), "target_outcomes": {},
             "failed_actions": {"remote": 0, "own": 0}, "result": dict(_metrics({}), exit=None)}
    for event in _decoded(lines):
        handler = _HANDLERS.get(next(iter(_dict(event.get("id"))), None))
        if handler:
            handler(event, state)
    tests, result = state["tests"], state["result"]
    statuses = list(tests.values())
    unsummarized = [outcome for (label, configuration), outcome in state["target_outcomes"].items()
                    if label in state["test_labels"] and (label, configuration) not in tests]
    executed = len(state["executed"] & set(tests))
    result["targets"] = state["targets"]
    result["tests"] = {
        "total": len(statuses) + len(unsummarized),
        "passed": sum(status in _PASSED for status in statuses),
        "flaky": sum(status in _FLAKY for status in statuses),
        "failed": sum(status not in _PASSED + _FLAKY + _NOT_RUN for status in statuses),
        "not_run": sum(status in _NOT_RUN for status in statuses) +
        sum(outcome in ("failed", "aborted") for outcome in unsummarized),
        "skipped": sum(outcome == "skipped" for outcome in unsummarized),
        "executed": executed,
        "cached": len(statuses) - executed,
    }
    result["failed_tests"] = sorted(label for (label, _), status in tests.items()
                                    if status not in _PASSED + _FLAKY + _NOT_RUN)
    result["failed_actions"] = state["failed_actions"]
    return result


# Where a phase stages its failed tests' logs and undeclared outputs: OUT/test-artifacts/PHASE/,
# laid out as CONFIGURATION/PACKAGE/TARGET/... like Bazel's testlogs trees, for the workflow's
# test-artifact collector (.github/actions/upload-bazel-test-artifacts), which keeps test.log,
# test.xml and anything below test.outputs/ wherever they are. Keeping the configuration keeps a
# test that ran in two configurations from overwriting itself.
ARTIFACTS = "test-artifacts"
# What one file and one run may stage. A larger file is skipped with a warning.
MAX_ARTIFACT_FILE_BYTES = 64 * 1024 * 1024
MAX_ARTIFACT_TOTAL_BYTES = 256 * 1024 * 1024
_COPY_CHUNK = 1024 * 1024
# The files the collector keeps: a final attempt's log and XML, and its undeclared outputs.
# Earlier attempts of a retried test (test_attempts/attempt_N.*) are not kept.
_COLLECTED_NAMES = ("test.log", "test.xml")


def _output_path(uri):
    """The local path a build event output URI names, or None. It must be a `file` URI without a
    host whose path is absolute, holds no NUL and has no empty, `.` or `..` segment."""
    if not isinstance(uri, str):
        return None
    try:
        parsed = urlparse(uri)
    except ValueError:
        return None
    if parsed.scheme != "file" or parsed.netloc not in ("", "localhost"):
        return None
    path = unquote(parsed.path)
    segments = path.split("/")
    if "\0" in path or segments[0] != "" or any(part in ("", ".", "..") for part in segments[1:]):
        return None
    return path


def _staged_key(path):
    """CONFIGURATION/PATH BELOW TESTLOGS of a test output the collector keeps, or None when the
    path names no single `bazel-out/CONFIGURATION/testlogs/` directory."""
    segments = path.split("/")
    found = [index for index in range(len(segments) - 3)
             if segments[index] == "bazel-out" and segments[index + 2] == "testlogs"]
    if len(found) != 1:
        return None
    key = PurePosixPath(segments[found[0] + 1], *segments[found[0] + 3:])
    if key.name in _COLLECTED_NAMES or "test.outputs" in key.parts[:-1]:
        return key
    return None


def failed_test_outputs(lines):
    """[(staged key, local path, label)] of every collected output that a test result of this
    invocation that did not pass names, each key once. The lane's output root persists between
    runs, so files are selected only from this invocation's own results; a passed test contributes
    nothing. The lane disables build event path conversion, so outputs are named by local file.
    Fields of an unexpected type are skipped."""
    outputs, seen = [], set()
    for event in _decoded(lines):
        label, listed = _failed_result(event)
        for key, path in filter(None, map(_collected_output, listed)):
            if key not in seen:
                seen.add(key)
                outputs.append((key, path, label))
    return outputs


def _failed_result(event):
    """(label, listed outputs) of a test result event that did not pass, or (None, [])."""
    identity = _dict(_dict(event.get("id")).get("testResult"))
    result = event.get("testResult")
    if not identity or not isinstance(result, dict) or result.get("status") == "PASSED":
        return None, []
    label = identity.get("label") if isinstance(identity.get("label"), str) else "a test"
    listed = result.get("testActionOutput")
    return label, listed if isinstance(listed, list) else []


def _collected_output(output):
    """(staged key, local path) of one listed output the collector keeps, or None."""
    path = _output_path(_dict(output).get("uri"))
    key = _staged_key(path) if path else None
    return None if key is None else (key, path)


def _contained_source(path, output_root):
    """The real path of a test output, or None unless it lies below the lane's output root and no
    component below the root is a symbolic link: its real path must be the root's real path plus
    the same relative path. The root itself may be reached through a link."""
    real_root = os.path.realpath(output_root)
    for root in dict.fromkeys((os.path.normpath(os.path.abspath(output_root)), real_root)):
        prefix = root.rstrip("/") + "/"
        if path.startswith(prefix):
            expected = os.path.normpath(os.path.join(real_root, path[len(prefix):]))
            if os.path.commonpath([real_root, expected]) != real_root:
                return None
            return expected if os.path.realpath(path) == expected else None
    return None


def _open_regular(path):
    """(read descriptor, size) of a regular file with one link, or None. A final symbolic link is
    not followed, a hard link (which can name a file outside the output root) is refused, and the
    type, link count and size come from the opened descriptor, so the file cannot be swapped
    between the check and the copy."""
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK |
                             getattr(os, "O_CLOEXEC", 0))
    except OSError:
        return None
    info = os.fstat(descriptor)
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        os.close(descriptor)
        return None
    return descriptor, info.st_size


def _copy_bounded(source, destination, limit):
    """Copies descriptor to descriptor; returns the bytes copied, or None when the source holds
    more than `limit` bytes, for example because it grew after it was measured."""
    copied = 0
    while True:
        chunk = os.read(source, min(_COPY_CHUNK, limit - copied + 1))
        if not chunk:
            return copied
        copied += len(chunk)
        if copied > limit:
            return None
        view = memoryview(chunk)
        while view:
            view = view[os.write(destination, view):]


def _destination(stage_root, key):
    """The staged file of a key. Raises ValueError unless it stays inside the stage root."""
    key = PurePosixPath(key)
    if key.is_absolute() or not key.parts or any(part in (".", "..") for part in key.parts):
        raise ValueError("a staged key must be a relative path without . or ..")
    destination = Path(stage_root) / key
    destination.parent.mkdir(parents=True, exist_ok=True)
    real_root = os.path.realpath(stage_root)
    if os.path.commonpath([real_root, os.path.realpath(destination.parent)]) != real_root:
        raise ValueError("a staged file would leave the stage root")
    return destination


def _printable(text):
    return "".join(character if character.isprintable() else "?" for character in str(text))


def _skip(label, key, size, cap, which):
    print("::warning::macOS remote shadow lane: %s of %s is not uploaded: %d bytes is over the "
          "%s cap of %d bytes." % (_printable(key), _printable(label), size, which, cap))


def _stage_one(stage_root, key, source, limit):
    """Copies one opened source to its staged file; returns the bytes copied, or None when the
    source exceeded `limit` while it was copied."""
    destination = _destination(stage_root, key)
    target = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
    try:
        copied = _copy_bounded(source, target, limit)
    except BaseException:
        os.close(target)
        destination.unlink()
        raise
    os.close(target)
    if copied is None:
        destination.unlink()
    return copied


def stage_failed_test_outputs(lines, out, phase, output_root, budget, staged=None):
    """Copies a phase's failed tests' collected outputs that Bazel downloaded (by the lane's
    download regex) and that lie inside the lane's output root to OUT/test-artifacts/PHASE/.
    `budget` is how many bytes this run may still stage. Returns (files, bytes) staged, which it
    also keeps current in `staged` as it goes, so a caller knows them even if it raises."""
    staged = {"files": 0, "bytes": 0} if staged is None else staged
    stage_root = Path(out) / ARTIFACTS / phase
    if not _cleared_stage_root(stage_root, phase):
        return 0, 0
    files = used = 0
    for key, path, label in failed_test_outputs(lines):
        copied = _stage_output(stage_root, key, path, label, output_root, budget - used)
        if copied is not None:
            files, used = files + 1, used + copied
            staged.update(files=files, bytes=used)
    return files, used


def _cleared_stage_root(stage_root, phase):
    """Removes an earlier stage; False, with a warning, when the stage root or its parent is a
    symbolic link. A stage root that cannot be removed raises."""
    if any(path.is_symlink() for path in (stage_root.parent, stage_root)):
        print("::warning::macOS remote shadow lane: %s is a symbolic link; nothing of the %s "
              "phase is staged." % (ARTIFACTS, phase))
        return False
    if os.path.lexists(stage_root):
        shutil.rmtree(stage_root)
    return True


def _stage_output(stage_root, key, path, label, output_root, remaining):
    """Stages one output within the caps, `remaining` bytes being what the run has left; returns
    the bytes copied, or None when nothing was staged."""
    # Residual window: a directory below the output root could be swapped for a link after this
    # check and before the open below. Only a concurrent process on the runner can do that, and
    # the lane's trust gate already lets such a pull request run code there.
    source = _contained_source(path, output_root)
    opened = _open_regular(source) if source else None
    if opened is None:
        return None
    descriptor, size = opened
    try:
        cap, which = min((MAX_ARTIFACT_FILE_BYTES, "per-file"), (remaining, "per-run"))
        if size > cap:
            _skip(label, key, size, cap, which)
            return None
        try:
            copied = _stage_one(stage_root, key, descriptor, cap)
        except (OSError, ValueError) as error:
            print("::warning::macOS remote shadow lane: %s of %s is not staged (%s)."
                  % (_printable(key), _printable(label), type(error).__name__))
            return None
        if copied is None:
            _skip(label, key, size, cap, which + " (it grew while it was copied)")
        return copied
    finally:
        os.close(descriptor)


_HEADER = ("| Phase | Exit status | Bazel result | Wall time (s) | Targets built | "
           "Targets failed | Targets skipped | Tests | Passed | Flaky | Failed | Not run | "
           "Skipped | Executed | Cached | Actions run | Action cache hits |")


def _row(phase, entry):
    if not entry:
        return "| %s | - | not run |%s" % (phase, " |" * 14)
    tests, targets = entry["tests"], entry["targets"]
    wall = entry["wall_time_seconds"]
    return "| %s | %s | %s | %s | %s |" % (
        phase, entry["status"], entry["exit"] or "-", "-" if wall is None else wall,
        " | ".join(str(value) for value in (
            targets["succeeded"], targets["failed"], targets["skipped"], tests["total"],
            tests["passed"], tests["flaky"], tests["failed"], tests["not_run"], tests["skipped"],
            tests["executed"], tests["cached"],
            entry["actions_executed"], entry["action_cache_hits"])))


def _stopped_details(phase, entry):
    """What a stopped phase got through, so a cold cache reads differently from starvation."""
    counts = entry.get("progress") or {}

    def pair(done, total):
        return "%s / %s" % ("-" if done is None else done, "-" if total is None else total)

    return ["- %s stopped (%s) after %s s: %s actions, %s tests, %s actions per minute; build "
            "events: %d actions executed, %d action cache hits" % (
                phase, entry["outcome"], entry["wall_time_seconds"],
                pair(counts.get("actions_done"), counts.get("actions_total")),
                pair(counts.get("tests_done"), counts.get("tests_total")),
                "-" if entry.get("actions_per_minute") is None else entry["actions_per_minute"],
                entry["actions_executed"], entry["action_cache_hits"])]


def _details(phase, entry):
    lines = _stopped_details(phase, entry) if entry.get("outcome") else []
    if (entry.get("targets") or {}).get("aborted"):
        lines.append("- %s: %d targets were not built because the build stopped before them" % (
            phase, entry["targets"]["aborted"]))
    if entry.get("runners"):
        lines.append("- %s spawns by runner: %s" % (phase, ", ".join(
            "%s %d" % (name, count) for name, count in sorted(entry["runners"].items()))))
    if any((entry.get("failed_actions") or {}).values()):
        lines.append("- %s: %s" % (phase, failed_actions_line(entry["failed_actions"])))
    if entry.get("failed_tests"):
        lines.append("- %s failed tests: %s" % (phase, " ".join(entry["failed_tests"])))
    return lines


def failed_actions_line(failed):
    """How a phase's failed actions failed, so an executor fault is not read as a build break."""
    return ("%d actions failed with a remote execution error (executor side), %d with their own "
            "exit status" % (failed["remote"], failed["own"]))


def report(summary):
    """The job summary: one row per phase, then the runner breakdown and any failed tests."""
    lines = ["## macOS remote-execution shadow lane (advisory)", "",
             "Builds and tests this pull request's selection for macOS from a Linux runner, "
             "every action on a remote macOS executor. Its result never fails the pull request.",
             ""]
    if summary.get("problem"):
        lines += ["**Stopped:** %s" % summary["problem"], ""]
    lines += [_HEADER, "|" + "---|" * 17]
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

# The lane's own wall-clock budget, --budget-minutes, which the workflow step passes. It is a
# deadline on this advisory lane's runtime, so that the lane stops itself and reports before the
# job's timeout-minutes cancels it with nothing written; it is not a deadline on any test or on
# correctness. When it runs out, the running Bazel command is stopped like a starved phase and the
# later phase is skipped. REPORT_MARGIN_MINUTES is what the job still needs after the budget, at
# worst: STOP_GRACE plus STOP_KILL_SECONDS to stop the command (150 s), SHUTDOWN_SECONDS to shut
# the server down so its build event file is complete, SHUTDOWN_SECONDS again for the final
# shutdown, then staging, the summary and UPLOAD_MINUTES for the upload steps: 12.5 minutes in
# all. The workflow's budget plus this margin stays below the job's timeout-minutes, and the margin
# covers that sum (//tools:ci_macos_remote_shadow_tests).
REPORT_MARGIN_MINUTES = 15
UPLOAD_MINUTES = 5
OUT_OF_TIME = "out of time"
OUT_OF_TIME_STATUS = 124
_STOP_STATUS = {STARVED: STARVED_STATUS, OUT_OF_TIME: OUT_OF_TIME_STATUS}
# An optional `(HH:MM:SS)` prefix: a runner rc may turn on --show_timestamps.
_PROGRESS = re.compile(r"^(?:\(\d\d:\d\d:\d\d(?:\.\d+)?\) )?\[([\d,]+) / ([\d,]+)\]")


_PROGRESS_TESTS = re.compile(r"\] ([\d,]+) / ([\d,]+) tests?\b")


def _last_progress_line(text):
    lines = [line for line in text.splitlines() if _PROGRESS.match(line)]
    return lines[-1] if lines else None


def _counts(match):
    return tuple(int(group.replace(",", "")) for group in match.groups())


def progress(text):
    """(done, total) of the last Bazel progress line in console text, or None."""
    line = _last_progress_line(text)
    return _counts(_PROGRESS.match(line)) if line else None


def progress_counts(text):
    """Actions and tests done and total of the last Bazel progress line, or None."""
    line = _last_progress_line(text)
    if line is None:
        return None
    actions = _counts(_PROGRESS.match(line))
    tests = _PROGRESS_TESTS.search(line)
    tests = _counts(tests) if tests else (None, None)
    return {"actions_done": actions[0], "actions_total": actions[1],
            "tests_done": tests[0], "tests_total": tests[1]}


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


# Interrupts sent to a command's process group before it is killed, and the wait after the kill.
STOP_GRACE = (60, 30, 30)
STOP_KILL_SECONDS = 30
# Once the command exited: how long the rest of its group may take to finish, then after a
# terminate, then after a kill. The worst case of a stop stays STOP_GRACE plus STOP_KILL_SECONDS.
_LEFTOVER_SECONDS = 10


def _stop(process, grace=STOP_GRACE):
    """Interrupts the command's process group as Ctrl-C would, up to three times (Bazel cancels
    the invocation on the first and kills its server on the third), then kills the group. Once
    the command itself has exited, what is left of its group is terminated: the stall watchdog's
    heartbeat is a shell background job, which ignores interrupts and would otherwise linger
    until its next heartbeat. The Bazel server is not in the group; it finishes cancelling the
    command and writing the build event file on its own, so callers shut it down before they
    read that file."""
    for seconds in grace:
        if not _signal_group(process.pid, signal.SIGINT):
            _reap(process)
            return
        try:
            process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            continue
        _end_group(process.pid)
        return
    _signal_group(process.pid, signal.SIGKILL)
    _group_gone(process.pid, STOP_KILL_SECONDS)
    _reap(process)


def _signal_group(group, sig):
    """Sends `sig` (0 only probes) to a process group. True when the group has a process this
    process may signal; False when it has none left to stop.

    The two refusals mean different things, and neither leaves the lane anything to stop:

      * ESRCH (ProcessLookupError): the group has no process at all.
      * EPERM (PermissionError): the group still has processes, but none this process may signal.
        Every process the lane starts runs as the lane's own user, so a process that is still
        running a command the lane started is not refused by the kernel's ownership rule. What
        is refused is a process that has exited and waits to be reaped (macOS has been seen to
        refuse a group whose remaining members are such processes, where Linux delivers the
        signal to them and reports ESRCH only once they are reaped), or a process that a sandbox
        or another user keeps out of reach. A further signal could not end either.

    So for the lane's question, whether anything of the command is still running that a signal
    can end, both mean no, on Linux and on macOS. EPERM is reported in the log, since on a runner
    it can also mean that something the lane started is out of its control."""
    try:
        os.killpg(group, sig)
    except ProcessLookupError:
        return False
    except PermissionError:
        print("macOS remote shadow lane: the stopped command's process group has no process the "
              "lane may signal (only exited processes waiting to be reaped, or processes outside "
              "the lane's permissions); nothing of it is left to stop.", flush=True)
        return False
    return True


def _reap(process):
    """Waits for the command itself, at most STOP_KILL_SECONDS: a command no signal could reach
    must not hang the lane, which then reports and leaves it."""
    try:
        process.wait(timeout=STOP_KILL_SECONDS)
    except subprocess.TimeoutExpired:
        print("::warning::macOS remote shadow lane: the stopped command did not exit within %d "
              "seconds and is left running." % STOP_KILL_SECONDS, flush=True)


def _end_group(group):
    """Lets what is left of a process group whose leader exited finish (a Bazel client still
    completing its cancel), then terminates it, then kills it: _LEFTOVER_SECONDS for each."""
    if _group_gone(group, _LEFTOVER_SECONDS):
        return
    for sig in (signal.SIGTERM, signal.SIGKILL):
        if not _signal_group(group, sig) or _group_gone(group, _LEFTOVER_SECONDS):
            return


def _group_gone(group, seconds):
    """Waits up to `seconds` until no process of a process group is left that a signal can end
    (see _signal_group); returns whether that happened. The group leader must already be reaped
    or exiting."""
    bound = time.monotonic() + seconds
    while True:
        if not _signal_group(group, 0):
            return True
        if time.monotonic() >= bound:
            return False
        time.sleep(0.1)


def run_phase(command, environment, console, limit=STARVED_SECONDS, poll=POLL_SECONDS,
              clock=time.monotonic, grace=(60, 30, 30), deadline=None):
    """Runs one phase command; returns (status, stop), stop being None, STARVED or OUT_OF_TIME
    (the lane's budget, `deadline` on `clock`, ran out). The process runs in a session of its
    own, so a stop reaches the wrappers and the Bazel client together; the lane shuts the
    server down at its end."""
    print("+ " + shlex.join(command), flush=True)
    process = subprocess.Popen(command, cwd=ROOT, env=environment, start_new_session=True)
    return _wait_phase(process, console, limit, poll, clock, grace, deadline)


def _wait_phase(process, console, limit, poll, clock, grace, deadline):
    """Waits for a running phase command, polling the starvation guard and the deadline."""
    guard = StarvationGuard(clock(), limit)
    while True:
        wait = poll if deadline is None else max(0.0, min(poll, deadline - clock()))
        try:
            return process.wait(timeout=wait), None
        except subprocess.TimeoutExpired:
            pass
        stop = _stop_reason(guard, console, clock, deadline)
        if stop is not None:
            print("::warning::macOS remote shadow lane: %s; stopping the command (%s)." % (
                "no Bazel action completed for %ds while actions remained" % limit
                if stop == STARVED else "the lane's budget ran out", stop), flush=True)
            _stop(process, grace)
            return _STOP_STATUS[stop], stop


def _stop_reason(guard, console, clock, deadline):
    """STARVED, OUT_OF_TIME or None. Starvation is checked first: a starved executor is the more
    useful finding."""
    if guard.starved(progress(_tail(console)), clock()):
        return STARVED
    if deadline is not None and clock() >= deadline:
        return OUT_OF_TIME
    return None


def _run_captured(command, environment, console, deadline=None, grace=STOP_GRACE):
    """(exit status, console output, stopped) of a command; the output is also printed, as it
    would be. Past `deadline` (time.monotonic) it is stopped like a phase: OUT_OF_TIME_STATUS,
    and stopped is True."""
    print("+ " + shlex.join(command), flush=True)
    with open(console, "w", encoding="utf-8") as handle:
        process = subprocess.Popen(command, cwd=ROOT, env=environment, stdout=handle,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            status = process.wait(
                timeout=None if deadline is None else max(0.0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            print("::warning::macOS remote shadow lane: the lane's budget ran out; stopping the "
                  "command (%s)." % OUT_OF_TIME, flush=True)
            _stop(process, grace)
            status = OUT_OF_TIME_STATUS
        stopped = status == OUT_OF_TIME_STATUS and deadline is not None and \
            time.monotonic() >= deadline
    output = Path(console).read_text(encoding="utf-8", errors="replace")
    Path(console).unlink(missing_ok=True)
    print(output, end="", flush=True)
    return status, output, stopped


def _read_lines(path):
    try:
        # Lines end at a newline only: str.splitlines also splits at characters such as U+0085 and
        # U+2028, which a build event can carry unescaped inside a string, and would cut that
        # event in two.
        return Path(path).read_text(encoding="utf-8", errors="replace").split("\n")
    except OSError:
        return None


def _finish(out, summary, environment):
    Path(out).mkdir(parents=True, exist_ok=True)
    (Path(out) / "shadow-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    text = report(summary)
    (Path(out) / "report.md").write_text(text, encoding="utf-8")
    print(text)
    step_summary = environment.get("GITHUB_STEP_SUMMARY")
    if step_summary:
        with open(step_summary, "a", encoding="utf-8") as handle:
            handle.write(text)


def run(args, environment):
    """Runs the preflight and both phases; returns the first failing status, or 0."""
    targets = selection(args.fallback, args.affected, args.targets)
    if not targets:
        print("::error::Empty target selection reached the macOS remote shadow lane.")
        return 2
    args.deadline = time.monotonic() + args.budget_minutes * 60 if args.budget_minutes > 0 \
        else None
    summary = {"version": 1, "targets": len(targets), "fallback": targets == ["//..."],
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
        _finish(args.out, summary, environment)
        return status
    first_failure, stopped = 0, None
    budget = MAX_ARTIFACT_TOTAL_BYTES
    for phase in PHASES:
        if stopped is None and _past(args.deadline):
            stopped = OUT_OF_TIME
            summary["problem"] = "the lane's budget ran out before the %s phase (%s)." % (
                phase, OUT_OF_TIME)
            first_failure = first_failure or OUT_OF_TIME_STATUS
        if stopped is not None:
            # A starved executor or no time left: another phase would only wait again.
            summary["phases"][phase] = None
            continue
        entry, stopped, used = _run_one_phase(args, environment, targets, phase, budget)
        budget -= used
        summary["phases"][phase] = entry
        if stopped is not None:
            summary["problem"] = _stop_problem(args, phase, entry)
        if entry["status"] != 0:
            print("::warning::macOS remote shadow lane: the %s phase exited %d (advisory; the "
                  "pull request is not failed by it)." % (phase, entry["status"]))
            first_failure = first_failure or entry["status"]
    _finish(args.out, summary, environment)
    return first_failure


def _past(deadline):
    return deadline is not None and time.monotonic() >= deadline


def _run_one_phase(args, environment, targets, phase, budget):
    """Runs and summarizes one phase and stages its failed tests' outputs: (entry, stop, bytes
    staged)."""
    files = _phase_files(args.out, phase)
    files["bep"].parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    status, stop = run_phase(phase_command(phase, args.bazel, args.startup_option,
                                           args.rc_option, args.out, targets),
                             environment, files["console"], deadline=args.deadline)
    if stop is not None:
        # The server finishes the cancelled command and its build event file only now; no later
        # phase runs after a stop, so its analysis cache is not needed.
        _shutdown(args, environment)
    lines = _read_lines(files["bep"]) or []
    try:
        entry = summarize(lines)
    except Exception as error:  # A malformed event file must not cost the summary.
        print("::warning::macOS remote shadow lane: the %s phase's build event file could "
              "not be summarized (%s)." % (phase, type(error).__name__))
        entry = summarize([])
    if any(entry["failed_actions"].values()):
        print("::warning::macOS remote shadow lane: in the %s phase, %s." % (
            phase, failed_actions_line(entry["failed_actions"])))
    used = _stage_phase(args, lines, phase, budget, status, entry["targets"]["failed"])
    entry["status"] = status
    if entry["wall_time_seconds"] is None:
        entry["wall_time_seconds"] = round(time.monotonic() - started, 1)
    if stop is not None:
        entry["outcome"] = stop
        entry["progress"] = progress_counts(_tail(files["console"]))
        minutes = (time.monotonic() - started) / 60.0
        done = (entry["progress"] or {}).get("actions_done")
        entry["actions_per_minute"] = None if done is None or minutes <= 0 else \
            round(done / minutes, 1)
    return entry, stop, used


def _stop_problem(args, phase, entry):
    """The summary's problem line for a phase stopped as starved or out of time."""
    if entry["outcome"] == STARVED:
        return "the %s phase was stopped: %s (no action completed for %d minutes while " \
            "actions remained)." % (phase, STARVED, STARVED_SECONDS // 60)
    return "the %s phase was stopped: %s (the lane's %s-minute budget ran out)." % (
        phase, OUT_OF_TIME, _minutes(args.budget_minutes))


def _minutes(value):
    return ("%g" % value) if value else "-"


# Test results of a test that ran and did not pass. A test that was never built or never started
# (FAILED_TO_BUILD, NO_STATUS, TOOL_HALTED_BEFORE_TESTING) has no outputs to stage.
_RAN_AND_DID_NOT_PASS = frozenset({"FAILED", "TIMEOUT", "INCOMPLETE", "REMOTE_FAILURE"})


def _ran_and_did_not_pass(lines):
    """How many test results of a build event file are of tests that ran and did not pass."""
    return sum(1 for event in _decoded(lines)
               if _dict(event.get("id")).get("testResult") is not None and
               _dict(event.get("testResult")).get("status") in _RAN_AND_DID_NOT_PASS)


def _stage_phase(args, lines, phase, budget, status, build_failures=0):
    """Stages a phase's failed test outputs; returns the bytes staged, also when staging stopped
    part way, so the run cap holds across phases. Never raises: staging must not cost the
    summary."""
    staged = {"files": 0, "bytes": 0}
    if not args.output_user_root:
        if status != 0:
            print("::warning::macOS remote shadow lane: failed test outputs are staged only with "
                  "--output-user-root.")
    else:
        try:
            stage_failed_test_outputs(lines, args.out, phase, args.output_user_root, budget,
                                      staged)
        except Exception as error:  # Staging is diagnostic only; never lose the summary.
            print("::warning::macOS remote shadow lane: the %s phase's failed test outputs were "
                  "not all staged (%s)." % (phase, type(error).__name__))
    print("macOS remote shadow lane: staged %d output files (%d bytes) for %s."
          % (staged["files"], staged["bytes"], phase))
    failed = _ran_and_did_not_pass(lines)
    if failed and not staged["files"]:
        print("::warning::macOS remote shadow lane: the %s phase has %d test results of tests "
              "that ran and did not pass but staged no output files; their outputs were not "
              "downloaded or do not lie below the lane's output root." % (phase, failed))
    if build_failures:
        print("::warning::macOS remote shadow lane: the %s phase failed to build %d targets, so "
              "their tests did not run; the failing actions' output is above in this log." % (
                  phase, build_failures))
    return staged["bytes"]


def parse_args(argv, environ):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--out", default="",
                        help="the output directory; default $RUNNER_TEMP/" + OUT_NAME)
    parser.add_argument("--fallback", default=environ.get("FALLBACK", ""))
    parser.add_argument("--affected", default=environ.get("AFFECTED", ""))
    parser.add_argument("--bazel", default="bazelisk")
    parser.add_argument("--startup-option", action="append", default=[])
    parser.add_argument("--output-user-root", default=None,
                        help="the lane's own Bazel output root, passed to every Bazel command")
    parser.add_argument("--rc-option", action="append", default=[])
    parser.add_argument("--targets", nargs="*", default=None,
                        help="the target patterns, in place of --fallback and --affected")
    parser.add_argument("--budget-minutes", type=float, default=0.0,
                        help="the lane's wall-clock budget; 0 for none (see REPORT_MARGIN_MINUTES)")
    # The deadline the budget sets, on time.monotonic; run() computes it.
    parser.set_defaults(deadline=None)
    return parser.parse_args(argv)


def main(argv=None, environ=None):
    environ = dict(os.environ if environ is None else environ)
    args = parse_args(sys.argv[1:] if argv is None else argv, environ)
    if not _default_locations(args, environ):
        print("::error::macOS remote shadow lane: pass --out, or run where RUNNER_TEMP is set.")
        return 2
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
        targets = selection(args.fallback, args.affected, args.targets) or \
            ["<empty selection>"]
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
