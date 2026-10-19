#!/usr/bin/env python3
"""Tests for ci_disk_cache.py."""

from contextlib import redirect_stdout
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ci_disk_cache

MIB = 1024 * 1024
# Just under half a MiB, so two entries and the directory headers fit a 1 MiB budget.
HALF = MIB // 2 - 8192
# Fixture mtimes, in seconds since the epoch: older entries have smaller values.
OLD, MIDDLE, NEW = 1_000_000, 2_000_000, 3_000_000


class CiDiskCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.cache = self.root / "bazel-disk"
        self.output = self.root / "github-output"

    def _entry(self, relative: str, size: int, mtime: int) -> Path:
        path = self.cache / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"x" * size)
        os.utime(path, (mtime, mtime))
        return path

    def _marker(self, mtime: int) -> Path:
        marker = self.root / "restore.marker"
        marker.touch()
        os.utime(marker, (mtime, mtime))
        return marker

    def _bound(self, *extra: str) -> str:
        stdout = io.StringIO()
        with redirect_stdout(stdout):
            status = ci_disk_cache.main(["bound", "--cache-dir", str(self.cache),
                                         "--github-output", str(self.output), *extra])
        self.assertEqual(status, 0)
        return stdout.getvalue()

    def _save_output(self) -> str:
        lines = self.output.read_text().splitlines()
        self.assertEqual(1, len(lines), lines)
        return lines[0]

    def _estimate(self) -> int:
        return ci_disk_cache.archive_estimate(*ci_disk_cache.scan(str(self.cache)))

    def test_tar_bytes_counts_header_and_padded_data(self) -> None:
        self.assertEqual(1536, ci_disk_cache.tar_bytes(0))
        self.assertEqual(2048, ci_disk_cache.tar_bytes(1))
        self.assertEqual(2048, ci_disk_cache.tar_bytes(512))
        self.assertEqual(2560, ci_disk_cache.tar_bytes(513))

    def test_cache_within_budget_is_saved_unchanged(self) -> None:
        kept = [self._entry("ac/aa/one", 1000, OLD), self._entry("cas/bb/two", 5000, NEW)]
        self._bound("--max-mib", "1")
        self.assertEqual("save=true", self._save_output())
        self.assertTrue(all(path.exists() for path in kept))

    def test_evicts_least_recently_used_until_within_budget(self) -> None:
        oldest = self._entry("cas/aa/oldest", HALF, OLD)
        middle = self._entry("cas/bb/middle", HALF, MIDDLE)
        newest = self._entry("cas/cc/newest", HALF, NEW)
        report = self._bound("--max-mib", "1")
        self.assertEqual("save=true", self._save_output())
        self.assertFalse(oldest.exists())
        self.assertTrue(middle.exists())
        self.assertTrue(newest.exists())
        self.assertLessEqual(self._estimate(), MIB)
        self.assertIn("evicting 1 files", report)

    def test_equal_mtimes_evict_action_results_before_their_blobs(self) -> None:
        # Bazel's collector breaks mtime ties by path, so ac/ goes before cas/ and an LRU trim
        # cannot leave an action result whose referenced blob is gone.
        action = self._entry("ac/aa/result", HALF, MIDDLE)
        blob = self._entry("cas/aa/blob", HALF, MIDDLE)
        self._entry("cas/bb/newer", HALF, NEW)
        self._bound("--max-mib", "1")
        self.assertFalse(action.exists())
        self.assertTrue(blob.exists())

    def test_orphaned_temporary_writes_are_evicted_first(self) -> None:
        orphan = self._entry("tmp/unfinished", 100, NEW)
        older = self._entry("cas/aa/older", 100, OLD)
        self._bound("--max-mib", "1")
        self.assertEqual("save=true", self._save_output())
        self.assertFalse(orphan.exists())
        self.assertTrue(older.exists())

    def test_dry_run_deletes_nothing_and_never_saves(self) -> None:
        entries = [self._entry("cas/aa/%d" % index, HALF, OLD + index) for index in range(4)]
        report = self._bound("--max-mib", "1", "--dry-run")
        self.assertEqual("save=false", self._save_output())
        self.assertTrue(all(path.exists() for path in entries))
        self.assertIn("would evict 2 files", report)

    def test_reports_the_working_set_used_since_restore(self) -> None:
        self._entry("cas/aa/restored", 2000, OLD)
        self._entry("cas/bb/used", 3000, NEW)
        marker = self._marker(MIDDLE)
        report = self._bound("--max-mib", "1", "--marker", str(marker))
        self.assertIn("used or written since restore: 1 files", report)

    def test_cache_still_over_budget_is_not_saved(self) -> None:
        self._entry("cas/aa/stuck", 2 * MIB, OLD)
        with mock.patch.object(ci_disk_cache.os, "unlink"):
            report = self._bound("--max-mib", "1")
        self.assertEqual("save=false", self._save_output())
        self.assertIn("over its 1.0 MiB budget", report)

    def test_error_while_bounding_is_not_saved(self) -> None:
        self._entry("cas/aa/entry", 100, OLD)
        with mock.patch.object(ci_disk_cache, "scan", side_effect=PermissionError("denied")):
            report = self._bound("--max-mib", "1")
        self.assertEqual("save=false", self._save_output())
        self.assertIn("Disk cache not bounded", report)

    def test_missing_cache_is_not_saved(self) -> None:
        self._bound("--max-mib", "1")
        self.assertEqual("save=false", self._save_output())

    def test_symlinks_are_never_followed_or_deleted(self) -> None:
        outside = self.root / "outside"
        outside.write_bytes(b"x" * (2 * MIB))
        self.cache.mkdir()
        (self.cache / "link").symlink_to(outside)
        self._bound("--max-mib", "1")
        self.assertEqual("save=true", self._save_output())
        self.assertTrue((self.cache / "link").is_symlink())
        self.assertTrue(outside.exists())

    def test_budget_must_be_positive(self) -> None:
        with redirect_stdout(io.StringIO()), mock.patch("sys.stderr", io.StringIO()):
            with self.assertRaises(SystemExit):
                ci_disk_cache.main(["bound", "--cache-dir", str(self.cache), "--max-mib", "0"])

    def test_probe_reports_usage_and_tolerates_absent_directories(self) -> None:
        self._entry("cas/aa/entry", 4096, OLD)
        marker = self._marker(MIDDLE)
        stdout = io.StringIO()
        # Keep the probe off the host's real Bazel caches.
        with redirect_stdout(stdout), mock.patch.dict(os.environ, {"HOME": str(self.root)}), \
                mock.patch.object(ci_disk_cache, "output_root",
                                  return_value=str(self.root / "output-root")):
            status = ci_disk_cache.main(["probe", "--stage", "after-test", "--cache-dir",
                                         str(self.cache), "--marker", str(marker)])
        self.assertEqual(status, 0)
        report = stdout.getvalue()
        self.assertIn("::group::Runner resources: after-test", report)
        self.assertIn("Bazel disk cache (%s): 1 files" % self.cache, report)
        self.assertIn("restore marker", report)
        self.assertIn("$ df -h", report)
        self.assertIn("::endgroup::", report)

    def test_probe_never_fails_the_job(self) -> None:
        stdout = io.StringIO()
        with redirect_stdout(stdout), mock.patch.object(
                ci_disk_cache, "probe_command", side_effect=RuntimeError("broken")), \
                mock.patch.object(ci_disk_cache, "probe_directory"):
            status = ci_disk_cache.main(["probe", "--stage", "before-save"])
        self.assertEqual(status, 0)
        self.assertIn("Runner resource probe failed", stdout.getvalue())

    def test_probe_walk_stops_at_its_deadline(self) -> None:
        self._entry("cas/aa/entry", 10, OLD)
        result = ci_disk_cache.walk_usage(str(self.cache), deadline=0)
        self.assertFalse(result[-1])


if __name__ == "__main__":
    unittest.main()
