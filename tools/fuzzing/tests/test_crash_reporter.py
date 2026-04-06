"""Tests for crash_reporter.py."""

import json
import textwrap
from pathlib import Path
from unittest import mock

import pytest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from run_continuous_fuzz import FuzzerTarget
from crash_reporter import (
    CrashInfo,
    classify_crash_type,
    compute_signature,
    find_crashes_in_run,
    load_known_crashes,
    parse_stack_trace,
    process_crashes,
    reproduce_crash,
    save_known_crashes,
    send_webhook,
)


# ============================================================================
# classify_crash_type
# ============================================================================

class TestClassifyCrashType:
    def test_crash(self):
        assert classify_crash_type("crash-abc123") == "crash"

    def test_timeout(self):
        assert classify_crash_type("timeout-def456") == "timeout"

    def test_oom(self):
        assert classify_crash_type("oom-ghi789") == "oom"

    def test_leak(self):
        assert classify_crash_type("leak-jkl012") == "leak"

    def test_unknown(self):
        assert classify_crash_type("some_other_file") == "unknown"

    def test_with_path(self):
        assert classify_crash_type("/tmp/crashes/crash-abc123") == "crash"


# ============================================================================
# parse_stack_trace
# ============================================================================

SAMPLE_ASAN_OUTPUT = textwrap.dedent("""\
    INFO: Running with entropic power schedule
    =================================================================
    ==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000001234
    READ of size 1 at 0x602000001234 thread T0
        #0 0x555555780abc in donner::svg::parser::SVGParser::parseElement(char const*, unsigned long) /home/user/donner/donner/svg/parser/SVGParser.cc:42:13
        #1 0x555555781def in donner::svg::parser::SVGParser::ParseSVG(std::string_view) /home/user/donner/donner/svg/parser/SVGParser.cc:100:5
        #2 0x555555782aaa in LLVMFuzzerTestOneInput /home/user/donner/donner/svg/parser/tests/SVGParser_fuzzer.cc:7:18
        #3 0x55555564bbbb in fuzzer::Fuzzer::ExecuteCallback(unsigned char const*, unsigned long) /src/libfuzzer/FuzzerLoop.cpp:614:13
        #4 0x55555564cccc in fuzzer::Fuzzer::RunOne(unsigned char const*, unsigned long) /src/libfuzzer/FuzzerLoop.cpp:516:7
        #5 0x7ffff7a00ddd in __libc_start_main /build/glibc/libc-start.c:308:16

    SUMMARY: AddressSanitizer: heap-buffer-overflow /home/user/donner/donner/svg/parser/SVGParser.cc:42:13
""")

SAMPLE_UBSAN_OUTPUT = textwrap.dedent("""\
    /home/user/donner/donner/base/parser/NumberParser.cc:55:10: runtime error: signed integer overflow
    =================================================================
    ==12345==ERROR: UndefinedBehaviorSanitizer: undefined-behavior
        #0 0x555555780abc in donner::base::parser::NumberParser::parse(char const*, unsigned long) /home/user/donner/donner/base/parser/NumberParser.cc:55:10
        #1 0x555555781def in LLVMFuzzerTestOneInput /home/user/donner/donner/base/parser/tests/NumberParser_fuzzer.cc:5:16
        #2 0x55555564bbbb in fuzzer::Fuzzer::ExecuteCallback(unsigned char const*, unsigned long) /src/libfuzzer/FuzzerLoop.cpp:614:13

    SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior
""")


class TestParseStackTrace:
    def test_asan_trace(self):
        trace, frames, signal = parse_stack_trace(SAMPLE_ASAN_OUTPUT)
        assert "heap-buffer-overflow" in signal
        assert "heap-buffer-overflow" in trace
        # Should include the parser frames but not LLVMFuzzerTestOneInput or fuzzer internals
        assert "donner::svg::parser::SVGParser::parseElement(char" in frames[0]
        assert "donner::svg::parser::SVGParser::ParseSVG(std::string_view)" in frames[1]
        assert len(frames) == 2  # Only the two real frames

    def test_ubsan_trace(self):
        trace, frames, signal = parse_stack_trace(SAMPLE_UBSAN_OUTPUT)
        assert "UndefinedBehaviorSanitizer" in signal
        assert len(frames) == 1
        assert "NumberParser::parse" in frames[0]

    def test_no_trace(self):
        trace, frames, signal = parse_stack_trace("no error here\njust normal output")
        assert trace == ""
        assert frames == []
        assert signal == ""

    def test_empty_input(self):
        trace, frames, signal = parse_stack_trace("")
        assert frames == []

    def test_filters_asan_internal_frames(self):
        output = textwrap.dedent("""\
            ==1==ERROR: AddressSanitizer: null-deref
                #0 0xaaa in __asan_report_load1 /asan/asan.cc:1
                #1 0xbbb in donner::RealFunction() /src/real.cc:10
                #2 0xccc in __interceptor_strlen /asan/interceptors.cc:1
                #3 0xddd in __sanitizer_print_stack /sanitizer/common.cc:1
                #4 0xeee in LLVMFuzzerTestOneInput /fuzz.cc:1
            SUMMARY: AddressSanitizer: null-deref
        """)
        _, frames, _ = parse_stack_trace(output)
        assert frames == ["donner::RealFunction()"]

    def test_preserves_frame_order(self):
        output = textwrap.dedent("""\
            ==1==ERROR: AddressSanitizer: stack-overflow
                #0 0xaaa in first::func() /a.cc:1
                #1 0xbbb in second::func() /b.cc:2
                #2 0xccc in third::func() /c.cc:3
                #3 0xddd in LLVMFuzzerTestOneInput /fuzz.cc:1
            SUMMARY: AddressSanitizer: stack-overflow
        """)
        _, frames, _ = parse_stack_trace(output)
        assert frames == ["first::func()", "second::func()", "third::func()"]


# ============================================================================
# compute_signature
# ============================================================================

class TestComputeSignature:
    def test_deterministic(self):
        frames = ["func_a()", "func_b()", "func_c()"]
        sig1 = compute_signature(frames)
        sig2 = compute_signature(frames)
        assert sig1 == sig2

    def test_different_frames_different_sig(self):
        sig1 = compute_signature(["func_a()", "func_b()"])
        sig2 = compute_signature(["func_c()", "func_d()"])
        assert sig1 != sig2

    def test_top_n_truncation(self):
        frames = ["a", "b", "c", "d", "e", "f", "g"]
        sig_5 = compute_signature(frames, top_n=5)
        sig_3 = compute_signature(frames, top_n=3)
        assert sig_5 != sig_3

    def test_same_top_frames_same_sig(self):
        """Different trailing frames shouldn't affect signature."""
        sig1 = compute_signature(["a", "b", "c", "d", "e", "EXTRA1"], top_n=5)
        sig2 = compute_signature(["a", "b", "c", "d", "e", "EXTRA2"], top_n=5)
        assert sig1 == sig2

    def test_returns_hex_string(self):
        sig = compute_signature(["func()"])
        assert len(sig) == 16
        assert all(c in "0123456789abcdef" for c in sig)

    def test_empty_frames(self):
        sig = compute_signature([])
        assert len(sig) == 16  # Still returns a valid hash


# ============================================================================
# find_crashes_in_run
# ============================================================================

class TestFindCrashesInRun:
    def test_finds_all_crash_types(self, tmp_path):
        fuzzer_dir = tmp_path / "test_fuzzer" / "crashes"
        fuzzer_dir.mkdir(parents=True)
        (fuzzer_dir / "crash-abc").write_bytes(b"c")
        (fuzzer_dir / "timeout-def").write_bytes(b"t")
        (fuzzer_dir / "oom-ghi").write_bytes(b"o")
        (fuzzer_dir / "leak-jkl").write_bytes(b"l")

        result = find_crashes_in_run(tmp_path)
        assert len(result) == 4
        names = {Path(f).name for _, f in result}
        assert names == {"crash-abc", "timeout-def", "oom-ghi", "leak-jkl"}
        assert all(name == "test_fuzzer" for name, _ in result)

    def test_ignores_non_crash_files(self, tmp_path):
        fuzzer_dir = tmp_path / "test_fuzzer" / "crashes"
        fuzzer_dir.mkdir(parents=True)
        (fuzzer_dir / "crash-abc").write_bytes(b"c")
        (fuzzer_dir / "normal_file").write_bytes(b"n")

        result = find_crashes_in_run(tmp_path)
        assert len(result) == 1

    def test_multiple_fuzzers(self, tmp_path):
        for name in ["fuzzer_a", "fuzzer_b"]:
            d = tmp_path / name / "crashes"
            d.mkdir(parents=True)
            (d / "crash-001").write_bytes(b"data")

        result = find_crashes_in_run(tmp_path)
        assert len(result) == 2
        fuzzers = {name for name, _ in result}
        assert fuzzers == {"fuzzer_a", "fuzzer_b"}

    def test_no_crashes(self, tmp_path):
        (tmp_path / "test_fuzzer" / "crashes").mkdir(parents=True)
        assert find_crashes_in_run(tmp_path) == []

    def test_no_crash_dir(self, tmp_path):
        (tmp_path / "test_fuzzer" / "corpus").mkdir(parents=True)
        assert find_crashes_in_run(tmp_path) == []

    def test_empty_run_dir(self, tmp_path):
        assert find_crashes_in_run(tmp_path) == []

    def test_sorted_output(self, tmp_path):
        for name in ["z_fuzzer", "a_fuzzer"]:
            d = tmp_path / name / "crashes"
            d.mkdir(parents=True)
            (d / "crash-001").write_bytes(b"data")

        result = find_crashes_in_run(tmp_path)
        fuzzer_names = [name for name, _ in result]
        assert fuzzer_names == ["a_fuzzer", "z_fuzzer"]


# ============================================================================
# known_crashes ledger
# ============================================================================

class TestKnownCrashesLedger:
    def test_load_empty(self, tmp_path, monkeypatch):
        monkeypatch.setattr("crash_reporter.KNOWN_CRASHES_FILE", tmp_path / "none.json")
        assert load_known_crashes() == {}

    def test_save_and_load(self, tmp_path, monkeypatch):
        ledger_file = tmp_path / "crashes.json"
        monkeypatch.setattr("crash_reporter.KNOWN_CRASHES_FILE", ledger_file)

        data = {"sig123": {"fuzzer": "test", "issue_url": "https://github.com/test/1"}}
        save_known_crashes(data)

        loaded = load_known_crashes()
        assert loaded == data

    def test_creates_parent_dirs(self, tmp_path, monkeypatch):
        ledger_file = tmp_path / "subdir" / "crashes.json"
        monkeypatch.setattr("crash_reporter.KNOWN_CRASHES_FILE", ledger_file)
        save_known_crashes({"test": {}})
        assert ledger_file.exists()


# ============================================================================
# reproduce_crash
# ============================================================================

class TestReproduceCrash:
    def test_captures_stderr(self, tmp_path):
        script = tmp_path / "fake_fuzzer.sh"
        script.write_text("#!/bin/bash\necho 'error output' >&2\nexit 1\n")
        script.chmod(0o755)
        crash_file = tmp_path / "crash-abc"
        crash_file.write_bytes(b"input")

        result = reproduce_crash(script, crash_file)
        assert "error output" in result

    def test_timeout_handling(self, tmp_path):
        script = tmp_path / "slow_fuzzer.sh"
        script.write_text("#!/bin/bash\nsleep 60\n")
        script.chmod(0o755)
        crash_file = tmp_path / "crash-abc"
        crash_file.write_bytes(b"input")

        result = reproduce_crash(script, crash_file, timeout=1)
        assert "timed out" in result

    def test_missing_binary(self, tmp_path):
        crash_file = tmp_path / "crash-abc"
        crash_file.write_bytes(b"input")
        result = reproduce_crash(Path("/nonexistent/binary"), crash_file)
        assert "failed" in result


# ============================================================================
# process_crashes
# ============================================================================

class TestProcessCrashes:
    @pytest.fixture
    def run_with_crash(self, tmp_path):
        """Set up a run directory with one crash artifact."""
        run_dir = tmp_path / "run"
        crash_dir = run_dir / "test_fuzzer" / "crashes"
        crash_dir.mkdir(parents=True)
        (crash_dir / "crash-abc123").write_bytes(b"crash input data")
        return run_dir

    @pytest.fixture
    def fake_target(self, tmp_path):
        """Create a target with a fake binary that outputs a stack trace."""
        script = tmp_path / "fake_binary.sh"
        script.write_text(textwrap.dedent("""\
            #!/bin/bash
            echo "==1==ERROR: AddressSanitizer: heap-buffer-overflow" >&2
            echo "    #0 0xaaa in donner::Parser::parse() /src/parser.cc:10" >&2
            echo "    #1 0xbbb in LLVMFuzzerTestOneInput /src/fuzz.cc:5" >&2
            echo "SUMMARY: AddressSanitizer: heap-buffer-overflow" >&2
            exit 1
        """))
        script.chmod(0o755)
        return FuzzerTarget(
            label="//donner/test:test_fuzzer_bin",
            name="test_fuzzer",
            binary_path=script,
        )

    def test_processes_new_crash(self, tmp_path, run_with_crash, fake_target, monkeypatch):
        monkeypatch.setattr("crash_reporter.KNOWN_CRASHES_FILE", tmp_path / "known.json")
        monkeypatch.setattr("crash_reporter.CONFIG_FILE", tmp_path / "config.json")

        results = process_crashes(run_with_crash, [fake_target], dry_run=True)
        assert len(results) == 1
        assert results[0].crash_type == "crash"
        assert results[0].fuzzer_name == "test_fuzzer"
        assert "donner::Parser::parse()" in results[0].stack_frames

    def test_deduplicates_known_crash(self, tmp_path, run_with_crash, fake_target, monkeypatch):
        known_file = tmp_path / "known.json"
        monkeypatch.setattr("crash_reporter.KNOWN_CRASHES_FILE", known_file)
        monkeypatch.setattr("crash_reporter.CONFIG_FILE", tmp_path / "config.json")

        # First run — new crash
        results1 = process_crashes(run_with_crash, [fake_target], dry_run=False)
        assert len(results1) == 1
        sig = results1[0].signature

        # Verify it was saved
        known = load_known_crashes()
        assert sig in known

        # Second run — should be deduplicated
        results2 = process_crashes(run_with_crash, [fake_target], dry_run=False)
        assert len(results2) == 1
        # The crash was found but it's a duplicate

    def test_no_crashes(self, tmp_path, monkeypatch):
        run_dir = tmp_path / "run"
        (run_dir / "test_fuzzer" / "crashes").mkdir(parents=True)
        monkeypatch.setattr("crash_reporter.KNOWN_CRASHES_FILE", tmp_path / "known.json")
        monkeypatch.setattr("crash_reporter.CONFIG_FILE", tmp_path / "config.json")

        results = process_crashes(run_dir, [], dry_run=True)
        assert results == []

    def test_skips_target_without_binary(self, tmp_path, run_with_crash, monkeypatch):
        monkeypatch.setattr("crash_reporter.KNOWN_CRASHES_FILE", tmp_path / "known.json")
        monkeypatch.setattr("crash_reporter.CONFIG_FILE", tmp_path / "config.json")

        target = FuzzerTarget(label="//test:bin", name="test_fuzzer")
        results = process_crashes(run_with_crash, [target], dry_run=True)
        assert results == []


# ============================================================================
# send_webhook
# ============================================================================

class TestSendWebhook:
    def test_sends_json_payload(self):
        crash = CrashInfo()
        crash.fuzzer_name = "test_fuzzer"
        crash.crash_type = "crash"
        crash.stack_frames = ["donner::func()"]
        crash.signature = "abc123"
        crash.commit = "deadbeef"

        with mock.patch("crash_reporter.urllib.request.urlopen") as mock_urlopen:
            send_webhook(crash, "https://hooks.example.com/webhook")

            mock_urlopen.assert_called_once()
            call_args = mock_urlopen.call_args
            req = call_args[0][0]
            assert req.get_header("Content-type") == "application/json"
            payload = json.loads(req.data)
            assert "test_fuzzer" in payload["text"]

    def test_handles_failure_gracefully(self, capsys):
        crash = CrashInfo()
        crash.fuzzer_name = "test"
        crash.crash_type = "crash"
        crash.stack_frames = []
        crash.signature = "sig"
        crash.commit = "abc"

        with mock.patch("crash_reporter.urllib.request.urlopen", side_effect=Exception("fail")):
            send_webhook(crash, "https://bad.url")

        captured = capsys.readouterr()
        assert "WARNING" in captured.err


# ============================================================================
# CrashInfo
# ============================================================================

class TestCrashInfo:
    def test_top_frame_with_frames(self):
        c = CrashInfo()
        c.stack_frames = ["first()", "second()"]
        assert c.top_frame == "first()"

    def test_top_frame_empty(self):
        c = CrashInfo()
        assert c.top_frame == "unknown"
