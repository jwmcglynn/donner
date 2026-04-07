#!/usr/bin/env python3
"""Continuous fuzzing runner for Donner.

Auto-discovers fuzzer targets via Bazel query, builds them with --config=asan-fuzzer,
runs them in parallel with coverage plateau detection, and collects crash artifacts.

Usage:
    # Run all fuzzers for 5 minutes each (default), 4 workers:
    python3 tools/fuzzing/run_continuous_fuzz.py

    # Run all fuzzers for 30 minutes each with 8 workers:
    python3 tools/fuzzing/run_continuous_fuzz.py --fuzzer-time=1800 --workers=8

    # Run specific fuzzers only:
    python3 tools/fuzzing/run_continuous_fuzz.py --filter=svg_parser

    # Stop fuzzers after 5 minutes of no new coverage (plateau detection):
    python3 tools/fuzzing/run_continuous_fuzz.py --plateau-timeout=300

    # Cap the entire run to 1 hour:
    python3 tools/fuzzing/run_continuous_fuzz.py --max-total-time=3600

    # Dry run (discover and build only, don't fuzz):
    python3 tools/fuzzing/run_continuous_fuzz.py --dry-run

    # Custom output directory for crashes:
    python3 tools/fuzzing/run_continuous_fuzz.py --output-dir=/tmp/fuzz-results
"""

import argparse
import concurrent.futures
import json
import os
import re
import select
import shutil
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

@dataclass
class FuzzerTarget:
    """A discovered fuzzer target."""
    # Full Bazel label, e.g. //donner/css/parser:color_parser_fuzzer_bin
    label: str
    # Short name, e.g. color_parser_fuzzer
    name: str
    # Path to the built binary (populated after build)
    binary_path: Optional[Path] = None
    # In-tree corpus directory (populated during discovery)
    corpus_dir: Optional[Path] = None


@dataclass
class FuzzerStats:
    """Stats parsed from a single fuzzer run."""
    name: str
    # Wall clock duration in seconds
    duration_secs: float = 0.0
    # Total executions
    total_execs: int = 0
    # Peak edge coverage
    peak_coverage: int = 0
    # Peak feature count
    peak_features: int = 0
    # Corpus size (number of inputs)
    corpus_size: int = 0
    # Executions per second
    execs_per_sec: int = 0
    # Number of crash artifacts found
    crashes_found: int = 0
    # List of crash file paths
    crash_files: list = field(default_factory=list)
    # Exit reason
    exit_reason: str = "completed"


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
STATE_DIR = Path.home() / ".donner-fuzz"
DEFAULT_FUZZER_TIME = 300  # 5 minutes per fuzzer
DEFAULT_INPUT_TIMEOUT = 30  # Per-input timeout
DEFAULT_RSS_LIMIT_MB = 4096
DEFAULT_WORKERS = 4
DEFAULT_PLATEAU_TIMEOUT = 600  # 10 minutes with no new coverage = stop
DEFAULT_MAX_TOTAL_TIME = 0  # 0 = no limit

# Regex patterns for parsing libFuzzer output
# Example: #12345	REDUCE cov: 1234 ft: 5678 corp: 100/50Kb ...
STATS_LINE_RE = re.compile(
    r"#(\d+)\s+\w+\s+cov:\s+(\d+)\s+ft:\s+(\d+)\s+corp:\s+(\d+)/(\S+)"
)
# Example: exec/s: 1234
EXECS_PER_SEC_RE = re.compile(r"exec/s:\s*(\d+)")
# Final stats line: stat::number_of_executed_units: 12345
FINAL_STAT_RE = re.compile(r"stat::(\w+):\s*(\d+)")


# ---------------------------------------------------------------------------
# Target discovery
# ---------------------------------------------------------------------------

def parse_bazel_query_output(
    output: str, name_filter: Optional[str] = None
) -> list[tuple[str, str]]:
    """Parse bazel query output into (label, name) pairs.

    Returns a sorted list of (label, short_name) tuples for _bin targets.
    """
    results = []
    for line in output.strip().splitlines():
        line = line.strip()
        if not line:
            continue
        # Extract short name: //donner/css/parser:color_parser_fuzzer_bin -> color_parser_fuzzer
        name = line.split(":")[-1]
        if name.endswith("_bin"):
            name = name[:-4]
        else:
            continue  # Skip non-_bin targets

        if name_filter and name_filter not in name:
            continue

        results.append((line, name))

    results.sort(key=lambda t: t[1])
    return results


def discover_targets(repo_root: Path, name_filter: Optional[str] = None) -> list[FuzzerTarget]:
    """Discover fuzzer targets via Bazel query."""
    print("Discovering fuzzer targets...")
    query = 'attr(tags, "fuzz_target", //...) intersect kind("cc_binary", //...)'
    result = subprocess.run(
        ["bazel", "query", query],
        cwd=repo_root,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"ERROR: bazel query failed:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)

    targets = []
    for label, name in parse_bazel_query_output(result.stdout, name_filter):
        target = FuzzerTarget(label=label, name=name)
        target.corpus_dir = find_corpus_dir(repo_root, label, name)
        targets.append(target)

    return targets


def find_corpus_dir(repo_root: Path, label: str, name: str) -> Optional[Path]:
    """Find the in-tree corpus directory for a fuzzer target."""
    # Derive the package path from the label: //donner/css/parser:foo -> donner/css/parser
    package = label.split(":")[0].lstrip("/")
    tests_dir = repo_root / package / "tests"

    # Convention: corpus dir name strips the _fuzzer suffix from the target name.
    # E.g., number_parser_fuzzer -> number_parser_corpus
    base_name = name.removesuffix("_fuzzer")

    # Check tests/ subdir first (most common), then package root
    for parent in [tests_dir, repo_root / package]:
        corpus_dir = parent / f"{base_name}_corpus"
        if corpus_dir.is_dir():
            return corpus_dir
        # Also try with the full name (in case convention varies)
        corpus_dir = parent / f"{name}_corpus"
        if corpus_dir.is_dir():
            return corpus_dir

    # Some fuzzers use testdata/ (e.g., woff_parser_fuzzer uses donner/base/fonts/testdata/)
    for testdata_parent in [tests_dir, repo_root / package]:
        testdata_dir = testdata_parent / "testdata"
        if testdata_dir.is_dir():
            return testdata_dir

    return None


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def build_targets(repo_root: Path, targets: list[FuzzerTarget]) -> bool:
    """Build all fuzzer targets with --config=asan-fuzzer."""
    labels = [t.label for t in targets]
    print(f"Building {len(labels)} fuzzer targets...")
    result = subprocess.run(
        ["bazel", "build", "--config=asan-fuzzer"] + labels,
        cwd=repo_root,
    )
    if result.returncode != 0:
        print("ERROR: bazel build failed", file=sys.stderr)
        return False

    # Resolve binary paths
    for target in targets:
        # //donner/css/parser:color_parser_fuzzer_bin -> bazel-bin/donner/css/parser/color_parser_fuzzer_bin
        package = target.label.split(":")[0].lstrip("/")
        bin_name = target.label.split(":")[-1]
        binary = repo_root / "bazel-bin" / package / bin_name
        if binary.exists():
            target.binary_path = binary
        else:
            print(f"WARNING: binary not found at {binary}", file=sys.stderr)

    return True


# ---------------------------------------------------------------------------
# Fuzzer execution
# ---------------------------------------------------------------------------

def parse_stats_line(line: str) -> Optional[dict]:
    """Parse a libFuzzer stats line and return extracted fields.

    Returns a dict with any of: total_execs, coverage, features, corpus_size,
    execs_per_sec, final_stat_key, final_stat_value. Returns None if the line
    contains no parseable stats.
    """
    result = {}

    m = STATS_LINE_RE.search(line)
    if m:
        result["total_execs"] = int(m.group(1))
        result["coverage"] = int(m.group(2))
        result["features"] = int(m.group(3))
        result["corpus_size"] = int(m.group(4))

    m = EXECS_PER_SEC_RE.search(line)
    if m:
        result["execs_per_sec"] = int(m.group(1))

    m = FINAL_STAT_RE.search(line)
    if m:
        result["final_stat_key"] = m.group(1)
        result["final_stat_value"] = int(m.group(2))

    return result if result else None


def _seed_corpus(target: FuzzerTarget, work_corpus: Path,
                  persistent_corpus_dir: Optional[Path]) -> None:
    """Seed the working corpus directory from in-tree and persistent sources."""
    # Seed from in-tree corpus
    if target.corpus_dir and target.corpus_dir.is_dir():
        for f in target.corpus_dir.iterdir():
            if f.is_file():
                dest = work_corpus / f.name
                if not dest.exists():
                    shutil.copy2(f, dest)

    # Seed from persistent corpus if available
    if persistent_corpus_dir and persistent_corpus_dir.is_dir():
        persistent = persistent_corpus_dir / target.name
        if persistent.is_dir():
            for f in persistent.iterdir():
                if f.is_file():
                    dest = work_corpus / f.name
                    if not dest.exists():
                        shutil.copy2(f, dest)


def _collect_crashes(crash_dir: Path) -> list[str]:
    """Collect crash artifact paths from the crash directory."""
    crash_files = []
    for f in crash_dir.iterdir():
        if f.is_file() and (
            f.name.startswith("crash-")
            or f.name.startswith("timeout-")
            or f.name.startswith("oom-")
            or f.name.startswith("leak-")
        ):
            crash_files.append(str(f))
    return crash_files


def run_fuzzer(
    target: FuzzerTarget,
    output_dir: Path,
    fuzzer_time: int,
    input_timeout: int,
    rss_limit_mb: int,
    persistent_corpus_dir: Optional[Path],
    plateau_timeout: int = DEFAULT_PLATEAU_TIMEOUT,
    global_deadline: float = 0,
) -> FuzzerStats:
    """Run a single fuzzer and return stats.

    Args:
        target: The fuzzer target to run.
        output_dir: Base output directory for this run.
        fuzzer_time: Max wall time for this fuzzer in seconds.
        input_timeout: Per-input timeout in seconds.
        rss_limit_mb: RSS memory limit in MB.
        persistent_corpus_dir: Path to persistent corpus storage.
        plateau_timeout: Seconds with no new coverage before stopping (0 = disabled).
        global_deadline: Monotonic time deadline for the entire run (0 = no deadline).
    """
    stats = FuzzerStats(name=target.name)

    if target.binary_path is None:
        stats.exit_reason = "no_binary"
        return stats

    # Check global deadline before starting
    if global_deadline > 0 and time.monotonic() >= global_deadline:
        stats.exit_reason = "deadline"
        return stats

    # If global deadline would cut this fuzzer short, reduce its time budget
    effective_time = fuzzer_time
    if global_deadline > 0:
        remaining = int(global_deadline - time.monotonic())
        if remaining <= 0:
            stats.exit_reason = "deadline"
            return stats
        effective_time = min(fuzzer_time, remaining)

    # Set up directories
    work_corpus = output_dir / target.name / "corpus"
    work_corpus.mkdir(parents=True, exist_ok=True)
    crash_dir = output_dir / target.name / "crashes"
    crash_dir.mkdir(parents=True, exist_ok=True)
    log_file = output_dir / target.name / "fuzzer.log"

    _seed_corpus(target, work_corpus, persistent_corpus_dir)

    cmd = [
        str(target.binary_path),
        str(work_corpus),
        f"-max_total_time={effective_time}",
        f"-timeout={input_timeout}",
        f"-rss_limit_mb={rss_limit_mb}",
        "-print_final_stats=1",
        f"-artifact_prefix={crash_dir}/",
    ]

    start_time = time.monotonic()

    # Coverage plateau tracking
    last_cov = 0
    last_cov_increase_time = start_time

    with open(log_file, "w") as log_fh:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            cwd=output_dir / target.name,
        )

        # Use os-level fd for non-blocking reads with select() so we can
        # check plateau/deadline timeouts even when libFuzzer is quiet.
        stderr_fd = proc.stderr.fileno()
        buf = b""
        terminated = False

        while not terminated:
            # Wait up to 2s for output, then check timeouts regardless
            ready = select.select([stderr_fd], [], [], 2.0)[0]
            if ready:
                chunk = os.read(stderr_fd, 65536)
                if not chunk:
                    break  # EOF
                buf += chunk
                # Process complete lines
                while b"\n" in buf:
                    line_bytes, buf = buf.split(b"\n", 1)
                    line = line_bytes.decode("utf-8", errors="replace")
                    log_fh.write(line + "\n")

                    parsed = parse_stats_line(line)
                    if parsed:
                        if "total_execs" in parsed:
                            stats.total_execs = parsed["total_execs"]
                        if "coverage" in parsed:
                            cov = parsed["coverage"]
                            stats.peak_coverage = max(stats.peak_coverage, cov)
                            stats.corpus_size = parsed.get(
                                "corpus_size", stats.corpus_size
                            )
                            stats.peak_features = max(
                                stats.peak_features,
                                parsed.get("features", 0),
                            )
                            if cov > last_cov:
                                last_cov = cov
                                last_cov_increase_time = time.monotonic()
                        if "execs_per_sec" in parsed:
                            stats.execs_per_sec = parsed["execs_per_sec"]
                        if parsed.get("final_stat_key") == "number_of_executed_units":
                            stats.total_execs = parsed["final_stat_value"]

            # Plateau detection (checked every iteration, not just on stats lines)
            if (
                plateau_timeout > 0
                and last_cov > 0
                and time.monotonic() - last_cov_increase_time >= plateau_timeout
            ):
                stats.exit_reason = f"plateau ({int(time.monotonic() - last_cov_increase_time)}s)"
                proc.terminate()
                proc.wait(timeout=10)
                terminated = True

            # Global deadline check
            if global_deadline > 0 and time.monotonic() >= global_deadline:
                stats.exit_reason = "deadline"
                proc.terminate()
                proc.wait(timeout=10)
                terminated = True

        # Process any remaining buffered data
        if buf:
            log_fh.write(buf.decode("utf-8", errors="replace"))

        if not terminated:
            proc.wait()

    stats.duration_secs = time.monotonic() - start_time

    # Collect crash artifacts
    stats.crash_files = _collect_crashes(crash_dir)
    stats.crashes_found = len(stats.crash_files)

    if stats.exit_reason == "completed":
        if proc.returncode != 0 and stats.crashes_found > 0:
            stats.exit_reason = "crash"
        elif proc.returncode != 0:
            stats.exit_reason = f"error (exit code {proc.returncode})"

    return stats


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def print_summary(all_stats: list[FuzzerStats], total_duration: float) -> None:
    """Print a summary table of all fuzzer runs."""
    print("\n" + "=" * 80)
    print("FUZZING SUMMARY")
    print("=" * 80)
    print(
        f"{'Fuzzer':<40} {'Time':>6} {'Execs':>10} {'Cov':>6} "
        f"{'Corpus':>7} {'Crashes':>7} {'Status':<10}"
    )
    print("-" * 80)

    total_execs = 0
    total_crashes = 0
    for s in all_stats:
        duration_str = f"{s.duration_secs:.0f}s"
        print(
            f"{s.name:<40} {duration_str:>6} {s.total_execs:>10,} {s.peak_coverage:>6} "
            f"{s.corpus_size:>7} {s.crashes_found:>7} {s.exit_reason:<10}"
        )
        total_execs += s.total_execs
        total_crashes += s.crashes_found

    print("-" * 80)
    print(
        f"{'TOTAL':<40} {total_duration:.0f}s {total_execs:>10,} "
        f"{'':>6} {'':>7} {total_crashes:>7}"
    )
    print("=" * 80)

    if total_crashes > 0:
        print(f"\n⚠ {total_crashes} CRASH(ES) FOUND:")
        for s in all_stats:
            for cf in s.crash_files:
                print(f"  {s.name}: {cf}")
    else:
        print("\nNo crashes found.")


def write_run_report(all_stats: list[FuzzerStats], output_dir: Path, total_duration: float) -> None:
    """Write a JSON run report to the output directory."""
    report = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "total_duration_secs": total_duration,
        "total_fuzzers": len(all_stats),
        "total_crashes": sum(s.crashes_found for s in all_stats),
        "fuzzers": [],
    }
    for s in all_stats:
        report["fuzzers"].append({
            "name": s.name,
            "duration_secs": s.duration_secs,
            "total_execs": s.total_execs,
            "peak_coverage": s.peak_coverage,
            "peak_features": s.peak_features,
            "corpus_size": s.corpus_size,
            "execs_per_sec": s.execs_per_sec,
            "crashes_found": s.crashes_found,
            "crash_files": s.crash_files,
            "exit_reason": s.exit_reason,
        })

    report_path = output_dir / "run_report.json"
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)
    print(f"\nRun report written to: {report_path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Continuous fuzzing runner for Donner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--fuzzer-time",
        type=int,
        default=DEFAULT_FUZZER_TIME,
        help=f"Max time per fuzzer in seconds (default: {DEFAULT_FUZZER_TIME})",
    )
    parser.add_argument(
        "--input-timeout",
        type=int,
        default=DEFAULT_INPUT_TIMEOUT,
        help=f"Per-input timeout in seconds (default: {DEFAULT_INPUT_TIMEOUT})",
    )
    parser.add_argument(
        "--rss-limit-mb",
        type=int,
        default=DEFAULT_RSS_LIMIT_MB,
        help=f"RSS memory limit per fuzzer in MB (default: {DEFAULT_RSS_LIMIT_MB})",
    )
    parser.add_argument(
        "--filter",
        type=str,
        default=None,
        help="Only run fuzzers whose name contains this substring",
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default=None,
        help="Output directory for crash artifacts and logs (default: ~/.donner-fuzz/runs/<timestamp>)",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=DEFAULT_WORKERS,
        help=f"Number of fuzzers to run in parallel (default: {DEFAULT_WORKERS})",
    )
    parser.add_argument(
        "--plateau-timeout",
        type=int,
        default=DEFAULT_PLATEAU_TIMEOUT,
        help=(
            f"Stop a fuzzer after this many seconds with no new coverage "
            f"(default: {DEFAULT_PLATEAU_TIMEOUT}, 0 = disabled)"
        ),
    )
    parser.add_argument(
        "--max-total-time",
        type=int,
        default=DEFAULT_MAX_TOTAL_TIME,
        help=(
            "Max wall time for the entire run in seconds "
            "(default: 0 = no limit)"
        ),
    )
    parser.add_argument(
        "--minimize",
        action="store_true",
        help="Run corpus minimization after fuzzing completes",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Discover and build targets but don't run fuzzers",
    )
    args = parser.parse_args()

    # Set up output directory
    if args.output_dir:
        output_dir = Path(args.output_dir)
    else:
        timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        output_dir = STATE_DIR / "runs" / timestamp
    output_dir.mkdir(parents=True, exist_ok=True)

    persistent_corpus_dir = STATE_DIR / "corpus"

    # Discover targets
    targets = discover_targets(REPO_ROOT, name_filter=args.filter)
    if not targets:
        print("No fuzzer targets found.", file=sys.stderr)
        sys.exit(1)
    print(f"Found {len(targets)} fuzzer target(s):")
    for t in targets:
        corpus_info = f" (corpus: {t.corpus_dir})" if t.corpus_dir else " (no corpus)"
        print(f"  {t.name}{corpus_info}")

    # Build
    if not build_targets(REPO_ROOT, targets):
        sys.exit(1)

    if args.dry_run:
        print("\nDry run complete. Would run the following fuzzers:")
        for t in targets:
            print(f"  {t.name} -> {t.binary_path}")
        return

    # Compute global deadline
    global_deadline = 0.0
    if args.max_total_time > 0:
        global_deadline = time.monotonic() + args.max_total_time

    workers = max(1, args.workers)
    mode = "sequentially" if workers == 1 else f"with {workers} workers"
    plateau_info = (
        f", plateau timeout {args.plateau_timeout}s"
        if args.plateau_timeout > 0
        else ""
    )
    deadline_info = (
        f", total time limit {args.max_total_time}s"
        if args.max_total_time > 0
        else ""
    )
    print(
        f"\nRunning {len(targets)} fuzzers {mode} "
        f"(max {args.fuzzer_time}s each{plateau_info}{deadline_info})..."
    )
    print(f"Output directory: {output_dir}\n")

    all_stats = []
    total_start = time.monotonic()
    completed_count = 0
    print_lock = threading.Lock()

    def _run_one(target: FuzzerTarget) -> FuzzerStats:
        nonlocal completed_count
        stats = run_fuzzer(
            target=target,
            output_dir=output_dir,
            fuzzer_time=args.fuzzer_time,
            input_timeout=args.input_timeout,
            rss_limit_mb=args.rss_limit_mb,
            persistent_corpus_dir=persistent_corpus_dir,
            plateau_timeout=args.plateau_timeout,
            global_deadline=global_deadline,
        )
        with print_lock:
            completed_count += 1
            print(
                f"[{completed_count}/{len(targets)}] {stats.name}: "
                f"{stats.total_execs:,} execs, cov={stats.peak_coverage}, "
                f"{stats.crashes_found} crashes, {stats.duration_secs:.0f}s "
                f"({stats.exit_reason})"
            )
        return stats

    if workers == 1:
        for target in targets:
            all_stats.append(_run_one(target))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {pool.submit(_run_one, t): t for t in targets}
            for future in concurrent.futures.as_completed(futures):
                all_stats.append(future.result())

    # Sort results by name for consistent display
    all_stats.sort(key=lambda s: s.name)
    total_duration = time.monotonic() - total_start

    # Summary
    print_summary(all_stats, total_duration)
    write_run_report(all_stats, output_dir, total_duration)

    # Post-run corpus minimization
    if args.minimize:
        print("\n--- Post-run corpus minimization ---")
        from manage_corpus import minimize_target, _log_corpus_stats, PERSISTENT_CORPUS_DIR
        PERSISTENT_CORPUS_DIR.mkdir(parents=True, exist_ok=True)
        min_results = []
        for target in targets:
            run_corpus = output_dir / target.name / "corpus"
            print(f"  {target.name}...", end=" ", flush=True)
            result = minimize_target(target, run_corpus, PERSISTENT_CORPUS_DIR)
            min_results.append(result)
            if result["status"] == "ok":
                print(f"{result['before_run'] + result['before_persistent']} -> {result['after']}")
            else:
                print(result["status"])
        _log_corpus_stats(min_results)
        total_after = sum(r["after"] for r in min_results)
        print(f"  Persistent corpus: {total_after} total minimized inputs")


if __name__ == "__main__":
    main()
