#!/usr/bin/env python3
"""Runs one browser CI lane under a wall-clock stall guard.

    lane_stall_guard.py --deadline-seconds N --diagnostics-dir DIR -- COMMAND [ARG...]

The command runs in a session of its own. When it exits, this exits with its status. When it is
still running after N seconds, this records the process table, the lane's process tree and the
open files of those processes and of browser crash handlers that started during the lane under
DIR, stops every process of the tree, and exits 124.

The guard turns a hang into a named failure. A Playwright run can wait without bound in browser
teardown: the browser child counts as closed only once its process exits and every inherited
stdio descriptor is closed, and the worker's graceful shutdown has no deadline while it keeps
sending heartbeats. The recorded tree and descriptors show which process holds the run open.
The deadline never binds on a passing lane; it is not a timing budget.

SIGINT and SIGTERM are forwarded to the lane's process group, so a cancelled job still stops it.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

STALL_EXIT_CODE = 124
# Each diagnostic command is bounded so a wedged host cannot hold the guard open.
TOOL_TIMEOUT_SECONDS = 20
# Time a stopped lane gets to exit after SIGTERM before every process left gets SIGKILL.
TERMINATION_GRACE_SECONDS = 10
# Process rows printed to the log; the full table is written to the diagnostics directory.
MAX_LOGGED_ROWS = 80
# Executable names whose open descriptors are recorded even after they leave the lane's tree:
# a crash handler outlives the browser it watched and is reparented away from the lane.
DETACHED_HELPER_NAMES = ("crashpad",)

PS_COLUMNS = "pid=,ppid=,pgid=,stat=,etime=,rss=,comm="


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
    """Rows of `ps -A -ww -o pid=,ppid=,pgid=,stat=,etime=,rss=,comm=` output."""
    rows = []
    for line in text.splitlines():
        fields = line.split(None, 6)
        if len(fields) < 7 or not all(field.isdigit() for field in fields[:3]):
            continue
        rows.append({
            "pid": int(fields[0]),
            "ppid": int(fields[1]),
            "pgid": int(fields[2]),
            "stat": fields[3],
            "elapsed": fields[4],
            "line": line.strip(),
            "command": fields[6],
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


def run_tool(command: list[str]) -> str:
    """A diagnostic command's output, or a fixed line saying why there is none."""
    try:
        result = subprocess.run(command, capture_output=True, text=True, errors="replace",
                                timeout=TOOL_TIMEOUT_SECONDS, check=False)
    except FileNotFoundError:
        return f"{command[0]}: not available\n"
    except subprocess.TimeoutExpired:
        return f"{command[0]}: no answer within {TOOL_TIMEOUT_SECONDS} s\n"
    return result.stdout + (f"{command[0]} exit status {result.returncode}\n"
                            if result.returncode else "")


def record_diagnostics(directory: Path, root_pid: int, lane_seconds: float) -> list[dict]:
    """Writes the diagnostics and prints the lane's rows; returns the lane's tree."""
    directory.mkdir(parents=True, exist_ok=True)
    table = run_tool(["ps", "-A", "-ww", "-o", PS_COLUMNS])
    (directory / "process-table.txt").write_text(table, encoding="utf-8")
    rows = parse_process_table(table)
    tree = lane_tree(rows, root_pid)
    helpers = [row for row in detached_helpers(rows, lane_seconds) if row not in tree]
    watched = tree + helpers
    summary = "".join(f"{row['line']}\n" for row in watched)
    (directory / "lane-processes.txt").write_text(summary, encoding="utf-8")
    pids = ",".join(str(row["pid"]) for row in watched)
    open_files = run_tool(["lsof", "-nP", "-p", pids]) if pids else "no lane processes\n"
    (directory / "open-files.txt").write_text(open_files, encoding="utf-8")
    print("lane stall guard: lane processes (pid ppid pgid stat elapsed rss command):", flush=True)
    for row in watched[:MAX_LOGGED_ROWS]:
        print(f"  {row['line']}", flush=True)
    if len(watched) > MAX_LOGGED_ROWS:
        print(f"  ... {len(watched) - MAX_LOGGED_ROWS} more in lane-processes.txt", flush=True)
    return tree


def signal_all(tree: list[dict], root_pid: int, signum: int) -> None:
    """Sends `signum` to the lane's group and to every process and group of its tree."""
    groups = {root_pid} | {row["pgid"] for row in tree if row["pgid"] > 1}
    for group in groups - {os.getpgrp()}:
        try:
            os.killpg(group, signum)
        except (ProcessLookupError, PermissionError):
            pass
    for row in tree:
        try:
            os.kill(row["pid"], signum)
        except (ProcessLookupError, PermissionError):
            pass


def stop_lane(child: subprocess.Popen, tree: list[dict]) -> None:
    """SIGTERM to the whole tree, then SIGKILL to anything still running after the grace."""
    signal_all(tree, child.pid, signal.SIGTERM)
    try:
        child.wait(timeout=TERMINATION_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        pass
    signal_all(tree, child.pid, signal.SIGKILL)
    child.wait()


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


def main(argv: list[str]) -> int:
    arguments = parse_arguments(argv)
    started = time.monotonic()
    # Descriptors the caller passed down reach the lane unchanged, as without the guard.
    child = subprocess.Popen(arguments.command, start_new_session=True, close_fds=False)

    def forward(signum, _frame):
        try:
            os.killpg(child.pid, signum)
        except ProcessLookupError:
            pass

    signal.signal(signal.SIGINT, forward)
    signal.signal(signal.SIGTERM, forward)
    try:
        return child.wait(timeout=arguments.deadline_seconds)
    except subprocess.TimeoutExpired:
        pass
    print(f"::error::lane stall guard: the lane is still running after "
          f"{arguments.deadline_seconds:g} s; recording its processes in "
          f"{arguments.diagnostics_dir} and stopping it", flush=True)
    tree = record_diagnostics(arguments.diagnostics_dir, child.pid, time.monotonic() - started)
    stop_lane(child, tree)
    return STALL_EXIT_CODE


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
