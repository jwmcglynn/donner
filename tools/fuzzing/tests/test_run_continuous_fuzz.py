"""Tests for run_continuous_fuzz.py."""

import json
import os
import shutil
import subprocess
import textwrap
import time
from io import StringIO
from pathlib import Path
from unittest import mock

import pytest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from run_continuous_fuzz import (
    FuzzerStats,
    FuzzerTarget,
    STATS_LINE_RE,
    EXECS_PER_SEC_RE,
    FINAL_STAT_RE,
    _collect_crashes,
    _seed_corpus,
    find_corpus_dir,
    parse_bazel_query_output,
    parse_stats_line,
    print_summary,
    write_run_report,
    run_fuzzer,
    build_targets,
)


# ============================================================================
# parse_bazel_query_output
# ============================================================================

class TestParseBazelQueryOutput:
    def test_parses_standard_output(self):
        output = textwrap.dedent("""\
            //donner/base/parser:number_parser_fuzzer_bin
            //donner/css/parser:color_parser_fuzzer_bin
            //donner/svg/parser:svg_parser_fuzzer_bin
        """)
        result = parse_bazel_query_output(output)
        assert len(result) == 3
        assert result[0] == ("//donner/css/parser:color_parser_fuzzer_bin", "color_parser_fuzzer")
        assert result[1] == ("//donner/base/parser:number_parser_fuzzer_bin", "number_parser_fuzzer")
        assert result[2] == ("//donner/svg/parser:svg_parser_fuzzer_bin", "svg_parser_fuzzer")

    def test_sorted_by_name(self):
        output = "//z:z_fuzzer_bin\n//a:a_fuzzer_bin\n//m:m_fuzzer_bin\n"
        result = parse_bazel_query_output(output)
        names = [name for _, name in result]
        assert names == ["a_fuzzer", "m_fuzzer", "z_fuzzer"]

    def test_skips_non_bin_targets(self):
        output = textwrap.dedent("""\
            //donner/base/parser:number_parser_fuzzer_bin
            //donner/base/parser:number_parser_fuzzer_10_seconds
            //donner/base/parser:number_parser_fuzzer
        """)
        result = parse_bazel_query_output(output)
        assert len(result) == 1
        assert result[0][1] == "number_parser_fuzzer"

    def test_name_filter(self):
        output = textwrap.dedent("""\
            //donner/base/parser:number_parser_fuzzer_bin
            //donner/css/parser:color_parser_fuzzer_bin
            //donner/svg/parser:svg_parser_fuzzer_bin
        """)
        result = parse_bazel_query_output(output, name_filter="svg")
        assert len(result) == 1
        assert result[0][1] == "svg_parser_fuzzer"

    def test_filter_no_match(self):
        output = "//donner/base/parser:number_parser_fuzzer_bin\n"
        result = parse_bazel_query_output(output, name_filter="nonexistent")
        assert result == []

    def test_empty_output(self):
        assert parse_bazel_query_output("") == []
        assert parse_bazel_query_output("  \n  \n") == []

    def test_handles_whitespace(self):
        output = "  //donner/base:foo_fuzzer_bin  \n\n  //donner/css:bar_fuzzer_bin\n"
        result = parse_bazel_query_output(output)
        assert len(result) == 2


# ============================================================================
# find_corpus_dir
# ============================================================================

class TestFindCorpusDir:
    def test_standard_convention(self, tmp_path):
        """Fuzzer name minus _fuzzer suffix + _corpus in tests/ subdir."""
        (tmp_path / "donner/base/parser/tests/number_parser_corpus").mkdir(parents=True)
        result = find_corpus_dir(
            tmp_path, "//donner/base/parser:number_parser_fuzzer_bin", "number_parser_fuzzer"
        )
        assert result == tmp_path / "donner/base/parser/tests/number_parser_corpus"

    def test_full_name_convention(self, tmp_path):
        """Falls back to full name with _corpus suffix."""
        (tmp_path / "donner/base/parser/tests/number_parser_fuzzer_corpus").mkdir(parents=True)
        result = find_corpus_dir(
            tmp_path, "//donner/base/parser:number_parser_fuzzer_bin", "number_parser_fuzzer"
        )
        assert result == tmp_path / "donner/base/parser/tests/number_parser_fuzzer_corpus"

    def test_testdata_in_package(self, tmp_path):
        """Falls back to testdata/ in the package dir (e.g., woff_parser_fuzzer)."""
        (tmp_path / "donner/base/fonts/testdata").mkdir(parents=True)
        result = find_corpus_dir(
            tmp_path, "//donner/base/fonts:woff_parser_fuzzer_bin", "woff_parser_fuzzer"
        )
        assert result == tmp_path / "donner/base/fonts/testdata"

    def test_testdata_in_tests_subdir(self, tmp_path):
        """testdata/ in tests/ subdir is preferred over package root."""
        (tmp_path / "donner/base/fonts/tests/testdata").mkdir(parents=True)
        (tmp_path / "donner/base/fonts/testdata").mkdir(parents=True)
        result = find_corpus_dir(
            tmp_path, "//donner/base/fonts:woff_parser_fuzzer_bin", "woff_parser_fuzzer"
        )
        assert result == tmp_path / "donner/base/fonts/tests/testdata"

    def test_corpus_in_package_root(self, tmp_path):
        """Corpus dir directly in the package (not tests/ subdir)."""
        (tmp_path / "donner/base/parser/number_parser_corpus").mkdir(parents=True)
        result = find_corpus_dir(
            tmp_path, "//donner/base/parser:number_parser_fuzzer_bin", "number_parser_fuzzer"
        )
        assert result == tmp_path / "donner/base/parser/number_parser_corpus"

    def test_no_corpus_found(self, tmp_path):
        (tmp_path / "donner/base/parser").mkdir(parents=True)
        result = find_corpus_dir(
            tmp_path, "//donner/base/parser:number_parser_fuzzer_bin", "number_parser_fuzzer"
        )
        assert result is None

    def test_tests_dir_preferred_over_package_root(self, tmp_path):
        """tests/ subdir is checked before package root."""
        (tmp_path / "donner/base/parser/tests/number_parser_corpus").mkdir(parents=True)
        (tmp_path / "donner/base/parser/number_parser_corpus").mkdir(parents=True)
        result = find_corpus_dir(
            tmp_path, "//donner/base/parser:number_parser_fuzzer_bin", "number_parser_fuzzer"
        )
        assert result == tmp_path / "donner/base/parser/tests/number_parser_corpus"

    def test_name_without_fuzzer_suffix(self, tmp_path):
        """Target name that doesn't end in _fuzzer."""
        (tmp_path / "donner/base/encoding/tests/decompress_corpus").mkdir(parents=True)
        result = find_corpus_dir(
            tmp_path, "//donner/base/encoding:decompress_fuzzer_bin", "decompress_fuzzer"
        )
        assert result == tmp_path / "donner/base/encoding/tests/decompress_corpus"


# ============================================================================
# parse_stats_line
# ============================================================================

class TestParseStatsLine:
    def test_new_line(self):
        line = "#29\tNEW    cov: 727 ft: 841 corp: 12/341b exec/s: 0 rss: 34Mb"
        result = parse_stats_line(line)
        assert result["total_execs"] == 29
        assert result["coverage"] == 727
        assert result["features"] == 841
        assert result["corpus_size"] == 12
        assert result["execs_per_sec"] == 0

    def test_reduce_line(self):
        line = "#5553320\tREDUCE cov: 793 ft: 1035 corp: 67/1847b lim: 4096 exec/s: 58456 rss: 486Mb"
        result = parse_stats_line(line)
        assert result["total_execs"] == 5553320
        assert result["coverage"] == 793
        assert result["features"] == 1035
        assert result["corpus_size"] == 67
        assert result["execs_per_sec"] == 58456

    def test_done_line(self):
        line = "#7049312\tDONE   cov: 793 ft: 1035 corp: 67/1843b lim: 4096 exec/s: 58258 rss: 486Mb"
        result = parse_stats_line(line)
        assert result["total_execs"] == 7049312
        assert result["coverage"] == 793

    def test_inited_line(self):
        line = "#5\tINITED cov: 215 ft: 270 corp: 4/119b exec/s: 0 rss: 33Mb"
        result = parse_stats_line(line)
        assert result["total_execs"] == 5
        assert result["coverage"] == 215
        assert result["corpus_size"] == 4

    def test_final_stat_line(self):
        line = "stat::number_of_executed_units: 550034"
        result = parse_stats_line(line)
        assert result["final_stat_key"] == "number_of_executed_units"
        assert result["final_stat_value"] == 550034

    def test_final_stat_peak_rss(self):
        line = "stat::peak_rss_mb: 486"
        result = parse_stats_line(line)
        assert result["final_stat_key"] == "peak_rss_mb"
        assert result["final_stat_value"] == 486

    def test_exec_per_sec_standalone(self):
        """exec/s can appear in lines that don't match the full stats pattern."""
        line = "some other text exec/s: 12345 more text"
        result = parse_stats_line(line)
        assert result["execs_per_sec"] == 12345

    def test_non_stats_line(self):
        assert parse_stats_line("INFO: Running with entropic power schedule") is None
        assert parse_stats_line("") is None
        assert parse_stats_line("INFO: Seed: 3969154939") is None

    def test_new_func_line(self):
        line = "\tNEW_FUNC[1/184]: 0xb92a03dc333c"
        assert parse_stats_line(line) is None

    def test_large_corpus_with_kb_suffix(self):
        line = "#100000\tNEW    cov: 5000 ft: 8000 corp: 500/2048Kb exec/s: 10000 rss: 512Mb"
        result = parse_stats_line(line)
        assert result["corpus_size"] == 500
        assert result["coverage"] == 5000

    def test_pulse_line(self):
        line = "#1048576\tpulse  cov: 793 ft: 1035 corp: 67/1843b lim: 4096 exec/s: 60000 rss: 500Mb"
        result = parse_stats_line(line)
        assert result["coverage"] == 793
        assert result["execs_per_sec"] == 60000


# ============================================================================
# Regex patterns (direct tests)
# ============================================================================

class TestRegexPatterns:
    def test_stats_line_re_groups(self):
        line = "#12345\tREDUCE cov: 1234 ft: 5678 corp: 100/50Kb"
        m = STATS_LINE_RE.search(line)
        assert m is not None
        assert m.group(1) == "12345"  # execs
        assert m.group(2) == "1234"   # cov
        assert m.group(3) == "5678"   # ft
        assert m.group(4) == "100"    # corp count
        assert m.group(5) == "50Kb"   # corp size

    def test_execs_per_sec_re(self):
        m = EXECS_PER_SEC_RE.search("exec/s: 58456")
        assert m is not None
        assert m.group(1) == "58456"

    def test_final_stat_re(self):
        m = FINAL_STAT_RE.search("stat::number_of_executed_units: 550034")
        assert m is not None
        assert m.group(1) == "number_of_executed_units"
        assert m.group(2) == "550034"


# ============================================================================
# _collect_crashes
# ============================================================================

class TestCollectCrashes:
    def test_collects_crash_files(self, tmp_path):
        (tmp_path / "crash-abc123").write_bytes(b"data")
        (tmp_path / "timeout-def456").write_bytes(b"data")
        (tmp_path / "oom-ghi789").write_bytes(b"data")
        (tmp_path / "leak-jkl012").write_bytes(b"data")
        result = _collect_crashes(tmp_path)
        assert len(result) == 4
        names = {Path(f).name for f in result}
        assert names == {"crash-abc123", "timeout-def456", "oom-ghi789", "leak-jkl012"}

    def test_ignores_non_crash_files(self, tmp_path):
        (tmp_path / "crash-abc123").write_bytes(b"data")
        (tmp_path / "some_other_file").write_bytes(b"data")
        (tmp_path / "corpus_input_01").write_bytes(b"data")
        result = _collect_crashes(tmp_path)
        assert len(result) == 1

    def test_empty_directory(self, tmp_path):
        assert _collect_crashes(tmp_path) == []

    def test_ignores_subdirectories(self, tmp_path):
        (tmp_path / "crash-abc123").mkdir()
        assert _collect_crashes(tmp_path) == []


# ============================================================================
# _seed_corpus
# ============================================================================

class TestSeedCorpus:
    def test_seeds_from_intree(self, tmp_path):
        corpus_dir = tmp_path / "intree_corpus"
        corpus_dir.mkdir()
        (corpus_dir / "seed1").write_bytes(b"input1")
        (corpus_dir / "seed2").write_bytes(b"input2")

        work_dir = tmp_path / "work"
        work_dir.mkdir()

        target = FuzzerTarget(
            label="//test:fuzzer_bin", name="fuzzer", corpus_dir=corpus_dir
        )
        _seed_corpus(target, work_dir, None)

        assert (work_dir / "seed1").read_bytes() == b"input1"
        assert (work_dir / "seed2").read_bytes() == b"input2"

    def test_seeds_from_persistent(self, tmp_path):
        persistent_dir = tmp_path / "persistent"
        (persistent_dir / "fuzzer").mkdir(parents=True)
        (persistent_dir / "fuzzer" / "persist1").write_bytes(b"p1")

        work_dir = tmp_path / "work"
        work_dir.mkdir()

        target = FuzzerTarget(label="//test:fuzzer_bin", name="fuzzer")
        _seed_corpus(target, work_dir, persistent_dir)

        assert (work_dir / "persist1").read_bytes() == b"p1"

    def test_intree_takes_precedence_over_persistent(self, tmp_path):
        """If same-named file exists in both, in-tree (copied first) wins."""
        corpus_dir = tmp_path / "intree"
        corpus_dir.mkdir()
        (corpus_dir / "conflict").write_bytes(b"intree_version")

        persistent_dir = tmp_path / "persistent"
        (persistent_dir / "fuzzer").mkdir(parents=True)
        (persistent_dir / "fuzzer" / "conflict").write_bytes(b"persistent_version")

        work_dir = tmp_path / "work"
        work_dir.mkdir()

        target = FuzzerTarget(
            label="//test:fuzzer_bin", name="fuzzer", corpus_dir=corpus_dir
        )
        _seed_corpus(target, work_dir, persistent_dir)

        assert (work_dir / "conflict").read_bytes() == b"intree_version"

    def test_no_corpus_dirs(self, tmp_path):
        work_dir = tmp_path / "work"
        work_dir.mkdir()
        target = FuzzerTarget(label="//test:fuzzer_bin", name="fuzzer")
        _seed_corpus(target, work_dir, None)
        assert list(work_dir.iterdir()) == []

    def test_merges_both_sources(self, tmp_path):
        corpus_dir = tmp_path / "intree"
        corpus_dir.mkdir()
        (corpus_dir / "from_intree").write_bytes(b"a")

        persistent_dir = tmp_path / "persistent"
        (persistent_dir / "fuzzer").mkdir(parents=True)
        (persistent_dir / "fuzzer" / "from_persistent").write_bytes(b"b")

        work_dir = tmp_path / "work"
        work_dir.mkdir()

        target = FuzzerTarget(
            label="//test:fuzzer_bin", name="fuzzer", corpus_dir=corpus_dir
        )
        _seed_corpus(target, work_dir, persistent_dir)

        assert (work_dir / "from_intree").exists()
        assert (work_dir / "from_persistent").exists()


# ============================================================================
# run_fuzzer
# ============================================================================

class TestRunFuzzer:
    def test_no_binary(self, tmp_path):
        target = FuzzerTarget(label="//test:fuzzer_bin", name="test_fuzzer")
        stats = run_fuzzer(target, tmp_path, fuzzer_time=10, input_timeout=2,
                           rss_limit_mb=512, persistent_corpus_dir=None)
        assert stats.exit_reason == "no_binary"
        assert stats.total_execs == 0

    def test_deadline_before_start(self, tmp_path):
        target = FuzzerTarget(
            label="//test:fuzzer_bin", name="test_fuzzer",
            binary_path=Path("/usr/bin/true"),
        )
        stats = run_fuzzer(
            target, tmp_path, fuzzer_time=10, input_timeout=2,
            rss_limit_mb=512, persistent_corpus_dir=None,
            global_deadline=time.monotonic() - 1,  # Already past
        )
        assert stats.exit_reason == "deadline"

    def test_creates_output_directories(self, tmp_path):
        """Even with no binary, the function should handle missing dirs gracefully."""
        target = FuzzerTarget(label="//test:fuzzer_bin", name="my_fuzzer")
        run_fuzzer(target, tmp_path, fuzzer_time=10, input_timeout=2,
                   rss_limit_mb=512, persistent_corpus_dir=None)
        # No crash since we return early for no_binary


class TestRunFuzzerIntegration:
    """Integration tests that run a real (trivial) fuzzer process."""

    @pytest.fixture
    def fake_fuzzer(self, tmp_path):
        """Create a shell script that mimics libFuzzer output."""
        script = tmp_path / "fake_fuzzer.sh"
        script.write_text(textwrap.dedent("""\
            #!/bin/bash
            # Mimic libFuzzer output on stderr
            echo "#5	INITED cov: 100 ft: 200 corp: 3/50b exec/s: 0 rss: 30Mb" >&2
            sleep 0.1
            echo "#100	NEW    cov: 150 ft: 300 corp: 5/80b exec/s: 5000 rss: 30Mb" >&2
            sleep 0.1
            echo "#500	NEW    cov: 200 ft: 400 corp: 8/120b exec/s: 10000 rss: 30Mb" >&2
            sleep 0.1
            echo "#1000	REDUCE cov: 200 ft: 400 corp: 8/110b exec/s: 10000 rss: 30Mb" >&2
            echo "stat::number_of_executed_units: 1000" >&2
            echo "stat::peak_rss_mb: 30" >&2
        """))
        script.chmod(0o755)
        return script

    @pytest.fixture
    def crashing_fuzzer(self, tmp_path):
        """Create a shell script that mimics a crashing fuzzer."""
        script = tmp_path / "crash_fuzzer.sh"
        script.write_text(textwrap.dedent("""\
            #!/bin/bash
            ARTIFACT_PREFIX=""
            for arg in "$@"; do
                case "$arg" in
                    -artifact_prefix=*) ARTIFACT_PREFIX="${arg#-artifact_prefix=}" ;;
                esac
            done
            echo "#5	INITED cov: 100 ft: 200 corp: 3/50b exec/s: 1000 rss: 30Mb" >&2
            echo "#50	NEW    cov: 150 ft: 300 corp: 5/80b exec/s: 5000 rss: 30Mb" >&2
            # Write a crash artifact
            echo "crash data" > "${ARTIFACT_PREFIX}crash-abc123def456"
            echo "stat::number_of_executed_units: 50" >&2
            exit 77
        """))
        script.chmod(0o755)
        return script

    @pytest.fixture
    def plateau_fuzzer(self, tmp_path):
        """Fuzzer that reaches coverage then stalls."""
        script = tmp_path / "plateau_fuzzer.sh"
        script.write_text(textwrap.dedent("""\
            #!/bin/bash
            echo "#5	INITED cov: 100 ft: 200 corp: 3/50b exec/s: 1000 rss: 30Mb" >&2
            echo "#50	NEW    cov: 200 ft: 400 corp: 8/120b exec/s: 5000 rss: 30Mb" >&2
            # Stall — keep emitting same coverage
            i=0
            while true; do
                i=$((i + 1))
                echo "#$((50 + i))	REDUCE cov: 200 ft: 400 corp: 8/110b exec/s: 5000 rss: 30Mb" >&2
                sleep 0.5
            done
        """))
        script.chmod(0o755)
        return script

    def test_basic_run(self, tmp_path, fake_fuzzer):
        output_dir = tmp_path / "output"
        target = FuzzerTarget(
            label="//test:fuzzer_bin", name="test_fuzzer",
            binary_path=fake_fuzzer,
        )
        stats = run_fuzzer(
            target, output_dir, fuzzer_time=10, input_timeout=2,
            rss_limit_mb=512, persistent_corpus_dir=None,
            plateau_timeout=0,
        )
        assert stats.name == "test_fuzzer"
        assert stats.peak_coverage == 200
        assert stats.peak_features == 400
        assert stats.corpus_size == 8
        assert stats.total_execs == 1000  # From final stats
        assert stats.exit_reason == "completed"
        assert stats.crashes_found == 0

        # Verify log file was written
        log_file = output_dir / "test_fuzzer" / "fuzzer.log"
        assert log_file.exists()
        log_content = log_file.read_text()
        assert "INITED" in log_content
        assert "cov: 200" in log_content

    def test_crash_detection(self, tmp_path, crashing_fuzzer):
        output_dir = tmp_path / "output"
        target = FuzzerTarget(
            label="//test:fuzzer_bin", name="crash_fuzzer",
            binary_path=crashing_fuzzer,
        )
        stats = run_fuzzer(
            target, output_dir, fuzzer_time=10, input_timeout=2,
            rss_limit_mb=512, persistent_corpus_dir=None,
            plateau_timeout=0,
        )
        assert stats.exit_reason == "crash"
        assert stats.crashes_found == 1
        assert any("crash-abc123def456" in f for f in stats.crash_files)

    def test_plateau_detection(self, tmp_path, plateau_fuzzer):
        output_dir = tmp_path / "output"
        target = FuzzerTarget(
            label="//test:fuzzer_bin", name="plateau_fuzzer",
            binary_path=plateau_fuzzer,
        )
        stats = run_fuzzer(
            target, output_dir, fuzzer_time=60, input_timeout=2,
            rss_limit_mb=512, persistent_corpus_dir=None,
            plateau_timeout=3,  # 3 second plateau timeout
        )
        assert "plateau" in stats.exit_reason
        assert stats.peak_coverage == 200
        assert stats.duration_secs < 30  # Should stop well before 60s

    def test_global_deadline(self, tmp_path, plateau_fuzzer):
        output_dir = tmp_path / "output"
        target = FuzzerTarget(
            label="//test:fuzzer_bin", name="deadline_fuzzer",
            binary_path=plateau_fuzzer,
        )
        deadline = time.monotonic() + 3
        stats = run_fuzzer(
            target, output_dir, fuzzer_time=60, input_timeout=2,
            rss_limit_mb=512, persistent_corpus_dir=None,
            plateau_timeout=0,  # Disable plateau
            global_deadline=deadline,
        )
        assert stats.exit_reason == "deadline"
        assert stats.duration_secs < 15

    def test_corpus_seeding(self, tmp_path, fake_fuzzer):
        """Verify corpus files are seeded into the working directory."""
        intree_corpus = tmp_path / "intree"
        intree_corpus.mkdir()
        (intree_corpus / "seed_input").write_bytes(b"test input")

        output_dir = tmp_path / "output"
        target = FuzzerTarget(
            label="//test:fuzzer_bin", name="seed_fuzzer",
            binary_path=fake_fuzzer,
            corpus_dir=intree_corpus,
        )
        run_fuzzer(
            target, output_dir, fuzzer_time=10, input_timeout=2,
            rss_limit_mb=512, persistent_corpus_dir=None,
            plateau_timeout=0,
        )
        # The working corpus should contain the seeded input
        work_corpus = output_dir / "seed_fuzzer" / "corpus"
        assert (work_corpus / "seed_input").exists()


# ============================================================================
# write_run_report
# ============================================================================

class TestWriteRunReport:
    def test_writes_valid_json(self, tmp_path):
        stats = [
            FuzzerStats(
                name="fuzzer_a", duration_secs=10.5, total_execs=1000,
                peak_coverage=500, peak_features=800, corpus_size=50,
                execs_per_sec=100, crashes_found=0, crash_files=[],
                exit_reason="completed",
            ),
            FuzzerStats(
                name="fuzzer_b", duration_secs=5.0, total_execs=500,
                peak_coverage=200, peak_features=300, corpus_size=20,
                execs_per_sec=100, crashes_found=1,
                crash_files=["/tmp/crash-abc"],
                exit_reason="crash",
            ),
        ]
        write_run_report(stats, tmp_path, total_duration=15.5)

        report_path = tmp_path / "run_report.json"
        assert report_path.exists()

        with open(report_path) as f:
            report = json.load(f)

        assert report["total_fuzzers"] == 2
        assert report["total_crashes"] == 1
        assert abs(report["total_duration_secs"] - 15.5) < 0.01
        assert len(report["fuzzers"]) == 2
        assert report["fuzzers"][0]["name"] == "fuzzer_a"
        assert report["fuzzers"][1]["crash_files"] == ["/tmp/crash-abc"]
        assert "timestamp" in report

    def test_empty_stats(self, tmp_path):
        write_run_report([], tmp_path, total_duration=0.0)
        with open(tmp_path / "run_report.json") as f:
            report = json.load(f)
        assert report["total_fuzzers"] == 0
        assert report["total_crashes"] == 0


# ============================================================================
# print_summary
# ============================================================================

class TestPrintSummary:
    def test_no_crashes(self, capsys):
        stats = [
            FuzzerStats(name="fuzzer_a", duration_secs=10, total_execs=1000,
                        peak_coverage=500, corpus_size=50, exit_reason="completed"),
        ]
        print_summary(stats, 10.0)
        captured = capsys.readouterr()
        assert "FUZZING SUMMARY" in captured.out
        assert "fuzzer_a" in captured.out
        assert "No crashes found." in captured.out

    def test_with_crashes(self, capsys):
        stats = [
            FuzzerStats(name="fuzzer_a", duration_secs=5, total_execs=500,
                        peak_coverage=200, crashes_found=2,
                        crash_files=["/tmp/crash-1", "/tmp/crash-2"],
                        exit_reason="crash"),
        ]
        print_summary(stats, 5.0)
        captured = capsys.readouterr()
        assert "CRASH(ES) FOUND" in captured.out
        assert "/tmp/crash-1" in captured.out
        assert "/tmp/crash-2" in captured.out

    def test_total_row(self, capsys):
        stats = [
            FuzzerStats(name="a", total_execs=1000, crashes_found=1),
            FuzzerStats(name="b", total_execs=2000, crashes_found=0),
        ]
        print_summary(stats, 20.0)
        captured = capsys.readouterr()
        assert "TOTAL" in captured.out
        assert "3,000" in captured.out  # Total execs
        assert "1" in captured.out      # Total crashes


# ============================================================================
# build_targets
# ============================================================================

class TestBuildTargets:
    def test_resolves_binary_paths(self, tmp_path):
        # Create a fake bazel-bin structure
        bin_dir = tmp_path / "bazel-bin" / "donner" / "css" / "parser"
        bin_dir.mkdir(parents=True)
        (bin_dir / "color_parser_fuzzer_bin").write_bytes(b"fake binary")

        target = FuzzerTarget(
            label="//donner/css/parser:color_parser_fuzzer_bin",
            name="color_parser_fuzzer",
        )

        with mock.patch("run_continuous_fuzz.subprocess.run") as mock_run:
            mock_run.return_value = mock.Mock(returncode=0)
            result = build_targets(tmp_path, [target])

        assert result is True
        assert target.binary_path == bin_dir / "color_parser_fuzzer_bin"

    def test_build_failure(self, tmp_path):
        target = FuzzerTarget(label="//test:fuzzer_bin", name="fuzzer")
        with mock.patch("run_continuous_fuzz.subprocess.run") as mock_run:
            mock_run.return_value = mock.Mock(returncode=1)
            result = build_targets(tmp_path, [target])
        assert result is False

    def test_missing_binary_warns(self, tmp_path, capsys):
        target = FuzzerTarget(label="//test:fuzzer_bin", name="fuzzer")
        with mock.patch("run_continuous_fuzz.subprocess.run") as mock_run:
            mock_run.return_value = mock.Mock(returncode=0)
            build_targets(tmp_path, [target])
        assert target.binary_path is None
        captured = capsys.readouterr()
        assert "WARNING" in captured.err


# ============================================================================
# FuzzerTarget / FuzzerStats dataclass basics
# ============================================================================

class TestDataclasses:
    def test_fuzzer_target_defaults(self):
        t = FuzzerTarget(label="//test:bin", name="test")
        assert t.binary_path is None
        assert t.corpus_dir is None

    def test_fuzzer_stats_defaults(self):
        s = FuzzerStats(name="test")
        assert s.duration_secs == 0.0
        assert s.total_execs == 0
        assert s.peak_coverage == 0
        assert s.crashes_found == 0
        assert s.crash_files == []
        assert s.exit_reason == "completed"

    def test_fuzzer_stats_crash_files_not_shared(self):
        """Verify mutable default field doesn't share state between instances."""
        s1 = FuzzerStats(name="a")
        s2 = FuzzerStats(name="b")
        s1.crash_files.append("crash-1")
        assert s2.crash_files == []
