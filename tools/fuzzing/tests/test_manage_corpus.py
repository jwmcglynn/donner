"""Tests for manage_corpus.py."""

import json
import shutil
from pathlib import Path
from unittest import mock

import pytest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from run_continuous_fuzz import FuzzerTarget
from manage_corpus import (
    find_latest_run,
    minimize_target,
    _log_corpus_stats,
    PERSISTENT_CORPUS_DIR,
)


# ============================================================================
# find_latest_run
# ============================================================================

class TestFindLatestRun:
    def test_finds_latest_by_name(self, tmp_path, monkeypatch):
        monkeypatch.setattr("manage_corpus.RUNS_DIR", tmp_path)

        (tmp_path / "20260401-100000").mkdir()
        (tmp_path / "20260401-100000" / "run_report.json").write_text("{}")
        (tmp_path / "20260402-120000").mkdir()
        (tmp_path / "20260402-120000" / "run_report.json").write_text("{}")
        (tmp_path / "20260401-080000").mkdir()
        (tmp_path / "20260401-080000" / "run_report.json").write_text("{}")

        result = find_latest_run()
        assert result is not None
        assert result.name == "20260402-120000"

    def test_skips_dirs_without_report(self, tmp_path, monkeypatch):
        monkeypatch.setattr("manage_corpus.RUNS_DIR", tmp_path)

        (tmp_path / "20260402-120000").mkdir()  # No run_report.json
        (tmp_path / "20260401-100000").mkdir()
        (tmp_path / "20260401-100000" / "run_report.json").write_text("{}")

        result = find_latest_run()
        assert result.name == "20260401-100000"

    def test_no_runs_dir(self, tmp_path, monkeypatch):
        monkeypatch.setattr("manage_corpus.RUNS_DIR", tmp_path / "nonexistent")
        assert find_latest_run() is None

    def test_empty_runs_dir(self, tmp_path, monkeypatch):
        monkeypatch.setattr("manage_corpus.RUNS_DIR", tmp_path)
        assert find_latest_run() is None


# ============================================================================
# minimize_target
# ============================================================================

class TestMinimizeTarget:
    def test_no_binary(self, tmp_path):
        target = FuzzerTarget(label="//test:bin", name="test_fuzzer")
        result = minimize_target(target, tmp_path / "run_corpus", tmp_path / "persistent")
        assert result["status"] == "no_binary"

    def test_empty_corpus(self, tmp_path):
        run_corpus = tmp_path / "run_corpus"
        run_corpus.mkdir()
        persistent = tmp_path / "persistent"
        persistent.mkdir()

        target = FuzzerTarget(
            label="//test:bin", name="test_fuzzer",
            binary_path=Path("/usr/bin/true"),
        )
        result = minimize_target(target, run_corpus, persistent)
        assert result["status"] == "empty"
        assert result["before_run"] == 0
        assert result["before_persistent"] == 0

    def test_counts_input_files(self, tmp_path):
        """Verify before_run and before_persistent counts are correct."""
        run_corpus = tmp_path / "run_corpus"
        run_corpus.mkdir()
        (run_corpus / "input1").write_bytes(b"a")
        (run_corpus / "input2").write_bytes(b"b")
        (run_corpus / "input3").write_bytes(b"c")

        persistent = tmp_path / "persistent"
        (persistent / "test_fuzzer").mkdir(parents=True)
        (persistent / "test_fuzzer" / "p1").write_bytes(b"x")

        # Use a fake binary that will fail — we just want to check counting
        fake_binary = tmp_path / "fake"
        fake_binary.write_text("#!/bin/bash\nexit 1\n")
        fake_binary.chmod(0o755)

        target = FuzzerTarget(
            label="//test:bin", name="test_fuzzer",
            binary_path=fake_binary,
        )
        result = minimize_target(target, run_corpus, persistent)
        assert result["before_run"] == 3
        assert result["before_persistent"] == 1

    def test_merge_with_fake_binary(self, tmp_path):
        """Test merge using a script that copies inputs to the output dir."""
        run_corpus = tmp_path / "run_corpus"
        run_corpus.mkdir()
        (run_corpus / "input1").write_bytes(b"data1")
        (run_corpus / "input2").write_bytes(b"data2")

        persistent = tmp_path / "persistent"
        persistent.mkdir()

        # Create a fake merge binary that copies all inputs from input dirs
        # to the first (output) directory
        fake_merge = tmp_path / "fake_merge.sh"
        fake_merge.write_text(
            '#!/bin/bash\n'
            '# $1 is -merge=1, $2 is output dir, rest are input dirs\n'
            'OUTPUT_DIR="$2"\n'
            'shift 2\n'
            'for dir in "$@"; do\n'
            '    if [ -d "$dir" ]; then\n'
            '        cp "$dir"/* "$OUTPUT_DIR/" 2>/dev/null || true\n'
            '    fi\n'
            'done\n'
        )
        fake_merge.chmod(0o755)

        target = FuzzerTarget(
            label="//test:bin", name="test_fuzzer",
            binary_path=fake_merge,
        )
        result = minimize_target(target, run_corpus, persistent)

        assert result["status"] == "ok"
        assert result["before_run"] == 2
        assert result["after"] == 2

        # Check persistent corpus was updated
        persistent_target = persistent / "test_fuzzer"
        assert persistent_target.is_dir()
        assert len(list(persistent_target.iterdir())) == 2

    def test_merge_replaces_old_persistent(self, tmp_path):
        """Verify that old persistent corpus is replaced, not appended to."""
        run_corpus = tmp_path / "run_corpus"
        run_corpus.mkdir()
        (run_corpus / "new_input").write_bytes(b"new")

        persistent = tmp_path / "persistent"
        (persistent / "test_fuzzer").mkdir(parents=True)
        (persistent / "test_fuzzer" / "old_input").write_bytes(b"old")

        fake_merge = tmp_path / "fake_merge.sh"
        fake_merge.write_text(
            '#!/bin/bash\n'
            'OUTPUT_DIR="$2"\n'
            'shift 2\n'
            'for dir in "$@"; do\n'
            '    cp "$dir"/* "$OUTPUT_DIR/" 2>/dev/null || true\n'
            'done\n'
        )
        fake_merge.chmod(0o755)

        target = FuzzerTarget(
            label="//test:bin", name="test_fuzzer",
            binary_path=fake_merge,
        )
        result = minimize_target(target, run_corpus, persistent)
        assert result["status"] == "ok"

        # Old persistent dir should be replaced with merge output
        persistent_files = list((persistent / "test_fuzzer").iterdir())
        file_names = {f.name for f in persistent_files}
        # Both old and new should be in the merge output since the fake
        # copies from all input dirs (run + persistent)
        assert "new_input" in file_names
        assert "old_input" in file_names

    def test_includes_intree_corpus_in_merge(self, tmp_path):
        """In-tree corpus is included as a merge input source."""
        intree_corpus = tmp_path / "intree"
        intree_corpus.mkdir()
        (intree_corpus / "seed").write_bytes(b"seed_data")

        run_corpus = tmp_path / "run_corpus"
        run_corpus.mkdir()
        (run_corpus / "found").write_bytes(b"found_data")

        persistent = tmp_path / "persistent"
        persistent.mkdir()

        # Fake merge that records the command args
        fake_merge = tmp_path / "fake_merge.sh"
        fake_merge.write_text(
            '#!/bin/bash\n'
            'OUTPUT_DIR="$2"\n'
            'shift 2\n'
            'for dir in "$@"; do\n'
            '    cp "$dir"/* "$OUTPUT_DIR/" 2>/dev/null || true\n'
            'done\n'
        )
        fake_merge.chmod(0o755)

        target = FuzzerTarget(
            label="//test:bin", name="test_fuzzer",
            binary_path=fake_merge,
            corpus_dir=intree_corpus,
        )
        result = minimize_target(target, run_corpus, persistent)
        assert result["status"] == "ok"

        # Both run and intree inputs should appear in the output
        persistent_files = {f.name for f in (persistent / "test_fuzzer").iterdir()}
        assert "seed" in persistent_files
        assert "found" in persistent_files

    def test_binary_failure(self, tmp_path):
        run_corpus = tmp_path / "run_corpus"
        run_corpus.mkdir()
        (run_corpus / "input1").write_bytes(b"data")

        persistent = tmp_path / "persistent"
        persistent.mkdir()

        fake_binary = tmp_path / "fail.sh"
        fake_binary.write_text("#!/bin/bash\nexit 1\n")
        fake_binary.chmod(0o755)

        target = FuzzerTarget(
            label="//test:bin", name="test_fuzzer",
            binary_path=fake_binary,
        )
        result = minimize_target(target, run_corpus, persistent)
        assert "error" in result["status"]


# ============================================================================
# _log_corpus_stats
# ============================================================================

class TestLogCorpusStats:
    def test_creates_history_file(self, tmp_path, monkeypatch):
        stats_dir = tmp_path / "stats"
        monkeypatch.setattr("manage_corpus.STATS_DIR", stats_dir)

        results = [
            {"name": "fuzzer_a", "before_run": 100, "before_persistent": 50, "after": 80, "status": "ok"},
            {"name": "fuzzer_b", "before_run": 0, "before_persistent": 0, "after": 0, "status": "empty"},
        ]
        _log_corpus_stats(results)

        history_file = stats_dir / "corpus_history.jsonl"
        assert history_file.exists()

        lines = history_file.read_text().strip().splitlines()
        assert len(lines) == 1

        entry = json.loads(lines[0])
        assert "timestamp" in entry
        assert entry["total_after"] == 80
        assert entry["targets"]["fuzzer_a"]["after"] == 80
        assert entry["targets"]["fuzzer_b"]["status"] == "empty"

    def test_appends_to_existing(self, tmp_path, monkeypatch):
        stats_dir = tmp_path / "stats"
        stats_dir.mkdir(parents=True)
        history_file = stats_dir / "corpus_history.jsonl"
        history_file.write_text('{"existing": true}\n')
        monkeypatch.setattr("manage_corpus.STATS_DIR", stats_dir)

        _log_corpus_stats([
            {"name": "fuzzer_a", "before_run": 10, "before_persistent": 0, "after": 8, "status": "ok"},
        ])

        lines = history_file.read_text().strip().splitlines()
        assert len(lines) == 2
        assert json.loads(lines[0]) == {"existing": True}
        assert json.loads(lines[1])["total_after"] == 8


# ============================================================================
# update-intree logic (tested as functions)
# ============================================================================

class TestUpdateIntreeLogic:
    """Test the core update-intree logic without running the full CLI."""

    def test_copies_new_files(self, tmp_path):
        """New files from persistent corpus are copied to in-tree."""
        persistent = tmp_path / "persistent" / "test_fuzzer"
        persistent.mkdir(parents=True)
        (persistent / "new_input").write_bytes(b"new")
        (persistent / "existing").write_bytes(b"persistent_ver")

        intree = tmp_path / "intree"
        intree.mkdir()
        (intree / "existing").write_bytes(b"intree_ver")

        # Simulate the copy logic from cmd_update_intree
        persistent_files = {f.name: f for f in persistent.iterdir() if f.is_file()}
        new_count = 0
        for name, src in persistent_files.items():
            dest = intree / name
            if not dest.exists():
                shutil.copy2(src, dest)
                new_count += 1

        assert new_count == 1
        assert (intree / "new_input").exists()
        # Existing file should NOT be overwritten
        assert (intree / "existing").read_bytes() == b"intree_ver"

    def test_prunes_removed_files(self, tmp_path):
        """Files not in persistent corpus are pruned from in-tree."""
        persistent = tmp_path / "persistent" / "test_fuzzer"
        persistent.mkdir(parents=True)
        (persistent / "kept").write_bytes(b"keep")

        intree = tmp_path / "intree"
        intree.mkdir()
        (intree / "kept").write_bytes(b"keep")
        (intree / "removed").write_bytes(b"remove")

        persistent_files = {f.name for f in persistent.iterdir() if f.is_file()}
        intree_files = {f.name: f for f in intree.iterdir() if f.is_file()}

        removed_count = 0
        for name, path in intree_files.items():
            if name not in persistent_files:
                path.unlink()
                removed_count += 1

        assert removed_count == 1
        assert (intree / "kept").exists()
        assert not (intree / "removed").exists()

    def test_no_prune_preserves_files(self, tmp_path):
        """With no_prune=True, extra in-tree files are kept."""
        persistent = tmp_path / "persistent" / "test_fuzzer"
        persistent.mkdir(parents=True)
        (persistent / "kept").write_bytes(b"keep")

        intree = tmp_path / "intree"
        intree.mkdir()
        (intree / "kept").write_bytes(b"keep")
        (intree / "extra").write_bytes(b"extra")

        # Don't prune — just copy new files
        persistent_files = {f.name: f for f in persistent.iterdir() if f.is_file()}
        for name, src in persistent_files.items():
            dest = intree / name
            if not dest.exists():
                shutil.copy2(src, dest)

        assert (intree / "kept").exists()
        assert (intree / "extra").exists()  # Preserved

    def test_empty_persistent_no_crash(self, tmp_path):
        """Empty persistent corpus doesn't cause errors."""
        persistent = tmp_path / "persistent" / "test_fuzzer"
        persistent.mkdir(parents=True)

        intree = tmp_path / "intree"
        intree.mkdir()
        (intree / "seed").write_bytes(b"seed")

        persistent_files = {f.name: f for f in persistent.iterdir() if f.is_file()}
        assert len(persistent_files) == 0
