"""Runs the browser CI lane stall guard against real child processes."""

import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import textwrap
import unittest

from python.runfiles import runfiles
from tools import test_timing
from tools.ci import lane_stall_guard

# The guard's deadline in the hang case. The lane below is ready within a fraction of it.
HANG_DEADLINE_SECONDS = 3

# How long the hung lane's processes block. Far beyond every wait in this test, so only the guard
# ends them while it works; if it does not, they still exit on their own instead of outliving the
# test on the host.
HUNG_LANE_LIFETIME_SECONDS = 4 * test_timing.STALL_GUARD_SECONDS

# A lane that starts a detached descendant, the way a browser driver launches a browser in a new
# session. The descendant inherits the write end of a pipe, reports itself on it and then blocks,
# as does the lane, so only the guard can end either of them within the test.
HUNG_LANE = textwrap.dedent("""\
    import os, subprocess, sys, threading
    write_fd, lifetime = int(sys.argv[1]), float(sys.argv[2])
    subprocess.Popen(
        [sys.executable, "-c",
         "import os, sys, threading\\n"
         "os.write(int(sys.argv[1]), b'ready %d\\\\n' % os.getpid())\\n"
         "threading.Event().wait(float(sys.argv[2]))\\n",
         str(write_fd), str(lifetime)],
        pass_fds=(write_fd,), start_new_session=True)
    threading.Event().wait(lifetime)
""")


def read_until_closed(read_fd: int) -> bytes:
    """Everything written to the pipe once every writer has closed it, bounded by the guard."""
    data = b""
    while True:
        ready, _, _ = select.select([read_fd], [], [], test_timing.STALL_GUARD_SECONDS)
        if not ready:
            raise AssertionError(
                f"a lane process still holds the pipe after {test_timing.STALL_GUARD_SECONDS} s;"
                f" read so far: {data!r}")
        chunk = os.read(read_fd, 4096)
        if not chunk:
            return data
        data += chunk


class ParsingTest(unittest.TestCase):
    def test_elapsed_times(self):
        self.assertEqual(lane_stall_guard.parse_elapsed_seconds("00:07"), 7)
        self.assertEqual(lane_stall_guard.parse_elapsed_seconds("01:02:03"), 3723)
        self.assertEqual(lane_stall_guard.parse_elapsed_seconds("2-00:00:01"), 172801)
        self.assertIsNone(lane_stall_guard.parse_elapsed_seconds("x:01"))
        self.assertIsNone(lane_stall_guard.parse_elapsed_seconds("7"))

    def test_lane_tree_follows_parents_and_crash_handlers_are_matched_by_age(self):
        table = (
            "  1     0     1 Ss  10-00:00:00   100 /sbin/launchd\n"
            " 10     1    10 S       05:00   100 python3\n"
            " 11    10    11 S       04:59   100 node\n"
            " 12    11    12 S       04:58   100 /cache/Chromium.app/Contents/MacOS/Chromium\n"
            " 13     1    13 S       04:57   100 /cache/chrome_crashpad_handler\n"
            " 14     1    14 S    01:00:00   100 /other/chrome_crashpad_handler\n"
            " 15     1    15 S       00:01   100 unrelated\n"
        )
        rows = lane_stall_guard.parse_process_table(table)
        self.assertEqual([row["pid"] for row in lane_stall_guard.lane_tree(rows, 11)], [11, 12])
        self.assertEqual(
            [row["pid"] for row in lane_stall_guard.detached_helpers(rows, lane_seconds=300)],
            [13])


class GuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.guard = Path(runfiles.Create().Rlocation("donner/tools/ci/lane_stall_guard.py"))

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="lane stall guard ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.diagnostics = self.work / "diagnostics"

    def run_guard(self, deadline_seconds, command, **options):
        return subprocess.run(
            [sys.executable, str(self.guard), "--deadline-seconds", str(deadline_seconds),
             "--diagnostics-dir", str(self.diagnostics), "--", *command],
            capture_output=True, text=True, timeout=test_timing.STALL_GUARD_SECONDS, **options)

    def test_a_lane_that_exits_keeps_its_status_and_records_nothing(self):
        result = self.run_guard(test_timing.STALL_GUARD_SECONDS,
                                [sys.executable, "-c", "import sys; sys.exit(7)"])
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        self.assertNotIn("lane stall guard", result.stdout)
        self.assertFalse(self.diagnostics.exists())

    def test_a_hung_lane_is_recorded_and_stopped_with_its_detached_descendants(self):
        lane = self.work / "hung_lane.py"
        lane.write_text(HUNG_LANE, encoding="utf-8")
        read_fd, write_fd = os.pipe()
        self.addCleanup(os.close, read_fd)
        try:
            result = self.run_guard(HANG_DEADLINE_SECONDS,
                                    [sys.executable, str(lane), str(write_fd),
                                     str(HUNG_LANE_LIFETIME_SECONDS)],
                                    pass_fds=(write_fd,))
        finally:
            os.close(write_fd)
        # End of file means no process the lane started still holds the pipe.
        written = read_until_closed(read_fd).decode()
        self.assertRegex(written, r"^ready \d+\n$",
                         "the detached descendant never started before the guard fired")
        descendant = written.split()[1]

        self.assertEqual(result.returncode, lane_stall_guard.STALL_EXIT_CODE,
                         result.stdout + result.stderr)
        self.assertIn("::error::lane stall guard: the lane is still running after 3 s",
                      result.stdout)
        recorded = (self.diagnostics / "lane-processes.txt").read_text(encoding="utf-8")
        self.assertIn(f"{descendant} ", recorded, recorded)
        self.assertIn(descendant, result.stdout)
        for name in ("process-table.txt", "open-files.txt"):
            self.assertTrue((self.diagnostics / name).is_file(), name)


if __name__ == "__main__":
    unittest.main()
