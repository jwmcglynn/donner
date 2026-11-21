"""A workflow's text with each lane step's commands, for tests that read commands from steps.

A main.yml step that runs `python3 tools/ci/lane.py phase <lane> <phase>` keeps its commands in
the lane table. `with_dry_runs` replaces that `run:` line with a block whose first line is the
call, as a comment, followed by the phase's dry run: each command on one line, its words joined
by spaces, and every other action as a comment. Everything else in the text is unchanged, so a
test that reads steps, conditions and commands from workflow text reads what each phase runs.
The edit helpers change the lane table for a test's negative controls.
"""

import itertools
import re
from unittest import mock

from tools import bazelrc_configs
from tools.ci import lane
from tools.ci import lanes

_LANE_STEP = re.compile(r"^(?P<indent> +)run: (?P<call>python3 tools/ci/lane\.py phase "
                        r"(?P<lane>[\w-]+) (?P<phase>[\w-]+))$", re.MULTILINE)


def phase_lines(lane_name, phase_name, environ=None):
    """A phase's dry run as lines: commands as their words, other actions as comments."""
    lines = []
    for action in lane.dry_run(lane_name, phase_name, environ or {})["actions"]:
        if action["action"] in ("bazel", "query", "run"):
            lines.append(" ".join(action["argv"]))
        else:
            detail = " ".join(action.get("argv") or ()) or action.get("error") or \
                action.get("text") or ""
            lines.append("# %s: %s" % (action["action"], detail))
    return lines


def split_placeholders():
    """Each run-time flag variable a phase splits into words, standing for itself."""
    return {item.name: "$" + item.name for entry in lanes.LANES.values()
            for phase in entry.phases.values() for action in phase.actions
            for item in getattr(action, "args", ())
            if isinstance(item, lanes.Split)}


def with_dry_runs(workflow, environ=None):
    """`workflow` with each lane step's `run:` line replaced by its phase's dry run.

    By default each run-time flag variable prints as `$NAME` where the phase splits it, as the
    step scripts wrote it; the selection prints as @selected_targets.
    """
    environ = split_placeholders() if environ is None else environ
    def expand(match):
        indent = match.group("indent") + "  "
        lines = ["# " + match.group("call")] + phase_lines(
            match.group("lane"), match.group("phase"), environ)
        return match.group("indent") + "run: |" + "".join("\n" + indent + line
                                                            for line in lines)
    return _LANE_STEP.sub(expand, workflow)


def expanded(argv, configs):
    """A command's argv with each repository config (bazelrc_configs.parse) replaced by its
    flags, as Bazel applies them; wrappers, startup options and a query stay as written."""
    start = argv.index("bazelisk") + 1
    startup = list(itertools.takewhile(lambda word: word.startswith("--"), argv[start:]))
    command = argv[start + len(startup)]
    options = argv[start + len(startup) + 1:]
    if command != "query":
        options = bazelrc_configs.expand(command, options, configs)
    return argv[:start] + startup + [command] + options


def phase_commands(lane_name, phase_name, configs=None, environ=None):
    """The argv of each command a phase runs, from its dry run; with `configs`, Bazel commands
    have their repository configs expanded."""
    commands = []
    for action in lane.dry_run(lane_name, phase_name, environ or {})["actions"]:
        if "argv" not in action:
            continue
        argv = action["argv"]
        if configs is not None and action["action"] == "bazel":
            argv = expanded(argv, configs)
        commands.append(argv)
    return commands


def replacing(old, *new):
    """An edit of a command's arguments: each `old` argument becomes the arguments `new`."""
    def edit(arguments):
        if old not in arguments:
            raise AssertionError("%r is not an argument of %r" % (old, arguments))
        return [word for argument in arguments
                for word in (new if argument == old else (argument,))]
    return edit


def edited_lane(lane_name, edit, phases=None):
    """The lane table with the Bazel commands of a lane's `phases` (default: every phase that
    passes the edited argument) edited, as a context manager, for negative controls."""
    entry = lanes.LANES[lane_name]
    edited = dict(entry.phases)
    for name, phase in entry.phases.items():
        if phases is None or name in phases:
            edited[name] = phase._replace(actions=tuple(
                _edited(action, edit, strict=phases is not None) for action in phase.actions))
    return mock.patch.dict(lanes.LANES, {lane_name: entry._replace(phases=edited)})


def _edited(action, edit, strict):
    """A Bazel action with its arguments edited; an action the edit does not apply to unchanged,
    unless `strict`."""
    if not isinstance(action, lanes.Bazel):
        return action
    try:
        return action._replace(args=tuple(edit(list(action.args))))
    except AssertionError:
        if strict:
            raise
        return action
