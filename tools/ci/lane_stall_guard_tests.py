"""Runs the browser CI lane stall guard against real child processes.

No case depends on how fast a host starts processes. A hung lane reports each process it starts
on a pipe, and the test waits for those reports, or for the guard's own output, before it sends
the next signal. The guard's own deadline is a backstop that no case reaches.
"""

import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from unittest import mock

from python.runfiles import runfiles
from tools import test_timing
from tools.ci import lane_stall_guard

# Far beyond every wait in this test. The hung lane's processes block this long, so only the
# guard ends them while it works; if it does not, they still exit on their own instead of
# outliving the test on the host. The guard's deadline is the same backstop.
LIFETIME_SECONDS = 4 * test_timing.STALL_GUARD_SECONDS

# A lane that reports itself and blocks. With `detached`, it first starts two descendants that
# leave its process tree, the way a browser driver launches a browser in a new session and a
# browser's crash handler outlives it: one in a new session, and one double-forked into a new
# session so that it is reparented away from the lane. Each inherits the write end of the pipe,
# reports itself on it and blocks. With `stubborn`, the lane ignores SIGTERM. With `request`, the
# lane asks its guard for the stall path itself, possibly before the guard waits for it.
HUNG_LANE = textwrap.dedent("""\
    import os, signal, subprocess, sys, threading
    write_fd, lifetime, mode = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
    if mode == "stubborn":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    if mode == "request":
        os.kill(os.getppid(), signal.SIGUSR1)
    def report():
        os.write(write_fd, b"ready %d\\n" % os.getpid())
    if mode == "detached":
        subprocess.Popen(
            [sys.executable, "-c",
             "import os, sys, threading\\n"
             "os.write(int(sys.argv[1]), b'ready %d\\\\n' % os.getpid())\\n"
             "threading.Event().wait(float(sys.argv[2]))\\n",
             str(write_fd), str(lifetime)],
            pass_fds=(write_fd,), start_new_session=True)
        first = os.fork()
        if first == 0:
            os.setsid()
            if os.fork() == 0:
                report()
                threading.Event().wait(lifetime)
            os._exit(0)
        os.waitpid(first, 0)
    report()
    threading.Event().wait(lifetime)
""")


def read_lines(read_fd: int, count: int) -> list[str]:
    """The first `count` lines written to the pipe, bounded by the stall guard."""
    data = b""
    while data.count(b"\n") < count:
        ready, _, _ = select.select([read_fd], [], [], test_timing.STALL_GUARD_SECONDS)
        chunk = os.read(read_fd, 4096) if ready else b""
        if not chunk:
            raise AssertionError(f"expected {count} reports, got {data!r}")
        data += chunk
    return data.decode().splitlines()


def read_until_closed(read_fd: int) -> bytes:
    """Everything written to the pipe once every writer has closed it, bounded by the guard."""
    data = b""
    while True:
        ready, _, _ = select.select([read_fd], [], [], test_timing.STALL_GUARD_SECONDS)
        if not ready:
            raise AssertionError(
                f"a lane process still holds the pipe after {test_timing.STALL_GUARD_SECONDS} s;"
                f" read after the reports: {data!r}")
        chunk = os.read(read_fd, 4096)
        if not chunk:
            return data
        data += chunk


def read_output_until(guard: subprocess.Popen, marker: bytes, seen: bytes = b"") -> bytes:
    """The guard's output up to and including `marker`, bounded by the stall guard."""
    data = seen
    while marker not in data:
        ready, _, _ = select.select([guard.stdout], [], [], test_timing.STALL_GUARD_SECONDS)
        chunk = os.read(guard.stdout.fileno(), 4096) if ready else b""
        if not chunk:
            raise AssertionError(f"the guard never printed {marker!r}; it printed {data!r}")
        data += chunk
    return data


class ParsingTest(unittest.TestCase):
    def test_elapsed_times(self):
        self.assertEqual(lane_stall_guard.parse_elapsed_seconds("00:07"), 7)
        self.assertEqual(lane_stall_guard.parse_elapsed_seconds("01:02:03"), 3723)
        self.assertEqual(lane_stall_guard.parse_elapsed_seconds("2-00:00:01"), 172801)
        self.assertIsNone(lane_stall_guard.parse_elapsed_seconds("x:01"))
        self.assertIsNone(lane_stall_guard.parse_elapsed_seconds("7"))

    def test_lane_tree_follows_parents_and_crash_handlers_are_matched_by_age(self):
        started = "Sat Nov 14 07:31:57 2026"
        table = "".join(f"{pid} {ppid} {pid} S {elapsed} 100 {started} {command}\n"
                        for pid, ppid, elapsed, command in (
                            (1, 0, "10-00:00:00", "/sbin/launchd"),
                            (10, 1, "05:00", "python3"),
                            (11, 10, "04:59", "node"),
                            (12, 11, "04:58", "/cache/Chromium.app/Contents/MacOS/Chromium"),
                            (13, 1, "04:57", "/cache/chrome_crashpad_handler"),
                            (14, 1, "01:00:00", "/other/chrome_crashpad_handler"),
                            (15, 1, "00:01", "unrelated")))
        rows = lane_stall_guard.parse_process_table(table)
        self.assertEqual(len(rows), 7)
        self.assertEqual(rows[3]["command"], "/cache/Chromium.app/Contents/MacOS/Chromium")
        self.assertEqual([row["pid"] for row in lane_stall_guard.lane_tree(rows, 11)], [11, 12])
        self.assertEqual(
            [row["pid"] for row in lane_stall_guard.detached_helpers(rows, lane_seconds=300)],
            [13])

    def test_start_times_are_read_in_the_c_locale_format(self):
        self.assertEqual(lane_stall_guard.parse_start_time("Sat Nov 14 07:31:57 2026"),
                         int(time.mktime((2026, 11, 14, 7, 31, 57, 0, 0, -1))))
        self.assertEqual(lane_stall_guard.parse_start_time("Wed Nov 4 07:31:57 2026"),
                         int(time.mktime((2026, 11, 4, 7, 31, 57, 0, 0, -1))))
        self.assertIsNone(lane_stall_guard.parse_start_time("Sam Nov 14 07:31:57 2026"))

    def test_only_processes_started_since_the_guard_are_signalled(self):
        members = [
            {"pid": 10, "started": 1000},  # The root: signalled through its group only.
            {"pid": 11, "started": 1000},
            {"pid": 12, "started": 1001},
            {"pid": 13, "started": 999},  # A reused pid: an older process.
            {"pid": 14, "started": None},  # An unreadable start time.
        ]
        self.assertEqual(lane_stall_guard.signal_targets(members, 10, not_before=1000), [11, 12])

    def test_signal_ends_map_to_shell_statuses(self):
        self.assertEqual(lane_stall_guard.exit_status(3), 3)
        self.assertEqual(lane_stall_guard.exit_status(-signal.SIGTERM), 128 + signal.SIGTERM)


class RunningChild:
    pid = 2 ** 22
    returncode = None


class ReapedChild(RunningChild):
    returncode = 0


class GuardStateTest(unittest.TestCase):
    def test_a_stall_request_outside_the_wait_is_only_recorded(self):
        guard = lane_stall_guard.Guard()
        guard.request_stall()
        self.assertTrue(guard.stall)

    def test_a_stall_request_during_the_wait_raises_once(self):
        guard = lane_stall_guard.Guard()
        guard.waiting = True
        with self.assertRaises(lane_stall_guard.StallRequested):
            guard.request_stall()
        self.assertFalse(guard.waiting)
        guard.request_stall()  # A second request while the stall path runs does not raise.

    def test_a_signal_after_the_lane_was_reaped_is_not_forwarded(self):
        guard = lane_stall_guard.Guard()
        guard.child = ReapedChild()
        with mock.patch.object(lane_stall_guard.os, "killpg") as killpg:
            guard.forward(signal.SIGTERM, None)
        killpg.assert_not_called()
        self.assertEqual(guard.signal, signal.SIGTERM)

    def test_a_second_interrupt_during_the_wait_requests_the_stall_path(self):
        guard = lane_stall_guard.Guard()
        guard.child = RunningChild()
        guard.waiting = True
        with mock.patch.object(lane_stall_guard.os, "killpg") as killpg, \
                mock.patch.object(lane_stall_guard.os, "write"):
            guard.forward(signal.SIGINT, None)
            with self.assertRaises(lane_stall_guard.StallRequested):
                guard.forward(signal.SIGINT, None)
        self.assertEqual(killpg.call_args_list,
                         [mock.call(RunningChild.pid, signal.SIGINT)] * 2)


class GuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.guard = Path(runfiles.Create().Rlocation("donner/tools/ci/lane_stall_guard.py"))

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="lane stall guard ")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.diagnostics = self.work / "diagnostics"
        self.lane = self.work / "hung_lane.py"
        self.lane.write_text(HUNG_LANE, encoding="utf-8")

    def guard_command(self, command, diagnostics=None):
        return [sys.executable, str(self.guard), "--deadline-seconds", str(LIFETIME_SECONDS),
                "--diagnostics-dir", str(diagnostics or self.diagnostics), "--", *command]

    def start_hung_lane(self, mode, diagnostics=None):
        """Starts the guard on a hung lane; returns the guard, the pipe and the reported pids."""
        read_fd, write_fd = os.pipe()
        self.addCleanup(os.close, read_fd)
        try:
            guard = subprocess.Popen(
                self.guard_command([sys.executable, str(self.lane), str(write_fd),
                                    str(LIFETIME_SECONDS), mode], diagnostics),
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, pass_fds=(write_fd,))
        finally:
            os.close(write_fd)
        self.addCleanup(lambda: guard.poll() is None and guard.kill())
        if mode == "request":
            return guard, read_fd, []
        reports = read_lines(read_fd, 3 if mode == "detached" else 1)
        return guard, read_fd, [line.split()[1] for line in reports]

    def finish(self, guard, seen=b""):
        output, _ = guard.communicate(timeout=test_timing.STALL_GUARD_SECONDS)
        return guard.returncode, (seen + output).decode(errors="replace")

    def test_a_lane_that_exits_keeps_its_status_and_records_nothing(self):
        result = subprocess.run(
            self.guard_command([sys.executable, "-c", "import sys; sys.exit(7)"]),
            capture_output=True, text=True, timeout=test_timing.STALL_GUARD_SECONDS)
        self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
        self.assertNotIn("lane stall guard", result.stdout)
        self.assertFalse(self.diagnostics.exists())

    def test_a_lane_ended_by_a_signal_reports_the_shell_status(self):
        result = subprocess.run(
            self.guard_command([sys.executable, "-c",
                                "import os, signal; os.kill(os.getpid(), signal.SIGTERM)"]),
            capture_output=True, text=True, timeout=test_timing.STALL_GUARD_SECONDS)
        self.assertEqual(result.returncode, 128 + signal.SIGTERM, result.stdout + result.stderr)

    def test_a_stalled_lane_is_recorded_and_stopped_with_every_descendant(self):
        guard, read_fd, pids = self.start_hung_lane("detached")
        guard.send_signal(signal.SIGUSR1)
        seen = read_output_until(guard, b"::error::lane stall guard")
        guard.send_signal(signal.SIGUSR1)  # Ignored once the stall path has started.
        status, output = self.finish(guard, seen)
        # End of file means no process the lane started still holds the pipe.
        self.assertEqual(read_until_closed(read_fd), b"")
        self.assertEqual(status, lane_stall_guard.STALL_EXIT_CODE, output)
        self.assertIn("::error::lane stall guard: the lane is still running", output)
        recorded = (self.diagnostics / "lane-processes.txt").read_text(encoding="utf-8")
        recorded_pids = {line.split()[0] for line in recorded.splitlines()}
        for pid in pids:
            self.assertIn(pid, recorded_pids, recorded)
        for name in ("process-table.txt", "open-files.txt"):
            self.assertTrue((self.diagnostics / name).is_file(), name)

    def test_a_failure_to_record_still_stops_the_lane(self):
        blocker = self.work / "not-a-directory"
        blocker.write_text("", encoding="utf-8")
        guard, read_fd, _ = self.start_hung_lane("alone", blocker / "diagnostics")
        guard.send_signal(signal.SIGUSR1)
        status, output = self.finish(guard)
        self.assertEqual(read_until_closed(read_fd), b"")
        self.assertEqual(status, lane_stall_guard.STALL_EXIT_CODE, output)
        self.assertIn("lane stall guard: recording diagnostics failed: NotADirectoryError", output)

    def test_a_terminated_guard_stops_the_lane_and_ends_by_the_same_signal(self):
        guard, read_fd, _ = self.start_hung_lane("alone")
        guard.send_signal(signal.SIGTERM)
        status, output = self.finish(guard)
        self.assertEqual(read_until_closed(read_fd), b"")
        self.assertEqual(status, -signal.SIGTERM, output)
        self.assertIn("lane stall guard: forwarded SIGTERM to the lane", output)
        self.assertNotIn("::error::", output)

    def test_a_second_interrupt_stops_a_lane_that_ignores_the_first(self):
        guard, read_fd, _ = self.start_hung_lane("stubborn")
        guard.send_signal(signal.SIGTERM)
        seen = read_output_until(guard, b"forwarded SIGTERM to the lane")
        guard.send_signal(signal.SIGTERM)
        status, output = self.finish(guard, seen)
        self.assertEqual(read_until_closed(read_fd), b"")
        self.assertEqual(status, -signal.SIGTERM, output)
        self.assertIn("::error::lane stall guard: the lane is still running", output)

    def test_a_stall_request_from_the_lane_itself_is_honoured(self):
        guard, read_fd, _ = self.start_hung_lane("request")
        status, output = self.finish(guard)
        # The lane may or may not report before it is stopped; either way it must be gone.
        self.assertRegex(read_until_closed(read_fd).decode(), r"^(ready \d+\n)?$")
        self.assertEqual(status, lane_stall_guard.STALL_EXIT_CODE, output)


if __name__ == "__main__":
    unittest.main()
