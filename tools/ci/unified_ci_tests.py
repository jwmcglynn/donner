"""Regression checks for one Linux runner coordinating independent platform phases."""

import fcntl
import os
from pathlib import Path
import subprocess
import sys
import time
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.ci import lanes, select_remote_cmake, unified_ci


class UnifiedTest(unittest.TestCase):
    def test_serial_darwin_phase_uses_remote_gpu_and_its_own_output(self):
        env = {"RUNNER_TEMP": "/tmp/unit", "UNIFIED_PHASE_BEP": "/tmp/unit/phase.json",
               "TARGETS_AFFECTED": "//pkg:test", "BAZEL_MACOS_BUILD_FLAGS": ""}
        ctx = unified_ci.RemoteMac(lanes.LANES["macos-browser"], env, True)
        action = lanes.LANES["macos-browser"].phases["browser_test"].actions[-1]
        with mock.patch.dict(os.environ, {"MAC_BAZEL_BASE": "/tmp/unit/output"}):
            args = ctx.bazel(action)
            self.assertIn("--config=unified-re-endpoint", args)
            self.assertIn("--config=remote-macos-gpu", args)
            self.assertIn("--jobs=1", args)
            self.assertIn("--noremote_local_fallback", args)
            self.assertNotIn("--config=ci", args)
            self.assertNotIn("--symlink_prefix=/", ctx.query("launches-browser"))
            self.assertIn("//pkg:test", args)
            self.assertIn("--output_base=/tmp/unit/output", args)

    def test_affected_cmake_paths(self):
        for path in ("CMakeLists.txt", "nested/test.cmake.in", "tools/ci/unified_ci.py",
                     "examples/cmake_consumer/main.cc", "third_party/bazel/non_bcr_deps.bzl"):
            self.assertTrue(select_remote_cmake.selected(path), path)
        self.assertFalse(select_remote_cmake.selected("donner/base/example.cc"))

    def test_endpoint_mismatch_stops_before_dispatch(self):
        with mock.patch.object(unified_ci, "prepare"), mock.patch.object(
                unified_ci, "routing", side_effect=["one", "two"]):
            with self.assertRaisesRegex(RuntimeError, "same configured"):
                unified_ci.prepare_job(Path("/tmp/unified-example"))

    def test_failed_child_contributes_to_joined_status(self):
        commands = [[sys.executable, "-c", "pass"], [sys.executable, "-c", "raise SystemExit(3)"]]
        self.assertEqual(unified_ci.supervise(commands), [0, 3])

    def test_cancel_stops_descendant_after_leader_exits(self):
        grandchild = ("import fcntl, os, signal, sys, time\n"
                      "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                      "with open(sys.argv[1], 'w') as lock:\n"
                      " fcntl.flock(lock, fcntl.LOCK_EX)\n"
                      " lock.write(str(os.getpid())); lock.flush()\n"
                      " time.sleep(20)")
        leader = ("import subprocess, sys; "
                  "subprocess.Popen([sys.executable, '-c', sys.argv[1], sys.argv[2]])")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "active"
            process = subprocess.Popen([sys.executable, "-c", leader, grandchild, str(path)],
                                       start_new_session=True)
            try:
                process.wait(timeout=3)
                deadline = time.monotonic() + 3
                while not (path.exists() and path.stat().st_size):
                    if time.monotonic() > deadline:
                        self.fail("grandchild did not acquire the fixture lock")
                    time.sleep(0.02)
                unified_ci.stop_children([process], grace=0.1)
                with path.open() as lock:
                    deadline = time.monotonic() + 2
                    while True:
                        try:
                            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                            break
                        except BlockingIOError:
                            if time.monotonic() > deadline:
                                self.fail("descendant survived cancellation")
                            time.sleep(0.01)
            finally:
                unified_ci.stop_children([process], grace=0.1)

    def test_missing_selection_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp, mock.patch.dict(os.environ, {"TARGETS_FALLBACK": "false",
                                                                                  "TARGETS_AFFECTED": ""}):
            with self.assertRaisesRegex(ValueError, "empty target"):
                unified_ci.combined(Path(temp) / "run")


if __name__ == "__main__":
    unittest.main()
