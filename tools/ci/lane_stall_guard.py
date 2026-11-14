#!/usr/bin/env python3
"""Runs one browser CI lane under a wall-clock stall guard.

    lane_stall_guard.py --deadline-seconds N --diagnostics-dir DIR -- COMMAND [ARG...]

The command runs in a session of its own, with an environment variable naming this run. When it
exits, this exits with its status (128 + N for a lane ended by signal N). When it is still running
after N seconds, this records the process table, the lane's processes and their open files, and
the open files of browser crash handlers that started during the lane, under DIR. It then stops
the lane: its process tree and every process that still carries the run's environment variable,
including processes that left the tree by starting a session of their own and being reparented.
The stall path always exits 124, even when recording fails. SIGUSR1 starts the stall path at once,
or as soon as the lane has been started.

The guard turns a hang into a named failure. A Playwright run can wait without bound in browser
teardown: the browser child counts as closed only once its process exits and every inherited
stdio descriptor is closed, and the worker's graceful shutdown has no deadline while it keeps
sending heartbeats. The recorded processes and descriptors show which process holds the run
open. The deadline never binds on a passing lane; it is not a timing budget.

SIGINT and SIGTERM are forwarded to the lane's process group. A second one while the lane is
still running starts the stall path, so a lane that ignores the first cannot hold the caller until
the deadline. Once the lane has exited, the guard ends itself with the signal it received, so the
caller sees an interrupted run rather than a lane result.

Only processes that started after the guard did are ever signalled individually. A process that
replaces its environment drops the tag and is stopped only while it is in the lane's process
tree; a guard nested in another guard's lane stops only its own lane, and the outer guard reaches
the inner lane through the outer tag that the inner lane inherits.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import uuid

STALL_EXIT_CODE = 124
# Names the guard's run in the lane's environment. Every process the lane starts inherits it,
# whatever its parent or session becomes.
TAG_VARIABLE = "DONNER_LANE_STALL_GUARD_TAG"
# Each diagnostic command is bounded, and one that ignores SIGKILL is abandoned, not awaited.
TOOL_TIMEOUT_SECONDS = 20
TOOL_REAP_SECONDS = 5
# Time a stopped lane gets to exit after SIGTERM before every process left gets SIGKILL.
TERMINATION_GRACE_SECONDS = 10
# Process rows printed to the log; the full table is written to the diagnostics directory.
MAX_LOGGED_ROWS = 80
# Executable names whose open descriptors are recorded even when they no longer carry the run's
# environment: a crash handler outlives the browser it watched and is reparented away from it.
DETACHED_HELPER_NAMES = ("crashpad",)

PS_COLUMNS = "pid=,ppid=,pgid=,stat=,etime=,rss=,lstart=,comm="
# `lstart` prints five words in the C locale, for example `Sat Nov 14 07:31:57 2026`.
_LSTART_WORDS = 5
_LSTART_FORMAT = "%a %b %d %H:%M:%S %Y"


class StallRequested(Exception):
    """A stall request arrived while the guard waited for the lane: run the stall path now."""


def parse_start_time(lstart: str) -> int | None:
    """Seconds since the epoch of a `ps` local start time, or None when it cannot be read."""
    try:
        return int(time.mktime(time.strptime(lstart, _LSTART_FORMAT)))
    except (ValueError, OverflowError):
        return None


def exit_status(returncode: int) -> int:
    """The shell's status for a child's return code: 128 + N for a child ended by signal N."""
    return 128 - returncode if returncode < 0 else returncode


def parse_elapsed_seconds(elapsed: str) -> int | None:
    """Seconds in a `ps` elapsed time of the form `[[dd-]hh:]mm:ss`, or None when malformed."""
    days, _, clock = elapsed.rpartition("-")
    parts = clock.split(":")
    if not 2 <= len(parts) <= 3 or not all(part.isdigit() for part in parts):
        return None
    if days and not days.isdigit():
        return None
    hours, minutes, seconds = ([0] * (3 - len(parts))) + [int(part) for part in parts]
    return ((int(days or 0) * 24 + hours) * 60 + minutes) * 60 + seconds


def parse_process_table(text: str) -> list[dict]:
    """Rows of `ps -A -ww -o` PS_COLUMNS output."""
    rows = []
    for line in text.splitlines():
        fields = line.split(None, 6 + _LSTART_WORDS)
        if len(fields) < 7 + _LSTART_WORDS or not all(field.isdigit() for field in fields[:3]):
            continue
        rows.append({
            "pid": int(fields[0]),
            "ppid": int(fields[1]),
            "pgid": int(fields[2]),
            "elapsed": fields[4],
            "started": parse_start_time(" ".join(fields[6:6 + _LSTART_WORDS])),
            "command": fields[6 + _LSTART_WORDS],
            "line": line.strip(),
        })
    return rows


def lane_tree(rows: list[dict], root_pid: int) -> list[dict]:
    """The root and every descendant reachable through parent ids, in table order."""
    members = {root_pid}
    grew = True
    while grew:
        grew = False
        for row in rows:
            if row["pid"] not in members and row["ppid"] in members:
                members.add(row["pid"])
                grew = True
    return [row for row in rows if row["pid"] in members]


def detached_helpers(rows: list[dict], lane_seconds: float) -> list[dict]:
    """Crash handlers no older than the lane, whichever parent they have now."""
    helpers = []
    for row in rows:
        name = os.path.basename(row["command"])
        elapsed = parse_elapsed_seconds(row["elapsed"])
        if any(part in name for part in DETACHED_HELPER_NAMES) and elapsed is not None \
                and elapsed <= lane_seconds + 1:
            helpers.append(row)
    return helpers


def _wait_bounded(process: subprocess.Popen, name: str) -> str:
    """Waits for a diagnostic command; returns a note when it had to be killed or abandoned."""
    try:
        process.wait(timeout=TOOL_TIMEOUT_SECONDS)
        return f"{name} exit status {process.returncode}\n" if process.returncode else ""
    except subprocess.TimeoutExpired:
        process.kill()
    try:
        process.wait(timeout=TOOL_REAP_SECONDS)
        return f"{name}: no answer within {TOOL_TIMEOUT_SECONDS} s; output partial\n"
    except subprocess.TimeoutExpired:
        return f"{name}: no answer within {TOOL_TIMEOUT_SECONDS} s; abandoned, output partial\n"


def run_tool(command: list[str]) -> str:
    """A diagnostic command's output, partial when it timed out, plus any note about how it ended.

    Output goes to a file rather than a pipe, so whatever it wrote is kept even when the command
    has to be abandoned.
    """
    with tempfile.TemporaryFile() as output:
        try:
            process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                       env=dict(os.environ, LC_ALL="C"))
        except FileNotFoundError:
            return f"{command[0]}: not available\n"
        note = _wait_bounded(process, command[0])
        output.seek(0)
        return output.read().decode(errors="replace") + note


def _environment_of(entry: Path) -> list[bytes]:
    """A process's environment from /proc, or nothing when it cannot be read."""
    try:
        return (entry / "environ").read_bytes().split(b"\0")
    except OSError:
        return []


def _proc_tagged_pids(marker: str) -> set[int]:
    """Tagged processes from /proc, where the kernel exposes each environment."""
    return {int(entry.name) for entry in Path("/proc").iterdir()
            if entry.name.isdigit() and marker.encode() in _environment_of(entry)}


def _ps_tagged_pids(marker: str) -> set[int]:
    """Tagged processes from `ps -E`, which appends each environment to the command."""
    rows = (line.split() for line in
            run_tool(["ps", "-A", "-ww", "-E", "-o", "pid=,command="]).splitlines())
    return {int(fields[0]) for fields in rows
            if fields and fields[0].isdigit() and marker in fields[1:]}


def tagged_pids(tag: str) -> set[int]:
    """Processes whose environment carries this run's tag, excluding the guard itself."""
    marker = f"{TAG_VARIABLE}={tag}"
    if Path("/proc/self/environ").exists():
        pids = _proc_tagged_pids(marker)
    else:
        pids = _ps_tagged_pids(marker)
    pids.discard(os.getpid())
    return pids


def lane_members(root_pid: int, tag: str) -> tuple[str, list[dict], list[dict]]:
    """A fresh process table, its rows, and the lane's members in it.

    Members are the root's process tree plus every other process carrying the run's tag. The
    tag is read just before the table, so a pid is used only within moments of being observed.
    """
    tagged = tagged_pids(tag)
    table = run_tool(["ps", "-A", "-ww", "-o", PS_COLUMNS])
    rows = parse_process_table(table)
    tree = lane_tree(rows, root_pid)
    in_tree = {row["pid"] for row in tree}
    members = tree + [row for row in rows if row["pid"] in tagged and row["pid"] not in in_tree]
    return table, rows, members


def record_diagnostics(directory: Path, root_pid: int, tag: str, lane_seconds: float) -> None:
    """Writes the process table, the lane's processes and their open files; prints the lane."""
    directory.mkdir(parents=True, exist_ok=True)
    table, rows, members = lane_members(root_pid, tag)
    (directory / "process-table.txt").write_text(table, encoding="utf-8")
    known = {row["pid"] for row in members}
    watched = members + [row for row in detached_helpers(rows, lane_seconds)
                         if row["pid"] not in known]
    summary = "".join(f"{row['line']}\n" for row in watched)
    (directory / "lane-processes.txt").write_text(summary, encoding="utf-8")
    pids = ",".join(str(row["pid"]) for row in watched)
    open_files = run_tool(["lsof", "-b", "-w", "-nP", "-p", pids]) if pids \
        else "no lane processes\n"
    (directory / "open-files.txt").write_text(open_files, encoding="utf-8")
    print("lane stall guard: lane processes (pid ppid pgid stat elapsed rss started command):",
          flush=True)
    for row in watched[:MAX_LOGGED_ROWS]:
        print(f"  {row['line']}", flush=True)
    if len(watched) > MAX_LOGGED_ROWS:
        print(f"  ... {len(watched) - MAX_LOGGED_ROWS} more in lane-processes.txt", flush=True)


def signal_targets(members: list[dict], root_pid: int, not_before: int) -> list[int]:
    """The members to signal one by one: not the root, and only those that started at or after
    `not_before`, so a pid reused by an older process is never signalled. A member whose start
    time cannot be read is not signalled."""
    return [row["pid"] for row in members if row["pid"] != root_pid
            and row["started"] is not None and row["started"] >= not_before]


def signal_lane(root_pid: int, tag: str, signum: int, not_before: int) -> None:
    """Sends `signum` to the lane's group and to each member of a fresh snapshot.

    The root is the guard's unreaped child, so neither its pid nor its group can be reused yet.
    Other pids come from a snapshot taken here, never from an earlier one.
    """
    try:
        os.killpg(root_pid, signum)
    except (ProcessLookupError, PermissionError):
        pass
    _, _, members = lane_members(root_pid, tag)
    for pid in signal_targets(members, root_pid, not_before):
        try:
            os.kill(pid, signum)
        except (ProcessLookupError, PermissionError):
            pass


def stop_lane(child: subprocess.Popen, tag: str, not_before: int) -> None:
    """SIGTERM to the lane, then SIGKILL to anything left after the grace, from a new snapshot."""
    signal_lane(child.pid, tag, signal.SIGTERM, not_before)
    try:
        child.wait(timeout=TERMINATION_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        pass
    signal_lane(child.pid, tag, signal.SIGKILL, not_before)
    try:
        child.wait(timeout=TERMINATION_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        print("lane stall guard: the lane did not exit after SIGKILL", flush=True)


def run_stall_path(arguments: argparse.Namespace, child: subprocess.Popen, tag: str,
                   started: float, not_before: int) -> int:
    print(f"::error::lane stall guard: the lane is still running after "
          f"{time.monotonic() - started:.0f} s (deadline {arguments.deadline_seconds:g} s); "
          f"recording its processes in {arguments.diagnostics_dir} and stopping it", flush=True)
    try:
        record_diagnostics(arguments.diagnostics_dir, child.pid, tag, time.monotonic() - started)
    except Exception as error:  # Recording is best effort; stopping the lane is not.
        print(f"lane stall guard: recording diagnostics failed: {type(error).__name__}: {error}",
              flush=True)
    finally:
        stop_lane(child, tag, not_before)
    return STALL_EXIT_CODE


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--deadline-seconds", type=float, required=True)
    parser.add_argument("--diagnostics-dir", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args(argv)
    if arguments.command[:1] == ["--"]:
        arguments.command = arguments.command[1:]
    if not arguments.command:
        parser.error("a command is required after --")
    if arguments.deadline_seconds <= 0:
        parser.error("--deadline-seconds must be positive")
    return arguments


class Guard:
    """The guard's signal state.

    A stall request only raises StallRequested while `waiting` is set, which covers exactly the
    guard's wait for the lane; the request clears it before raising, so a request can never
    escape outside that wait or interrupt the stall path. Elsewhere a request is only recorded.
    """

    def __init__(self):
        self.child: subprocess.Popen | None = None
        self.signal: int | None = None
        self.signals = 0
        self.stall = False
        self.waiting = False

    def request_stall(self) -> None:
        self.stall = True
        if self.waiting:
            self.waiting = False
            raise StallRequested()

    def on_stall_signal(self, _signum, _frame) -> None:
        self.request_stall()

    def deliver(self, signum: int) -> None:
        """Sends `signum` to a lane that has not been reaped yet."""
        if self.child is None or self.child.returncode is not None:
            return
        try:
            os.killpg(self.child.pid, signum)
        except (ProcessLookupError, PermissionError):
            return
        os.write(sys.stdout.fileno(),
                 f"lane stall guard: forwarded {signal.Signals(signum).name} to the lane\n"
                 .encode())

    def forward(self, signum, _frame) -> None:
        self.signal = signum
        self.signals += 1
        self.deliver(signum)
        if self.signals > 1:
            self.request_stall()

    def run(self, arguments: argparse.Namespace) -> int:
        tag = uuid.uuid4().hex
        started = time.monotonic()
        not_before = int(time.time())
        child = None
        try:
            # Descriptors the caller passed down reach the lane unchanged, as without the guard.
            child = subprocess.Popen(arguments.command, start_new_session=True, close_fds=False,
                                     env=dict(os.environ, **{TAG_VARIABLE: tag}))
            self.child = child
            if self.signal is not None:
                self.deliver(self.signal)
            self.waiting = True
            if self.stall:
                self.waiting = False
                raise StallRequested()
            returncode = child.wait(timeout=arguments.deadline_seconds)
            self.waiting = False
            return exit_status(returncode)
        except (subprocess.TimeoutExpired, StallRequested):
            self.waiting = False
            if child.poll() is not None:  # The lane ended as the request arrived.
                return exit_status(child.returncode)
            return run_stall_path(arguments, child, tag, started, not_before)


def main(argv: list[str]) -> int:
    arguments = parse_arguments(argv)
    guard = Guard()
    handlers = {signal.SIGINT: guard.forward, signal.SIGTERM: guard.forward,
                signal.SIGUSR1: guard.on_stall_signal}
    previous = {signum: signal.signal(signum, handler) for signum, handler in handlers.items()}
    try:
        status = guard.run(arguments)
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)
    if guard.signal is not None:
        signal.signal(guard.signal, signal.SIG_DFL)
        os.kill(os.getpid(), guard.signal)
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
