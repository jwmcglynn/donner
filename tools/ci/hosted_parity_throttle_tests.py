"""Tests for tools/ci/hosted_parity_throttle.py."""

from pathlib import Path
import resource
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hosted_parity_throttle as throttle  # noqa: E402

LAUNCHER = str(Path(__file__).resolve().parent / "hosted_parity_throttle.py")
# A CPU-bound child: a fixed amount of Python work.
SPIN = [sys.executable, "-c", "x = 0\nfor i in range(8_000_000):\n    x += i\n"]


class _FakeChild:
    """A child that exits after a number of polls."""

    def __init__(self, polls_until_exit, status=0):
        self.pid = 100
        self.polls = polls_until_exit
        self.status = status
        self.waited = 0

    def poll(self):
        self.polls -= 1
        return self.status if self.polls < 0 else None

    def wait(self):
        self.waited += 1
        return self.status


class _Recorder:
    """Records every kill and sleep in order, and lists a fixed process tree."""

    def __init__(self, tree=(100, 101)):
        self.events = []
        self.tree_pids = list(tree)
        self.listings = 0
        self.now = 0.0

    def kill(self, pid, signum):
        self.events.append(("kill", pid, signum))

    def sleep(self, seconds):
        self.events.append(("sleep", round(seconds, 6)))
        self.now += seconds

    def clock(self):
        return self.now

    def tree(self, _root):
        self.listings += 1
        return list(self.tree_pids)


def _throttle(child, recorder, **kwargs):
    return throttle.Throttle(child, kill=recorder.kill, sleep=recorder.sleep,
                             clock=recorder.clock, tree=recorder.tree, **kwargs)


def _child_cpu_seconds():
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


def _wait_for(path, test):
    deadline = time.monotonic() + 30
    while not path.exists() or not path.read_text():
        test.assertLess(time.monotonic(), deadline, "%s never appeared" % path)
        time.sleep(0.01)
    return path.read_text()


class HostedParityThrottleTest(unittest.TestCase):
    def test_descendants_follow_the_parent_links(self):
        table = "  10     1\n  11    10\n  12    11\n  13     1\n  14    10\nbad line\n"
        self.assertEqual(sorted(throttle.descendants(10, table)), [10, 11, 12, 14])
        self.assertEqual(throttle.descendants(99, table), [99])

    def test_process_tree_finds_a_real_grandchild(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "grandchild"
            script = ("import pathlib, subprocess, sys\n"
                      "g = subprocess.Popen([sys.executable, '-c', "
                      "'import time; time.sleep(60)'])\n"
                      "pathlib.Path(%r).write_text(str(g.pid))\n"
                      "g.wait()\n" % str(marker))
            child = subprocess.Popen([sys.executable, "-c", script])
            try:
                grandchild = int(_wait_for(marker, self))
                self.assertEqual({child.pid, grandchild},
                                 set(throttle.process_tree(child.pid)))
            finally:
                throttle.signal_all(throttle.process_tree(child.pid), signal.SIGKILL)
                child.wait()

    def test_a_cycle_runs_then_stops_and_resumes_the_tree(self):
        recorder = _Recorder()
        status = _throttle(_FakeChild(2), recorder, run_fraction=0.4, period_s=0.02).run()
        self.assertEqual(0, status)
        stop, resume = signal.SIGSTOP, signal.SIGCONT
        cycle = [("sleep", 0.008), ("kill", 100, stop), ("kill", 101, stop), ("sleep", 0.012),
                 ("kill", 100, resume), ("kill", 101, resume)]
        # Two cycles, then the final resume before waiting for the child.
        self.assertEqual(recorder.events, cycle + cycle + [("kill", 100, resume),
                                                           ("kill", 101, resume)])

    def test_the_tree_is_listed_again_only_after_the_refresh_interval(self):
        recorder = _Recorder()
        _throttle(_FakeChild(30), recorder, period_s=0.02).run()
        # 30 cycles of 20 ms is 0.6 s: listings at 0, after 0.25 s and after 0.5 s, plus the
        # final resume.
        self.assertEqual(4, recorder.listings)

    def test_a_terminating_signal_resumes_and_forwards_once(self):
        recorder = _Recorder()
        child = _FakeChild(1000)
        instance = _throttle(child, recorder)

        def sleep_then_signal(seconds):
            recorder.events.append(("sleep", seconds))
            instance.on_signal(signal.SIGTERM)
            instance.on_signal(signal.SIGINT)

        instance.sleep = sleep_then_signal
        instance.run()
        kills = [event for event in recorder.events if event[0] == "kill"]
        self.assertEqual([("kill", 100, signal.SIGCONT), ("kill", 101, signal.SIGCONT),
                          ("kill", 100, signal.SIGTERM),
                          ("kill", 100, signal.SIGCONT), ("kill", 101, signal.SIGCONT)], kills)
        self.assertEqual(signal.SIGTERM, instance.terminating)
        self.assertEqual(1, child.waited)

    def test_an_error_resumes_and_stops_the_test_before_raising(self):
        recorder = _Recorder()
        child = _FakeChild(1000)

        def failing_kill(pid, signum):
            recorder.events.append(("kill", pid, signum))
            if signum == signal.SIGSTOP:
                raise RuntimeError("boom")

        instance = throttle.Throttle(child, kill=failing_kill, sleep=recorder.sleep,
                                     clock=recorder.clock, tree=recorder.tree)
        with self.assertRaises(RuntimeError):
            instance.run()
        self.assertEqual(recorder.events[-3:], [("kill", 100, signal.SIGCONT),
                                                ("kill", 101, signal.SIGCONT),
                                                ("kill", 100, signal.SIGTERM)])
        self.assertEqual(1, child.waited)

    def test_signal_errors_and_listing_failures_are_tolerated(self):
        def gone(_pid, _signum):
            raise PermissionError

        throttle.signal_all([1, 2], signal.SIGCONT, gone)

        def failing_run(*_args, **_kwargs):
            raise OSError("no ps")

        original = throttle.subprocess.run
        throttle.subprocess.run = failing_run
        try:
            if not Path("/proc/self/stat").exists():
                self.assertEqual([42], throttle.process_tree(42))
        finally:
            throttle.subprocess.run = original

    def test_the_test_status_passes_through(self):
        for code in (0, 1, 3):
            with self.subTest(code=code):
                status = subprocess.run(
                    [sys.executable, LAUNCHER, sys.executable, "-c",
                     "import sys; sys.exit(%d)" % code], check=False).returncode
                self.assertEqual(code, status)
        self.assertEqual(2, throttle.main([]))

    def test_cpu_bound_work_takes_longer_than_its_cpu_time(self):
        # Wall time over the child's own CPU time, so a loaded machine slows both alike. The
        # nominal ratio is 1 / RUN_FRACTION = 2.5; an unthrottled child stays near 1.
        before = _child_cpu_seconds()
        start = time.monotonic()
        status = subprocess.run([sys.executable, LAUNCHER, *SPIN], check=False).returncode
        wall = time.monotonic() - start
        cpu = _child_cpu_seconds() - before
        self.assertEqual(0, status)
        self.assertGreater(wall / cpu, 1.5, (wall, cpu))

    def test_a_terminated_launcher_resumes_the_test_and_ends_by_the_same_signal(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "started"
            child = ("import pathlib, signal, sys, time\n"
                     "signal.signal(signal.SIGTERM, lambda *_: sys.exit(42))\n"
                     "pathlib.Path(%r).write_text('x')\n"
                     "while True:\n    time.sleep(0.001)\n" % str(marker))
            launcher = subprocess.Popen([sys.executable, LAUNCHER, sys.executable, "-c", child])
            _wait_for(marker, self)
            launcher.send_signal(signal.SIGTERM)
            # The test ran its own handler (so it was resumed), and the launcher then ended by
            # the signal it received.
            self.assertEqual(-signal.SIGTERM, launcher.wait(timeout=30))


if __name__ == "__main__":
    unittest.main()
