"""Runs a test so it gets roughly the CPU capacity of main's GitHub-hosted macOS runner.

The self-hosted runner's cores are about 2.5 times faster than the hosted runner's for this
repository's CPU-bound tests, so a test whose result depends on how fast its threads progress
against a wall-clock deadline can pass there and fail on main. This launcher lets the test's
process tree run for RUN_FRACTION of every PERIOD_S and stops it (SIGSTOP) for the rest, so the
test's CPU work takes about 1 / RUN_FRACTION times longer in wall-clock time while its threads
keep their relative order. GPU work, I/O and timers are not slowed.

Used through `--run_under` by `--config=hosted-parity` (.bazelrc). Exits with the test's status.
The test is always resumed before this launcher exits, including when Bazel stops it with a
signal; a SIGKILL of this launcher leaves the test to Bazel, which kills the whole process group.
"""

import os
from pathlib import Path
import signal
import subprocess
import sys
import time

# Calibrated against main's hosted macOS job: its CPU-bound editor tests take 2 to 3 times as
# long as on the self-hosted runner (for example editor_shell_tests, 46 s against 16 s).
RUN_FRACTION = 0.4
PERIOD_S = 0.02
# How often the process tree is listed again, so new child processes are throttled too.
TREE_REFRESH_S = 0.25


def descendants(root, table):
    """`root` and every process below it, from a `ps -o pid=,ppid=` listing."""
    children = {}
    for line in table.splitlines():
        fields = line.split()
        if len(fields) == 2:
            children.setdefault(int(fields[1]), []).append(int(fields[0]))
    found, pending = [], [root]
    while pending:
        pid = pending.pop()
        found.append(pid)
        pending.extend(children.get(pid, []))
    return found


def _proc_table():
    """`pid ppid` lines from /proc, where it exists (Linux)."""
    lines = []
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit():
            try:
                stat = (entry / "stat").read_text(encoding="utf-8")
            except OSError:
                continue
            # The command name is parenthesized and may contain spaces; the parent follows it.
            fields = stat.rsplit(")", 1)[-1].split()
            if len(fields) > 1:
                lines.append("%s %s" % (entry.name, fields[1]))
    return "\n".join(lines)


def process_tree(root):
    """`root` and its descendants, now."""
    if Path("/proc/self/stat").exists():
        return descendants(root, _proc_table())
    table = subprocess.run(["ps", "-A", "-o", "pid=,ppid="], capture_output=True, text=True,
                           check=False).stdout
    return descendants(root, table)


def signal_all(pids, signum):
    for pid in pids:
        try:
            os.kill(pid, signum)
        except ProcessLookupError:
            pass


def run(command, run_fraction=RUN_FRACTION, period_s=PERIOD_S):
    """Runs `command` throttled and returns its exit status."""
    child = subprocess.Popen(command)
    state = {"pids": [child.pid], "listed": 0.0}

    def resume_and_forward(signum, _frame):
        signal_all(process_tree(child.pid), signal.SIGCONT)
        child.send_signal(signum)

    for signum in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(signum, resume_and_forward)
    try:
        while child.poll() is None:
            time.sleep(period_s * run_fraction)
            if time.monotonic() - state["listed"] > TREE_REFRESH_S:
                state["pids"] = process_tree(child.pid)
                state["listed"] = time.monotonic()
            signal_all(state["pids"], signal.SIGSTOP)
            time.sleep(period_s * (1 - run_fraction))
            signal_all(state["pids"], signal.SIGCONT)
    finally:
        signal_all(process_tree(child.pid), signal.SIGCONT)
    status = child.wait()
    return 128 - status if status < 0 else status


def main(argv):
    if not argv:
        print("usage: hosted_parity_throttle COMMAND [ARGS...]", file=sys.stderr)
        return 2
    return run(argv)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
