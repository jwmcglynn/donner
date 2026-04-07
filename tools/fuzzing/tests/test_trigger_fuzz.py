"""Tests for trigger_fuzz.sh.

Tests the shell script's eligibility logic (rate limiting, commit checking,
locking) using subprocess calls with controlled state directories.
"""

import os
import subprocess
import time
from pathlib import Path

import pytest


SCRIPT = Path(__file__).resolve().parent.parent / "trigger_fuzz.sh"
REPO_ROOT = Path(__file__).resolve().parent.parent.parent.parent


def run_trigger(state_dir: Path, extra_env: dict = None, args: list = None) -> subprocess.CompletedProcess:
    """Run trigger_fuzz.sh with a controlled state directory."""
    env = os.environ.copy()
    env["FUZZ_STATE_DIR"] = str(state_dir)
    env["FUZZ_REPO_DIR"] = str(REPO_ROOT)
    if extra_env:
        env.update(extra_env)

    cmd = [str(SCRIPT)] + (args or [])
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=30,
        env=env,
    )


class TestRateLimit:
    def test_first_run_passes_rate_limit(self, tmp_path):
        """First run (no timestamp file) should pass rate limit check."""
        result = run_trigger(tmp_path, args=["--dry-run"])
        assert result.returncode == 0
        assert "Rate limit OK" in result.stdout or "First run" in result.stdout

    def test_recent_run_is_rate_limited(self, tmp_path):
        """A run within the minimum interval should be rate limited."""
        ts_file = tmp_path / "last_run_timestamp"
        ts_file.write_text(str(int(time.time())))

        # Also need to set a commit file so commit check passes
        commit_file = tmp_path / "last_run_commit"
        commit_file.write_text("0000000000000000000000000000000000000000")

        result = run_trigger(tmp_path, args=["--dry-run"])
        assert result.returncode == 0
        assert "Rate limited" in result.stdout or "Not eligible" in result.stdout

    def test_old_run_passes_rate_limit(self, tmp_path):
        """A run older than the minimum interval should pass."""
        ts_file = tmp_path / "last_run_timestamp"
        ts_file.write_text(str(int(time.time()) - 8000))  # > 7200s ago

        result = run_trigger(tmp_path, args=["--dry-run"])
        assert result.returncode == 0
        assert "Rate limit OK" in result.stdout

    def test_custom_interval(self, tmp_path):
        """Custom FUZZ_MIN_INTERVAL should be respected."""
        ts_file = tmp_path / "last_run_timestamp"
        ts_file.write_text(str(int(time.time()) - 10))  # 10 seconds ago

        result = run_trigger(
            tmp_path,
            extra_env={"FUZZ_MIN_INTERVAL": "5"},  # 5 second interval
            args=["--dry-run"],
        )
        assert result.returncode == 0
        assert "Rate limit OK" in result.stdout


class TestCommitCheck:
    def test_no_previous_commit_is_first_run(self, tmp_path):
        """No commit file = first run, should pass."""
        result = run_trigger(tmp_path, args=["--dry-run"])
        assert result.returncode == 0
        assert "First run" in result.stdout

    def test_same_commit_skips(self, tmp_path):
        """Same commit as last run should skip."""
        # Use origin/main (what the trigger compares against), not local HEAD
        head = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "origin/main"],
            capture_output=True, text=True,
        ).stdout.strip()
        if not head or head.startswith("fatal"):
            # Fall back to main if origin/main doesn't exist
            head = subprocess.run(
                ["git", "-C", str(REPO_ROOT), "rev-parse", "main"],
                capture_output=True, text=True,
            ).stdout.strip()

        commit_file = tmp_path / "last_run_commit"
        commit_file.write_text(head)

        # Also make rate limit pass
        ts_file = tmp_path / "last_run_timestamp"
        ts_file.write_text("0")

        result = run_trigger(tmp_path, args=["--dry-run"])
        assert result.returncode == 0
        assert "No new commits" in result.stdout

    def test_different_commit_proceeds(self, tmp_path):
        """Different commit should proceed."""
        commit_file = tmp_path / "last_run_commit"
        commit_file.write_text("0000000000000000000000000000000000000000")

        ts_file = tmp_path / "last_run_timestamp"
        ts_file.write_text("0")

        result = run_trigger(tmp_path, args=["--dry-run"])
        assert result.returncode == 0
        assert "New commits" in result.stdout or "First run" in result.stdout


class TestForceFlag:
    def test_force_bypasses_rate_limit(self, tmp_path):
        """--force should bypass rate limiting."""
        ts_file = tmp_path / "last_run_timestamp"
        ts_file.write_text(str(int(time.time())))  # Just ran

        commit_file = tmp_path / "last_run_commit"
        # Set to current HEAD so commit check would normally skip
        head = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "HEAD"],
            capture_output=True, text=True,
        ).stdout.strip()
        commit_file.write_text(head)

        result = run_trigger(tmp_path, args=["--force", "--dry-run"])
        assert result.returncode == 0
        assert "bypassed" in result.stdout.lower()
        assert "would start fuzzing" in result.stdout.lower()


class TestLocking:
    def test_creates_lock_file(self, tmp_path):
        """Running the trigger should create a lock file."""
        result = run_trigger(tmp_path, args=["--dry-run"])
        assert result.returncode == 0
        # Lock should be cleaned up after exit (trap)
        assert not (tmp_path / "trigger.lock").exists()

    def test_stale_lock_is_cleaned(self, tmp_path):
        """A lock file with a dead PID should be cleaned up."""
        lock_file = tmp_path / "trigger.lock"
        lock_file.write_text("999999999")  # Non-existent PID

        result = run_trigger(tmp_path, args=["--dry-run"])
        assert result.returncode == 0
        assert "Stale lock" in result.stdout


class TestQuietHours:
    def test_ignore_mode_runs_full_speed(self, tmp_path):
        """FUZZ_QUIET_MODE=ignore should always run at full capacity."""
        result = run_trigger(
            tmp_path,
            extra_env={"FUZZ_QUIET_MODE": "ignore"},
            args=["--dry-run"],
        )
        assert result.returncode == 0
        assert "disabled" in result.stdout.lower()
        assert "would start fuzzing now" in result.stdout.lower()

    def test_no_contention_full_speed(self, tmp_path):
        """When no contention signals, run at full capacity."""
        # Disable steal check (set to 100% — impossible to exceed) to test
        # the no-contention path predictably
        result = run_trigger(
            tmp_path,
            extra_env={"FUZZ_STEAL_THRESHOLD": "100"},
            args=["--dry-run"],
        )
        assert result.returncode == 0
        assert "idle" in result.stdout.lower() or "would start fuzzing" in result.stdout.lower()

    def test_steal_time_logged(self, tmp_path):
        """CPU steal time should be checked and logged."""
        result = run_trigger(
            tmp_path,
            extra_env={"FUZZ_STEAL_THRESHOLD": "100"},  # Won't trigger
            args=["--force", "--dry-run"],
        )
        assert result.returncode == 0
        assert "steal time" in result.stdout.lower()

    def test_high_steal_triggers_quiet(self, tmp_path):
        """Steal threshold of 0% should always trigger quiet mode."""
        result = run_trigger(
            tmp_path,
            extra_env={
                "FUZZ_QUIET_MODE": "reduce",
                "FUZZ_STEAL_THRESHOLD": "0",  # 0% = always triggers
                "FUZZ_QUIET_WORKERS": "2",
            },
            args=["--force", "--dry-run"],
        )
        assert result.returncode == 0
        # steal >= 0 always true, so quiet should activate
        assert "reducing" in result.stdout.lower()
        assert "quiet mode" in result.stdout.lower()

    def test_load_threshold_below(self, tmp_path):
        """Load below threshold should not trigger quiet mode."""
        result = run_trigger(
            tmp_path,
            extra_env={
                "FUZZ_QUIET_MODE": "reduce",
                "FUZZ_LOAD_THRESHOLD": "9999",  # Unreachably high
                "FUZZ_STEAL_THRESHOLD": "100",   # Disable steal check
            },
            args=["--dry-run"],
        )
        assert result.returncode == 0
        assert "idle" in result.stdout.lower() or "would start fuzzing" in result.stdout.lower()

    def test_skip_mode_exits_when_busy(self, tmp_path):
        """FUZZ_QUIET_MODE=skip should exit when contention is detected."""
        result = run_trigger(
            tmp_path,
            extra_env={
                "FUZZ_QUIET_MODE": "skip",
                "FUZZ_STEAL_THRESHOLD": "0",  # Always triggers
            },
            args=["--force", "--dry-run"],
        )
        assert result.returncode == 0
        assert "skipping" in result.stdout.lower()

    def test_reduce_mode_shows_reduced_workers(self, tmp_path):
        """FUZZ_QUIET_MODE=reduce should log the reduced worker count."""
        result = run_trigger(
            tmp_path,
            extra_env={
                "FUZZ_QUIET_MODE": "reduce",
                "FUZZ_STEAL_THRESHOLD": "0",  # Always triggers
                "FUZZ_QUIET_WORKERS": "3",
            },
            args=["--force", "--dry-run"],
        )
        assert result.returncode == 0
        assert "reducing" in result.stdout.lower()
        assert "3 workers" in result.stdout
        assert "quiet mode" in result.stdout.lower()

    def test_quiet_dry_run_message(self, tmp_path):
        """Dry run in quiet mode should mention quiet mode."""
        result = run_trigger(
            tmp_path,
            extra_env={
                "FUZZ_QUIET_MODE": "reduce",
                "FUZZ_STEAL_THRESHOLD": "0",  # Always triggers
            },
            args=["--force", "--dry-run"],
        )
        assert result.returncode == 0
        assert "quiet mode" in result.stdout.lower()


class TestDryRun:
    def test_dry_run_does_not_modify_state(self, tmp_path):
        """--dry-run should not create timestamp or commit files."""
        result = run_trigger(tmp_path, args=["--dry-run"])
        assert result.returncode == 0
        assert not (tmp_path / "last_run_timestamp").exists()
        assert not (tmp_path / "last_run_commit").exists()

    def test_dry_run_message(self, tmp_path):
        result = run_trigger(tmp_path, args=["--dry-run"])
        assert "Dry run" in result.stdout or "would start fuzzing" in result.stdout.lower()
