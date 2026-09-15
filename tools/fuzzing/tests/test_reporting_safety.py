"""Publication boundaries for unattended crash reporting."""

import hashlib
import json
import sys
from pathlib import Path
from unittest import mock

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import crash_reporter as reporter
from run_continuous_fuzz import FuzzerTarget


@pytest.fixture
def crash_run(tmp_path, monkeypatch):
    run = tmp_path / "run"
    artifact = run / "parser_fuzzer" / "crashes" / "crash-1234"
    artifact.parent.mkdir(parents=True)
    artifact.write_bytes(b"<svg/>")
    binary = tmp_path / "fuzzer"
    binary.write_text("#!/bin/sh\necho 'normal execution' >&2\nexit 0\n")
    binary.chmod(0o755)
    target = FuzzerTarget(label="//donner/test:parser_fuzzer_bin", name="parser_fuzzer", binary_path=binary)
    monkeypatch.setattr(reporter, "KNOWN_CRASHES_FILE", tmp_path / "ledger.json")
    monkeypatch.setattr(reporter, "CONFIG_FILE", tmp_path / "config.json")
    monkeypatch.setattr(reporter, "get_current_commit", lambda: "a" * 40)
    (run / "run_report.json").write_text(json.dumps({
        "commit": "a" * 40,
        "fuzzers": [{"name": target.name, "label": target.label,
                     "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}],
    }))
    return run, artifact, target


def test_successful_replay_never_files_bug(crash_run):
    run, _, target = crash_run
    with mock.patch.object(reporter, "file_github_issue", return_value="https://github.com/jwmcglynn/donner/issues/1") as publish:
        reporter.process_crashes(run, [target])
    publish.assert_not_called()


def test_old_revision_is_not_reported_as_current(crash_run):
    run, _, target = crash_run
    report = json.loads((run / "run_report.json").read_text())
    report["commit"] = "b" * 40
    (run / "run_report.json").write_text(json.dumps(report))
    with mock.patch.object(reporter, "file_github_issue", return_value="https://github.com/jwmcglynn/donner/issues/1") as publish:
        reporter.process_crashes(run, [target])
    publish.assert_not_called()


def test_symlink_crash_artifact_is_not_read(crash_run, tmp_path):
    run, artifact, target = crash_run
    artifact.unlink()
    private = tmp_path / "private"
    private.write_text("must not publish")
    artifact.symlink_to(private)
    assert reporter.find_crashes_in_run(run) == []


def test_nonzero_tool_failure_is_not_a_crash(crash_run):
    run, _, target = crash_run
    target.binary_path.write_text("#!/bin/sh\necho 'shared library missing' >&2\nexit 127\n")
    with mock.patch.object(reporter, "file_github_issue", return_value="https://github.com/jwmcglynn/donner/issues/1") as publish:
        reporter.process_crashes(run, [target])
    publish.assert_not_called()


def test_missing_reproducer_is_not_publishable():
    with mock.patch.object(reporter.subprocess, "run") as command:
        assert reporter.file_github_issue(reporter.CrashInfo()) is None
    command.assert_not_called()
