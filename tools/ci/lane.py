"""Runs one phase of a CI lane from the lane table (tools/ci/lanes.py).

    python3 tools/ci/lane.py phase <lane> <phase> [--dry-run]
    python3 tools/ci/lane.py list
    bazel run //tools/ci:lane -- phase <lane> <phase> --dry-run

A main.yml step runs exactly one phase. The step passes what only the workflow knows through its
environment: the planner's selection (TARGETS_FALLBACK, TARGETS_AFFECTED), the remote cache flags
of the BuildBuddy action (BUILDBUDDY_FLAGS) and the runner's own variables. Nothing is
interpolated into a script.

Each action runs as the step script ran it under `set -euo pipefail`: the first failing command
ends the phase with its exit status (128 + N for a command killed by signal N), apart from the
deferred commands of an advisory phase, which run to the end and report the first failure.
`--dry-run` prints the phase's actions with every run-time value filled in, as JSON, and runs
nothing: a run-time variable that is unset prints as written (`$NAME` or `${NAME}`), an unset
selection as
@selected_targets, and a value only a command can produce as that command's `$(...)`.

Standard library only: this runs under each runner's system python3 (3.9 or later).
"""

import argparse
import glob
import json
import os
import re
import shlex
import subprocess
import sys

try:
    from tools.ci import lanes
except ModuleNotFoundError as error:
    # `python3 tools/ci/lane.py` puts this script's directory, not the checkout, on sys.path.
    if error.name not in ("tools", "tools.ci"):
        raise
    import lanes

SELECTION_SYMBOL = "@selected_targets"
REMOTE_GPU_MAPPER = ("python3", "tools/ci_remote_gpu_targets.py")
_VARIABLE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)")
# The shell's default field separators: an unquoted expansion splits on these.
_FIELDS = re.compile(r"[ \t\n]+")
_BLANK = re.compile(r"[ \t\n\r\f\v]*")


class PhaseFailed(Exception):
    def __init__(self, status, message=None):
        super().__init__(message)
        self.status = status
        self.message = message


def _fields(text):
    return [field for field in _FIELDS.split(text) if field]


class _Context:
    """Run-time values for one phase: the environment, the selection and any mapped targets."""

    def __init__(self, lane, environ, dry_run):
        self.lane = lane
        self.environ = environ
        self.dry_run = dry_run
        self.mapped = None

    def variable(self, name, spelling=None):
        if name in self.environ:
            return self.environ[name]
        if self.dry_run:
            return spelling or "$" + name
        raise PhaseFailed(1, "lane.py: %s: unbound variable" % name)

    def word(self, text):
        return _VARIABLE.sub(lambda match: self.variable(match.group(1) or match.group(2),
                                                         match.group(0)), text)

    def selection(self):
        """The selection as the step scripts derived it: the raw text and its words."""
        fallback = self.environ.get("TARGETS_FALLBACK")
        affected = self.environ.get("TARGETS_AFFECTED")
        if fallback is None and affected is None:
            if self.dry_run:
                return SELECTION_SYMBOL, [SELECTION_SYMBOL]
            raise PhaseFailed(1, "lane.py: TARGETS_FALLBACK and TARGETS_AFFECTED are unset")
        raw = "//..." if fallback == "true" else (affected or "").rstrip("\n")
        return raw, _fields(raw)

    def remote_gpu_selection(self):
        if self.mapped is not None:
            return self.mapped
        return ["$(%s)" % " ".join(REMOTE_GPU_MAPPER + tuple(self.selection()[1]))]

    def words(self, item):
        if isinstance(item, lanes.Selected):
            if item == lanes.REMOTE_GPU_SELECTED:
                return self.remote_gpu_selection()
            selection = self.selection()[1]
            if (item == lanes.HOSTED_BUILD_SELECTED and "//..." not in selection
                    and _fields(self.environ.get("BUILDBUDDY_FLAGS", ""))):
                return ["//...", *selection]
            return selection
        if isinstance(item, lanes.Split):
            return _fields(self.environ.get(item.name, ""))
        return [self.word(item)]

    def argv(self, items):
        return [word for item in items for word in self.words(item)]

    def bazel(self, action):
        return (self.argv(action.wrappers) + ["bazelisk"] + list(self.lane.startup)
                + self.argv(action.args))

    def query(self, tag):
        expression = "attr('tags', '%s', tests(set(%s)))" % (tag, self.selection()[0])
        return ["bazelisk"] + list(self.lane.startup) + ["query", expression]


# --- Running commands. ---------------------------------------------------------------------------


def _status(returncode):
    return 128 - returncode if returncode < 0 else returncode


def _run(argv, **kwargs):
    """Runs a command as the step's shell did and returns its exit status. It is echoed first, in
    a form a shell reads back as the same words; no secret reaches a phase's arguments."""
    print("+ " + shlex.join(argv), flush=True)
    try:
        process = subprocess.Popen(argv, **kwargs)
    except OSError as error:
        print("lane.py: %s: %s" % (argv[0], error.strerror), file=sys.stderr, flush=True)
        return 127, None
    while True:
        try:
            output, _ = process.communicate()
            return _status(process.returncode), output
        except KeyboardInterrupt:
            # As the shell does: an interrupt reaches the whole process group, and the step ends
            # when the command it is running does.
            continue


def _check(argv, **kwargs):
    status, output = _run(argv, **kwargs)
    if status:
        raise PhaseFailed(status)
    return output


def _ignoring_failure(argv, quiet=False, **kwargs):
    """`command || true`, or with `quiet` `command 2>/dev/null || true`: the output of a command
    whose failure is ignored."""
    try:
        return subprocess.run(argv, stderr=subprocess.DEVNULL if quiet else None, check=False,
                              universal_newlines=True, **kwargs).stdout
    except OSError as error:
        if not quiet:
            print("lane.py: %s: %s" % (argv[0], error.strerror), file=sys.stderr, flush=True)
        return None


def _reset_repository(context, repository):
    """Removes a partially fetched repository from the output base before a retry."""
    # As the step script did: only `info`, `du` and `find` ran with standard error discarded.
    output_base = (_ignoring_failure(["bazelisk", "info", "output_base"], quiet=True,
                                     stdout=subprocess.PIPE) or "").rstrip("\n")
    _ignoring_failure(["bazelisk", "shutdown"])
    pattern = "*%s*" % repository
    external = os.path.join(output_base, "external")
    print("::group::Bazel disk state before LLVM fetch retry", flush=True)
    _ignoring_failure(["df", "-h", "/var/cache/bazel", context.variable("GITHUB_WORKSPACE")])
    if output_base and os.path.isdir(external):
        _ignoring_failure(["du", "-sh"] + (sorted(glob.glob(os.path.join(external, pattern)))
                                           or [os.path.join(external, pattern)]), quiet=True)
    print("::endgroup::", flush=True)
    if output_base and os.path.isdir(external):
        _ignoring_failure(["find", external, "-maxdepth", "1", "-name", pattern, "-print",
                           "-exec", "rm", "-rf", "{}", "+"], quiet=True)


class _Done(Exception):
    """Ends a phase successfully before its remaining actions."""


def _settle(action, status, deferred):
    if status and getattr(action, "defer", False):
        deferred.append(status)
    elif status:
        raise PhaseFailed(status)


def _run_bazel(context, action, deferred):
    _settle(action, _run(context.bazel(action))[0], deferred)


def _run_command(context, action, deferred):
    _settle(action, _run(context.argv(action.argv))[0], deferred)


def _run_notice(context, action, deferred):
    print("::notice::" + action.text, flush=True)


def _selection_error(action):
    return "::error::Empty target selection reached %s." % action.where


def _run_require_selection(context, action, deferred):
    if _BLANK.fullmatch(context.selection()[0]):
        raise PhaseFailed(1, _selection_error(action))


def _run_map_remote_gpu(context, action, deferred):
    status, output = _run(list(REMOTE_GPU_MAPPER) + context.selection()[1],
                          stdout=subprocess.PIPE, universal_newlines=True)
    if status:
        raise PhaseFailed(1, "::error::Failed to select remote GPU test targets.")
    if _BLANK.fullmatch(output.rstrip("\n")):
        raise PhaseFailed(1, "::error::Remote GPU target selection became empty.")
    context.mapped = _fields(output)


_NO_BROWSER_TARGETS = "No browser targets selected."


def _run_query_tag(context, action, deferred):
    output = _check(context.query(action.tag), stdout=subprocess.PIPE, universal_newlines=True)
    if not output.rstrip("\n"):
        print(_NO_BROWSER_TARGETS, flush=True)
        raise _Done()


def _run_retry_after_reset(context, action, deferred):
    status, _ = _run(context.bazel(action.fetch))
    if status:
        _reset_repository(context, action.repository)
        status, _ = _run(context.bazel(action.fetch))
    _settle(action, status, deferred)


_RECEIPT = "$RUNNER_TEMP/coverage-receipt/coverage-receipt.json"


def _run_require_executed(context, action, deferred):
    with open(context.word(_RECEIPT), encoding="utf-8") as receipt:
        step = json.load(receipt)["steps"][action.step]
    if step.get("executed", 0) != action.count or step.get("tests", 0) != action.count:
        raise PhaseFailed(1, action.message)


def _described(entry, action):
    if action.defer:
        entry["defer"] = True
    return entry


# How each kind of action runs, and how a dry run describes it.
_ACTIONS = {
    lanes.Bazel: (_run_bazel, lambda context, action: _described(
        {"action": "bazel", "argv": context.bazel(action)}, action)),
    lanes.Command: (_run_command, lambda context, action: _described(
        {"action": "run", "argv": context.argv(action.argv)}, action)),
    lanes.Notice: (_run_notice, lambda context, action: {
        "action": "notice", "text": "::notice::" + action.text}),
    lanes.RequireSelection: (_run_require_selection, lambda context, action: {
        "action": "require_selection", "error": _selection_error(action)}),
    lanes.MapRemoteGpu: (_run_map_remote_gpu, lambda context, action: {
        "action": "map_remote_gpu", "argv": list(REMOTE_GPU_MAPPER) + context.selection()[1]}),
    lanes.QueryTag: (_run_query_tag, lambda context, action: {
        "action": "query", "argv": context.query(action.tag), "when_empty": _NO_BROWSER_TARGETS}),
    lanes.RetryAfterReset: (_run_retry_after_reset, lambda context, action: {
        "action": "bazel", "argv": context.bazel(action.fetch),
        "retry_after_reset": action.repository}),
    lanes.RequireExecuted: (_run_require_executed, lambda context, action: {
        "action": "require_executed", "step": action.step, "count": action.count,
        "receipt": context.word(_RECEIPT)}),
}


def _report(failure):
    """Prints a failure's message where the step scripts did: workflow commands on standard
    output, where Actions reads them; other errors on standard error."""
    if failure.message:
        stream = sys.stdout if failure.message.startswith("::") else sys.stderr
        print(failure.message, file=stream, flush=True)


def run_phase(lane_name, phase_name, environ=None):
    """Runs a phase and returns its exit status."""
    lane = lanes.LANES[lane_name]
    context = _Context(lane, os.environ if environ is None else environ, dry_run=False)
    deferred = []
    try:
        for action in lane.phases[phase_name].actions:
            _ACTIONS[type(action)][0](context, action, deferred)
    except _Done:
        pass
    except PhaseFailed as failure:
        _report(failure)
        return failure.status
    return deferred[0] if deferred else 0


# --- Dry run. ------------------------------------------------------------------------------------


def dry_run(lane_name, phase_name, environ=None):
    """What a phase runs, as data: its step name, environment additions and actions."""
    lane = lanes.LANES[lane_name]
    phase = lane.phases[phase_name]
    context = _Context(lane, os.environ if environ is None else environ, dry_run=True)
    return {"lane": lane_name, "phase": phase_name, "step": phase.step, "env": {},
            "actions": [_ACTIONS[type(action)][1](context, action) for action in phase.actions]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    commands = parser.add_subparsers(dest="command", required=True)
    phase = commands.add_parser("phase", help="run one phase of a lane")
    phase.add_argument("lane", choices=sorted(lanes.LANES))
    phase.add_argument("phase")
    phase.add_argument("--dry-run", action="store_true",
                       help="print the phase's commands as JSON and run nothing")
    commands.add_parser("list", help="print every lane and phase")
    arguments = parser.parse_args(argv)
    if arguments.command == "list":
        for name, lane in lanes.LANES.items():
            for phase_name, entry in lane.phases.items():
                print("%s %s  (%s)" % (name, phase_name, entry.step))
        return 0
    if arguments.phase not in lanes.LANES[arguments.lane].phases:
        parser.error("lane %s has no phase %r; phases: %s" % (
            arguments.lane, arguments.phase,
            ", ".join(lanes.LANES[arguments.lane].phases)))
    # Under `bazel run`, commands resolve their paths against the checkout.
    if os.environ.get("BUILD_WORKSPACE_DIRECTORY"):
        os.chdir(os.environ["BUILD_WORKSPACE_DIRECTORY"])
    if arguments.dry_run:
        print(json.dumps(dry_run(arguments.lane, arguments.phase), indent=2))
        return 0
    return run_phase(arguments.lane, arguments.phase)


if __name__ == "__main__":
    sys.exit(main())
