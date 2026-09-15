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


@pytest.fixture
def verified_crash(crash_run):
    run, artifact, target = crash_run
    crash = reporter.CrashInfo()
    crash.fuzzer_name = target.name
    crash.fuzzer_label = target.label
    crash.crash_file = artifact
    crash.binary_path = target.binary_path
    crash.binary_sha256 = hashlib.sha256(target.binary_path.read_bytes()).hexdigest()
    crash.commit = "a" * 40
    crash.signature = "b" * 16
    crash.stack_frames = ["donner::Parser::parse(char"]
    crash.signal = "==123==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x123"
    crash.input_bytes = artifact.read_bytes()
    crash.confirmed = True
    crash.isolated = True
    return crash


def test_issue_contains_portable_reproducer_without_host_paths(verified_crash):
    title, body = reporter.render_issue(verified_crash)
    assert "base64.b64decode" in body
    assert "-runs=1" in body
    assert str(verified_crash.binary_path) not in body
    assert "0x123" not in body
    assert "donner-fuzz:v2:" in body
    assert hashlib.sha256(verified_crash.input_bytes).hexdigest() in body


@pytest.mark.parametrize("data", [b"token=secret", b"/home/example/private", b"-----BEGIN PRIVATE KEY-----", b"a" * (24 * 1024 + 1)])
def test_sensitive_or_oversized_input_stays_local(verified_crash, data):
    verified_crash.input_bytes = data
    with pytest.raises(ValueError):
        reporter.render_issue(verified_crash)


def test_remote_duplicate_does_not_create_issue(verified_crash):
    with mock.patch.object(reporter, "find_existing_issue", return_value="https://github.com/jwmcglynn/donner/issues/12"), mock.patch.object(reporter.subprocess, "run") as command:
        assert reporter.file_github_issue(verified_crash).endswith("/12")
    command.assert_not_called()


def test_lookup_failure_fails_closed(verified_crash):
    with mock.patch.object(reporter, "find_existing_issue", side_effect=RuntimeError("offline")), mock.patch.object(reporter.subprocess, "run") as command:
        assert reporter.file_github_issue(verified_crash) is None
    command.assert_not_called()


def test_uncertain_publication_is_not_repeated(verified_crash):
    import subprocess
    with mock.patch.object(reporter, "find_existing_issue", return_value=None), mock.patch.object(reporter.subprocess, "run", side_effect=subprocess.TimeoutExpired("gh", 30)) as command:
        assert reporter.file_github_issue(verified_crash) is None
        assert reporter.file_github_issue(verified_crash) is None
    assert command.call_count == 1
    assert reporter.load_known_crashes()[verified_crash.signature]["pending"]


def test_report_uses_body_file_and_existing_bug_label(verified_crash):
    def submit(argv, **kwargs):
        assert "--body-file" in argv and "--body" not in argv
        assert argv[argv.index("--label") + 1] == "bug"
        assert "base64.b64decode" in Path(argv[argv.index("--body-file") + 1]).read_text()
        return mock.Mock(returncode=0, stdout="https://github.com/jwmcglynn/donner/issues/12", stderr="")
    with mock.patch.object(reporter, "find_existing_issue", return_value=None), mock.patch.object(reporter.subprocess, "run", side_effect=submit):
        assert reporter.file_github_issue(verified_crash).endswith("/12")



def test_sandbox_command_hides_home_credentials_and_network(tmp_path, monkeypatch):
    import execution
    binary = tmp_path / "binary"
    binary.write_bytes(b"binary")
    monkeypatch.setenv("FUZZ_SANDBOX", "1")
    monkeypatch.setenv("FUZZ_REPO_DIR", str(tmp_path))
    monkeypatch.setenv("GH_TOKEN", "must-not-pass")
    monkeypatch.setattr(execution.shutil, "which", lambda tool: "/usr/bin/" + tool)
    cmd = execution.fuzzer_command(binary, ["-runs=1"])
    assert "--unshare-all" in cmd and "--clearenv" in cmd
    assert "must-not-pass" not in str(cmd)
    assert ["--ro-bind", "/", "/"] != cmd[1:4]
    assert str(Path.home()) not in cmd
    assert cmd[-3:] == ["--", "/fuzzer", "-runs=1"]


def test_sandbox_is_not_silently_bypassed(tmp_path, monkeypatch):
    import execution
    binary = tmp_path / "binary"
    binary.write_bytes(b"binary")
    monkeypatch.setenv("FUZZ_SANDBOX", "1")
    monkeypatch.setattr(execution.shutil, "which", lambda _: None)
    with pytest.raises(RuntimeError):
        execution.fuzzer_command(binary, [])



def test_unisolated_replay_cannot_publish(verified_crash):
    verified_crash.isolated = False
    with mock.patch.object(reporter.subprocess, "run") as command:
        assert reporter.file_github_issue(verified_crash) is None
    command.assert_not_called()
