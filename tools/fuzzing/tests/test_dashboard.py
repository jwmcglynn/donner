"""Tests for dashboard.py."""

import json
from pathlib import Path

import pytest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from dashboard import (
    format_duration,
    load_corpus_history,
    load_known_crashes,
    load_run_reports,
    print_coverage_trends,
    print_crash_history,
    print_health_summary,
    print_run_summary,
)


# ============================================================================
# format_duration
# ============================================================================

class TestFormatDuration:
    def test_seconds(self):
        assert format_duration(45) == "45s"

    def test_minutes(self):
        assert format_duration(125) == "2m5s"

    def test_hours(self):
        assert format_duration(3725) == "1h2m"

    def test_zero(self):
        assert format_duration(0) == "0s"

    def test_exact_minute(self):
        assert format_duration(60) == "1m0s"


# ============================================================================
# load_run_reports
# ============================================================================

class TestLoadRunReports:
    def test_loads_reports(self, tmp_path, monkeypatch):
        monkeypatch.setattr("dashboard.RUNS_DIR", tmp_path)

        for name in ["20260401-100000", "20260402-120000"]:
            d = tmp_path / name
            d.mkdir()
            (d / "run_report.json").write_text(json.dumps({
                "total_fuzzers": 16,
                "total_crashes": 0,
                "total_duration_secs": 900,
                "fuzzers": [],
            }))

        reports = load_run_reports(max_runs=10)
        assert len(reports) == 2
        assert reports[0]["_name"] == "20260402-120000"  # Most recent first

    def test_respects_max_runs(self, tmp_path, monkeypatch):
        monkeypatch.setattr("dashboard.RUNS_DIR", tmp_path)

        for i in range(5):
            d = tmp_path / f"2026040{i}-100000"
            d.mkdir()
            (d / "run_report.json").write_text(json.dumps({
                "total_fuzzers": 1, "fuzzers": [],
            }))

        reports = load_run_reports(max_runs=2)
        assert len(reports) == 2

    def test_empty_dir(self, tmp_path, monkeypatch):
        monkeypatch.setattr("dashboard.RUNS_DIR", tmp_path)
        assert load_run_reports() == []

    def test_missing_dir(self, tmp_path, monkeypatch):
        monkeypatch.setattr("dashboard.RUNS_DIR", tmp_path / "nonexistent")
        assert load_run_reports() == []

    def test_skips_dirs_without_report(self, tmp_path, monkeypatch):
        monkeypatch.setattr("dashboard.RUNS_DIR", tmp_path)
        (tmp_path / "20260401-100000").mkdir()  # No report
        d = tmp_path / "20260402-120000"
        d.mkdir()
        (d / "run_report.json").write_text('{"fuzzers": []}')

        reports = load_run_reports()
        assert len(reports) == 1


# ============================================================================
# load_corpus_history
# ============================================================================

class TestLoadCorpusHistory:
    def test_loads_entries(self, tmp_path, monkeypatch):
        stats_dir = tmp_path / "stats"
        stats_dir.mkdir()
        monkeypatch.setattr("dashboard.STATS_DIR", stats_dir)

        history_file = stats_dir / "corpus_history.jsonl"
        history_file.write_text(
            '{"timestamp": "2026-04-06T20:00:00Z", "total_after": 100}\n'
            '{"timestamp": "2026-04-07T01:00:00Z", "total_after": 200}\n'
        )

        entries = load_corpus_history()
        assert len(entries) == 2
        assert entries[0]["total_after"] == 100
        assert entries[1]["total_after"] == 200

    def test_missing_file(self, tmp_path, monkeypatch):
        monkeypatch.setattr("dashboard.STATS_DIR", tmp_path)
        assert load_corpus_history() == []


# ============================================================================
# load_known_crashes
# ============================================================================

class TestLoadKnownCrashes:
    def test_loads_crashes(self, tmp_path, monkeypatch):
        crashes_file = tmp_path / "known_crashes.json"
        crashes_file.write_text('{"sig123": {"fuzzer": "test"}}')
        monkeypatch.setattr("dashboard.KNOWN_CRASHES_FILE", crashes_file)

        crashes = load_known_crashes()
        assert "sig123" in crashes

    def test_missing_file(self, tmp_path, monkeypatch):
        monkeypatch.setattr("dashboard.KNOWN_CRASHES_FILE", tmp_path / "none.json")
        assert load_known_crashes() == {}


# ============================================================================
# Display functions (smoke tests — verify they don't crash)
# ============================================================================

class TestDisplayFunctions:
    SAMPLE_REPORTS = [
        {
            "_name": "20260407-013513",
            "_dir": "/tmp/run1",
            "total_duration_secs": 1023,
            "total_fuzzers": 16,
            "total_crashes": 0,
            "fuzzers": [
                {"name": "svg_parser_fuzzer", "total_execs": 43748,
                 "peak_coverage": 43033, "exit_reason": "completed"},
                {"name": "number_parser_fuzzer", "total_execs": 6473195,
                 "peak_coverage": 793, "exit_reason": "plateau (120s)"},
            ],
        },
        {
            "_name": "20260406-203045",
            "_dir": "/tmp/run2",
            "total_duration_secs": 901,
            "total_fuzzers": 16,
            "total_crashes": 1,
            "fuzzers": [
                {"name": "svg_parser_fuzzer", "total_execs": 47501,
                 "peak_coverage": 42293, "exit_reason": "deadline"},
                {"name": "number_parser_fuzzer", "total_execs": 6124294,
                 "peak_coverage": 793, "exit_reason": "plateau (121s)"},
            ],
        },
    ]

    def test_print_run_summary(self, capsys):
        print_run_summary(self.SAMPLE_REPORTS)
        out = capsys.readouterr().out
        assert "RECENT FUZZING RUNS" in out
        assert "20260407-013513" in out
        assert "20260406-203045" in out

    def test_print_run_summary_empty(self, capsys):
        print_run_summary([])
        assert "No fuzzing runs" in capsys.readouterr().out

    def test_print_coverage_trends(self, capsys):
        print_coverage_trends(self.SAMPLE_REPORTS)
        out = capsys.readouterr().out
        assert "COVERAGE TRENDS" in out
        assert "svg_parser_fuzzer" in out
        assert "43,033" in out

    def test_print_crash_history_empty(self, capsys):
        print_crash_history({})
        assert "No known crashes" in capsys.readouterr().out

    def test_print_crash_history_with_data(self, capsys):
        crashes = {
            "abc123": {
                "fuzzer": "test_fuzzer",
                "crash_type": "crash",
                "date": "2026-04-07T00:00:00Z",
                "issue_url": "https://github.com/test/1",
            }
        }
        print_crash_history(crashes)
        out = capsys.readouterr().out
        assert "KNOWN CRASHES" in out
        assert "test_fuzzer" in out

    def test_print_health_summary(self, capsys):
        print_health_summary(self.SAMPLE_REPORTS, {})
        out = capsys.readouterr().out
        assert "HEALTH CHECK" in out
        assert "14,684,260" not in out  # That's from real data, not sample
        assert "Known crashes" in out

    def test_print_health_summary_empty(self, capsys):
        print_health_summary([], {})
        # Should not crash
        assert capsys.readouterr().out == ""
