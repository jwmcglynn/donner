"""Tests for web_dashboard.py."""

import json
import os
import threading
import time
import urllib.request
from pathlib import Path
from unittest import mock

import pytest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from web_dashboard import (
    DashboardHandler,
    get_trigger_status,
    get_latest_trigger_log,
    render_page,
    render_health,
    render_runs_table,
    render_coverage_trends,
    render_corpus_history,
    render_crashes,
    render_log,
    render_status_badge,
)


# ============================================================================
# render_status_badge
# ============================================================================

class TestRenderStatusBadge:
    def test_running(self):
        badge = render_status_badge(True)
        assert "running" in badge.lower()
        assert "FUZZING" in badge

    def test_idle(self):
        badge = render_status_badge(False)
        assert "idle" in badge.lower()
        assert "IDLE" in badge


# ============================================================================
# get_trigger_status
# ============================================================================

class TestGetTriggerStatus:
    def test_no_state_files(self, tmp_path, monkeypatch):
        monkeypatch.setattr("web_dashboard.STATE_DIR", tmp_path)
        status = get_trigger_status()
        assert status["last_run_time"] is None
        assert status["last_run_commit"] is None
        assert status["is_running"] is False

    def test_with_timestamp(self, tmp_path, monkeypatch):
        monkeypatch.setattr("web_dashboard.STATE_DIR", tmp_path)
        (tmp_path / "last_run_timestamp").write_text("1775520000")
        status = get_trigger_status()
        assert status["last_run_time"] is not None
        assert "UTC" in status["last_run_time"]

    def test_with_commit(self, tmp_path, monkeypatch):
        monkeypatch.setattr("web_dashboard.STATE_DIR", tmp_path)
        (tmp_path / "last_run_commit").write_text("abcdef1234567890")
        status = get_trigger_status()
        assert status["last_run_commit"] == "abcdef123456"

    def test_stale_lock(self, tmp_path, monkeypatch):
        monkeypatch.setattr("web_dashboard.STATE_DIR", tmp_path)
        (tmp_path / "trigger.lock").write_text("999999999")
        status = get_trigger_status()
        assert status["lock_pid"] == 999999999
        assert status["is_running"] is False

    def test_active_lock(self, tmp_path, monkeypatch):
        monkeypatch.setattr("web_dashboard.STATE_DIR", tmp_path)
        (tmp_path / "trigger.lock").write_text(str(os.getpid()))
        status = get_trigger_status()
        assert status["is_running"] is True


# ============================================================================
# get_latest_trigger_log
# ============================================================================

class TestGetLatestTriggerLog:
    def test_reads_latest_log(self, tmp_path, monkeypatch):
        monkeypatch.setattr("web_dashboard.TRIGGER_LOGS_DIR", tmp_path)
        (tmp_path / "20260406-100000.log").write_text("old log")
        (tmp_path / "20260407-120000.log").write_text("new log\nline 2")

        log = get_latest_trigger_log()
        assert "new log" in log

    def test_no_logs(self, tmp_path, monkeypatch):
        monkeypatch.setattr("web_dashboard.TRIGGER_LOGS_DIR", tmp_path)
        assert "no trigger logs" in get_latest_trigger_log()

    def test_missing_dir(self, tmp_path, monkeypatch):
        monkeypatch.setattr("web_dashboard.TRIGGER_LOGS_DIR", tmp_path / "nope")
        assert "no trigger logs" in get_latest_trigger_log()

    def test_respects_max_lines(self, tmp_path, monkeypatch):
        monkeypatch.setattr("web_dashboard.TRIGGER_LOGS_DIR", tmp_path)
        lines = [f"line {i}" for i in range(100)]
        (tmp_path / "test.log").write_text("\n".join(lines))

        log = get_latest_trigger_log(max_lines=5)
        assert log.count("\n") == 4  # 5 lines = 4 newlines


# ============================================================================
# HTML render functions
# ============================================================================

SAMPLE_REPORTS = [
    {
        "_name": "20260407-013513",
        "_dir": "/tmp/run1",
        "total_duration_secs": 1023,
        "total_fuzzers": 16,
        "total_crashes": 0,
        "fuzzers": [
            {"name": "svg_parser_fuzzer", "total_execs": 43748,
             "peak_coverage": 43033, "exit_reason": "deadline"},
            {"name": "number_parser_fuzzer", "total_execs": 6473195,
             "peak_coverage": 793, "exit_reason": "plateau (120s)"},
        ],
    },
]

SAMPLE_TRIGGER = {
    "last_run_time": "2026-04-07 01:35:13 UTC",
    "last_run_commit": "0ecb26ecbecb",
    "is_running": False,
    "lock_pid": None,
}


class TestRenderHealth:
    def test_with_data(self):
        html = render_health(SAMPLE_REPORTS, {}, SAMPLE_TRIGGER)
        assert "Status" in html
        assert "16" in html  # fuzzers count
        assert "0ecb26ecbecb" in html

    def test_no_reports(self):
        html = render_health([], {}, SAMPLE_TRIGGER)
        assert "No runs yet" in html

    def test_with_crashes(self):
        crashes = {"sig1": {}, "sig2": {}}
        html = render_health(SAMPLE_REPORTS, crashes, SAMPLE_TRIGGER)
        assert "2" in html  # crash count


class TestRenderRunsTable:
    def test_renders_rows(self):
        html = render_runs_table(SAMPLE_REPORTS)
        assert "20260407-013513" in html
        assert "Recent Runs" in html

    def test_empty(self):
        assert render_runs_table([]) == ""


class TestRenderCoverageTrends:
    def test_renders_fuzzer_names(self):
        html = render_coverage_trends(SAMPLE_REPORTS)
        assert "svg_parser_fuzzer" in html
        assert "number_parser_fuzzer" in html
        assert "43,033" in html

    def test_empty(self):
        assert render_coverage_trends([]) == ""


class TestRenderCorpusHistory:
    def test_renders_entries(self):
        history = [
            {"timestamp": "2026-04-06T20:00:00Z", "total_after": 9259},
            {"timestamp": "2026-04-07T01:58:40Z", "total_after": 10451},
        ]
        html = render_corpus_history(history)
        assert "9,259" in html
        assert "10,451" in html

    def test_empty(self):
        assert render_corpus_history([]) == ""


class TestRenderCrashes:
    def test_no_crashes(self):
        html = render_crashes({})
        assert "No crashes" in html

    def test_with_crashes(self):
        crashes = {
            "abc123": {
                "fuzzer": "svg_parser_fuzzer",
                "crash_type": "crash",
                "top_frame": "donner::Parser::parse()",
                "date": "2026-04-07T00:00:00Z",
                "issue_url": "https://github.com/test/issues/1",
            }
        }
        html = render_crashes(crashes)
        assert "svg_parser_fuzzer" in html
        assert "abc123" in html
        assert "https://github.com/test/issues/1" in html


class TestRenderLog:
    def test_escapes_html(self):
        html = render_log("<script>alert('xss')</script>")
        assert "<script>" not in html
        assert "&lt;script&gt;" in html


# ============================================================================
# Full page render
# ============================================================================

class TestRenderPage:
    def test_produces_valid_html(self, tmp_path, monkeypatch):
        monkeypatch.setattr("dashboard.RUNS_DIR", tmp_path / "runs")
        monkeypatch.setattr("dashboard.STATS_DIR", tmp_path / "stats")
        monkeypatch.setattr("dashboard.KNOWN_CRASHES_FILE", tmp_path / "crashes.json")
        monkeypatch.setattr("web_dashboard.STATE_DIR", tmp_path)
        monkeypatch.setattr("web_dashboard.TRIGGER_LOGS_DIR", tmp_path / "logs")

        html = render_page()
        assert "<!DOCTYPE html>" in html
        assert "Donner Fuzzing Dashboard" in html
        assert "</html>" in html

    def test_auto_refresh(self, tmp_path, monkeypatch):
        monkeypatch.setattr("dashboard.RUNS_DIR", tmp_path / "runs")
        monkeypatch.setattr("dashboard.STATS_DIR", tmp_path / "stats")
        monkeypatch.setattr("dashboard.KNOWN_CRASHES_FILE", tmp_path / "crashes.json")
        monkeypatch.setattr("web_dashboard.STATE_DIR", tmp_path)
        monkeypatch.setattr("web_dashboard.TRIGGER_LOGS_DIR", tmp_path / "logs")

        html = render_page()
        assert 'http-equiv="refresh"' in html


# ============================================================================
# HTTP handler (integration)
# ============================================================================

class TestHTTPHandler:
    @pytest.fixture
    def server(self, tmp_path, monkeypatch):
        """Start a test server on a random port."""
        from http.server import HTTPServer

        monkeypatch.setattr("dashboard.RUNS_DIR", tmp_path / "runs")
        monkeypatch.setattr("dashboard.STATS_DIR", tmp_path / "stats")
        monkeypatch.setattr("dashboard.KNOWN_CRASHES_FILE", tmp_path / "crashes.json")
        monkeypatch.setattr("web_dashboard.STATE_DIR", tmp_path)
        monkeypatch.setattr("web_dashboard.TRIGGER_LOGS_DIR", tmp_path / "logs")

        httpd = HTTPServer(("127.0.0.1", 0), DashboardHandler)
        port = httpd.server_address[1]
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()
        yield f"http://127.0.0.1:{port}"
        httpd.shutdown()

    def test_index_returns_html(self, server):
        resp = urllib.request.urlopen(f"{server}/")
        assert resp.status == 200
        assert "text/html" in resp.headers["Content-Type"]
        body = resp.read().decode()
        assert "Donner Fuzzing Dashboard" in body

    def test_api_returns_json(self, server):
        resp = urllib.request.urlopen(f"{server}/api/status")
        assert resp.status == 200
        assert "application/json" in resp.headers["Content-Type"]
        data = json.loads(resp.read())
        assert "trigger" in data
        assert "runs" in data
        assert "known_crashes" in data

    def test_404(self, server):
        try:
            urllib.request.urlopen(f"{server}/nonexistent")
            assert False, "Should have raised"
        except urllib.error.HTTPError as e:
            assert e.code == 404
