"""Runs a test so it gets roughly the CPU capacity of main's GitHub-hosted macOS runner.

The self-hosted runner's cores are about 2.5 times faster than the hosted runner's for this
repository's CPU-bound tests, so a test whose result depends on how fast its threads progress
against a wall-clock deadline can pass there and fail on main. This launcher lets the test's
process tree run for RUN_FRACTION of every PERIOD_S and stops it (SIGSTOP) for the rest, so the
test's CPU work takes about 1 / RUN_FRACTION times longer in wall-clock time while its threads
keep their relative order. GPU work, I/O and timers are not slowed.

It is an approximation. Each process in the tree is stopped and resumed separately, so
processes of the tree can briefly run while others are stopped. The tree is listed again only
every TREE_REFRESH_S, so a new child process runs unthrottled until then. RUN_FRACTION is
nominal: the slowdown actually observed depends on scheduling. The value was calibrated and
validated on macOS only.

Used through `--run_under` by `--config=hosted-parity-runtime` (.bazelrc). Exits with the test's
status. On the first SIGTERM, SIGINT, SIGHUP or SIGQUIT the launcher stops throttling, resumes
the test, forwards that signal once, waits for the test, and then ends with the same signal, so
Bazel sees the termination it would see without the launcher. Any error in the launcher also
resumes the test and terminates it before re-raising. A SIGKILL of the launcher leaves the test
to Bazel, which kills the whole process group.
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
TERMINATING_SIGNALS = (signal.SIGTERM, signal.SIGINT, signal.SIGHUP, signal.SIGQUIT)


def descendants(root, table):
    """`root` and every process below it, from a `ps -o pid=,ppid=` listing."""
    children = {}
    for line in table.splitlines():
        fields = line.split()
        if len(fields) == 2 and all(field.isdigit() for field in fields):
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
    """`root` and its descendants, now. If they cannot be listed, `root` alone."""
    try:
        if Path("/proc/self/stat").exists():
            return descendants(root, _proc_table())
        table = subprocess.run(["ps", "-A", "-o", "pid=,ppid="], capture_output=True,
                               text=True, check=False, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return [root]
    return descendants(root, table)


def signal_all(pids, signum, kill=os.kill):
    for pid in pids:
        try:
            kill(pid, signum)
        except OSError:
            pass


class Throttle:
    """Duty-cycles one child's process tree. Sleep, clock, kill and tree listing are injectable
    so the stop/resume sequence can be tested without real processes."""

    def __init__(self, child, *, run_fraction=RUN_FRACTION, period_s=PERIOD_S, kill=os.kill,
                 sleep=time.sleep, clock=time.monotonic, tree=process_tree):
        self.child = child
        self.run_fraction = run_fraction
        self.period_s = period_s
        self.kill = kill
        self.sleep = sleep
        self.clock = clock
        self.tree = tree
        self.terminating = None
        self.pids = [child.pid]
        self.listed = None

    def _resume(self):
        signal_all(self.tree(self.child.pid), signal.SIGCONT, self.kill)

    def on_signal(self, signum, _frame=None):
        """Stops throttling, resumes the test and forwards the first terminating signal."""
        if self.terminating is not None:
            return
        self.terminating = signum
        self._resume()
        signal_all([self.child.pid], signum, self.kill)

    def cycle(self):
        """One period: let the tree run, then stop it, then resume it."""
        self.sleep(self.period_s * self.run_fraction)
        if self.terminating is not None:
            return
        if self.listed is None or self.clock() - self.listed > TREE_REFRESH_S:
            self.pids = self.tree(self.child.pid)
            self.listed = self.clock()
        signal_all(self.pids, signal.SIGSTOP, self.kill)
        try:
            self.sleep(self.period_s * (1 - self.run_fraction))
        finally:
            signal_all(self.pids, signal.SIGCONT, self.kill)

    def run(self):
        """Throttles until the child exits or a terminating signal arrives; returns its status."""
        try:
            while self.child.poll() is None and self.terminating is None:
                self.cycle()
        except BaseException:
            self._resume()
            signal_all([self.child.pid], signal.SIGTERM, self.kill)
            self.child.wait()
            raise
        self._resume()
        return self.child.wait()


def main(argv):
    if not argv:
        print("usage: hosted_parity_throttle COMMAND [ARGS...]", file=sys.stderr)
        return 2
    child = subprocess.Popen(argv)
    throttle = Throttle(child)
    for signum in TERMINATING_SIGNALS:
        signal.signal(signum, throttle.on_signal)
    status = throttle.run()
    if throttle.terminating is not None:
        # End the way the test would have without the launcher.
        signal.signal(throttle.terminating, signal.SIG_DFL)
        os.kill(os.getpid(), throttle.terminating)
    return 128 - status if status < 0 else status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
