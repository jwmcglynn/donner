"""Tests for tools/ci/hosted_parity_throttle.py."""

from pathlib import Path
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
SPIN = [sys.executable, "-c", "x = 0\nfor i in range(6_000_000):\n    x += i\n"]


def _elapsed(command):
    start = time.monotonic()
    status = subprocess.run(command, check=False).returncode
    return status, time.monotonic() - start


class HostedParityThrottleTest(unittest.TestCase):
    def test_descendants_follow_the_parent_links(self):
        table = "  10     1\n  11    10\n  12    11\n  13     1\n  14    10\n"
        self.assertEqual(sorted(throttle.descendants(10, table)), [10, 11, 12, 14])
        self.assertEqual(throttle.descendants(99, table), [99])

    def test_the_test_status_passes_through(self):
        for code in (0, 1, 3):
            with self.subTest(code=code):
                self.assertEqual(code, throttle.run(
                    [sys.executable, "-c", "import sys; sys.exit(%d)" % code]))
        self.assertEqual(2, throttle.main([]))

    def test_cpu_bound_work_slows_by_about_the_run_fraction(self):
        _, plain = _elapsed(SPIN)
        status, throttled = _elapsed([sys.executable, LAUNCHER, *SPIN])
        self.assertEqual(0, status)
        # 1 / RUN_FRACTION is 2.5; scheduling noise makes the bound loose, but an unthrottled
        # run cannot reach it.
        self.assertGreater(throttled / plain, 1.6, (plain, throttled))

    def test_a_terminated_launcher_resumes_and_stops_the_test(self):
        with tempfile.TemporaryDirectory() as directory:
            marker = Path(directory) / "started"
            child = ("import pathlib, signal, sys, time\n"
                     "signal.signal(signal.SIGTERM, lambda *_: sys.exit(42))\n"
                     "pathlib.Path(%r).write_text('x')\n"
                     "while True:\n    time.sleep(0.001)\n" % str(marker))
            launcher = subprocess.Popen([sys.executable, LAUNCHER, sys.executable, "-c", child])
            deadline = time.monotonic() + 30
            while not marker.exists():
                self.assertLess(time.monotonic(), deadline, "the test never started")
                time.sleep(0.01)
            launcher.send_signal(signal.SIGTERM)
            # The test must run its own handler, so it was resumed, and nothing stays stopped.
            self.assertEqual(42, launcher.wait(timeout=30))


if __name__ == "__main__":
    unittest.main()
