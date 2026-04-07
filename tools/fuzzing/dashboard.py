#!/usr/bin/env python3
"""Dashboard for Donner continuous fuzzing.

Summarizes recent fuzzing runs, shows coverage trends, corpus growth,
and crash history.

Usage:
    python3 tools/fuzzing/dashboard.py              # Summary of recent runs
    python3 tools/fuzzing/dashboard.py --runs=10    # Show last 10 runs
    python3 tools/fuzzing/dashboard.py --json       # Output as JSON
"""

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_continuous_fuzz import STATE_DIR


RUNS_DIR = STATE_DIR / "runs"
STATS_DIR = STATE_DIR / "stats"
KNOWN_CRASHES_FILE = STATE_DIR / "known_crashes.json"


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

def load_run_reports(max_runs: int = 5) -> list[dict]:
    """Load run reports, most recent first."""
    if not RUNS_DIR.is_dir():
        return []
    reports = []
    for run_dir in sorted(RUNS_DIR.iterdir(), reverse=True):
        if not run_dir.is_dir():
            continue
        report_file = run_dir / "run_report.json"
        if not report_file.exists():
            continue
        with open(report_file) as f:
            report = json.load(f)
        report["_dir"] = str(run_dir)
        report["_name"] = run_dir.name
        reports.append(report)
        if len(reports) >= max_runs:
            break
    return reports


def load_corpus_history() -> list[dict]:
    """Load corpus minimization history."""
    history_file = STATS_DIR / "corpus_history.jsonl"
    if not history_file.exists():
        return []
    entries = []
    for line in history_file.read_text().strip().splitlines():
        if line.strip():
            entries.append(json.loads(line))
    return entries


def load_known_crashes() -> dict:
    """Load known crashes ledger."""
    if KNOWN_CRASHES_FILE.exists():
        with open(KNOWN_CRASHES_FILE) as f:
            return json.load(f)
    return {}


# ---------------------------------------------------------------------------
# Display
# ---------------------------------------------------------------------------

def format_duration(secs: float) -> str:
    """Format seconds as human-readable duration."""
    if secs < 60:
        return f"{secs:.0f}s"
    elif secs < 3600:
        return f"{secs / 60:.0f}m{secs % 60:.0f}s"
    else:
        h = int(secs // 3600)
        m = int((secs % 3600) // 60)
        return f"{h}h{m}m"


def print_run_summary(reports: list[dict]) -> None:
    """Print a summary of recent runs."""
    if not reports:
        print("No fuzzing runs found.")
        return

    print("=" * 90)
    print("RECENT FUZZING RUNS")
    print("=" * 90)
    print(
        f"{'Run':<18} {'Duration':>9} {'Fuzzers':>8} {'Execs':>12} "
        f"{'Crashes':>8} {'Plateau':>8} {'Deadline':>8}"
    )
    print("-" * 90)

    for report in reports:
        name = report["_name"]
        duration = format_duration(report.get("total_duration_secs", 0))
        total_fuzzers = report.get("total_fuzzers", 0)
        total_execs = sum(f.get("total_execs", 0) for f in report.get("fuzzers", []))
        total_crashes = report.get("total_crashes", 0)

        plateau_count = sum(
            1 for f in report.get("fuzzers", [])
            if "plateau" in f.get("exit_reason", "")
        )
        deadline_count = sum(
            1 for f in report.get("fuzzers", [])
            if f.get("exit_reason", "") == "deadline"
        )

        print(
            f"{name:<18} {duration:>9} {total_fuzzers:>8} {total_execs:>12,} "
            f"{total_crashes:>8} {plateau_count:>8} {deadline_count:>8}"
        )

    print("=" * 90)


def print_coverage_trends(reports: list[dict]) -> None:
    """Print per-fuzzer coverage trends across recent runs."""
    if not reports:
        return

    # Collect all fuzzer names
    all_fuzzers = set()
    for report in reports:
        for f in report.get("fuzzers", []):
            all_fuzzers.add(f["name"])

    print("\nCOVERAGE TRENDS (peak edges per run)")
    print("=" * 90)

    # Header: fuzzer name + run columns
    run_names = [r["_name"][-8:] for r in reversed(reports)]  # Show HHMMSS
    header = f"{'Fuzzer':<40}" + "".join(f"{n:>10}" for n in run_names)
    print(header)
    print("-" * 90)

    for fuzzer_name in sorted(all_fuzzers):
        row = f"{fuzzer_name:<40}"
        for report in reversed(reports):
            cov = 0
            for f in report.get("fuzzers", []):
                if f["name"] == fuzzer_name:
                    cov = f.get("peak_coverage", 0)
                    break
            if cov > 0:
                row += f"{cov:>10,}"
            else:
                row += f"{'—':>10}"
        print(row)

    print("=" * 90)


def print_corpus_stats(history: list[dict]) -> None:
    """Print corpus growth over time."""
    if not history:
        return

    print("\nCORPUS HISTORY (minimized inputs)")
    print("=" * 60)
    print(f"{'Timestamp':<28} {'Total Inputs':>14}")
    print("-" * 60)

    for entry in history[-10:]:  # Last 10 entries
        ts = entry.get("timestamp", "?")
        if "T" in ts:
            ts = ts[:19].replace("T", " ")
        total = entry.get("total_after", 0)
        print(f"{ts:<28} {total:>14,}")

    print("=" * 60)


def print_crash_history(crashes: dict) -> None:
    """Print known crashes."""
    if not crashes:
        print("\nNo known crashes. Parsers are holding up well.")
        return

    print(f"\nKNOWN CRASHES ({len(crashes)} total)")
    print("=" * 90)
    print(f"{'Signature':<18} {'Fuzzer':<30} {'Type':<10} {'Date':<12} {'Issue'}")
    print("-" * 90)

    for sig, info in sorted(crashes.items(), key=lambda x: x[1].get("date", "")):
        date = info.get("date", "?")[:10]
        print(
            f"{sig:<18} {info.get('fuzzer', '?'):<30} "
            f"{info.get('crash_type', '?'):<10} {date:<12} "
            f"{info.get('issue_url', 'N/A')}"
        )

    print("=" * 90)


def print_health_summary(reports: list[dict], crashes: dict) -> None:
    """Print a quick health check."""
    if not reports:
        return

    latest = reports[0]
    total_execs = sum(f.get("total_execs", 0) for f in latest.get("fuzzers", []))
    total_fuzzers = latest.get("total_fuzzers", 0)
    plateau_count = sum(
        1 for f in latest.get("fuzzers", [])
        if "plateau" in f.get("exit_reason", "")
    )
    still_growing = total_fuzzers - plateau_count

    print("\nHEALTH CHECK")
    print("-" * 60)
    print(f"  Last run:          {latest['_name']}")
    print(f"  Total executions:  {total_execs:,}")
    print(f"  Fuzzers:           {total_fuzzers} ({plateau_count} plateaued, {still_growing} still growing)")
    print(f"  Total runs:        {len(reports)} in history")
    print(f"  Known crashes:     {len(crashes)}")

    if still_growing > 0:
        growing = [
            f["name"] for f in latest.get("fuzzers", [])
            if "plateau" not in f.get("exit_reason", "") and f.get("exit_reason") != "deadline"
               or f.get("exit_reason") == "deadline"
        ]
        if growing:
            print(f"  Still growing:     {', '.join(growing[:5])}")

    print("-" * 60)


def output_json(reports: list[dict], history: list[dict], crashes: dict) -> None:
    """Output everything as JSON."""
    data = {
        "runs": reports,
        "corpus_history": history,
        "known_crashes": crashes,
    }
    # Remove internal fields
    for r in data["runs"]:
        r.pop("_dir", None)
        r.pop("_name", None)
    print(json.dumps(data, indent=2))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Continuous fuzzing dashboard for Donner",
    )
    parser.add_argument(
        "--runs", type=int, default=5,
        help="Number of recent runs to show (default: 5)",
    )
    parser.add_argument(
        "--json", action="store_true",
        help="Output as JSON instead of tables",
    )
    args = parser.parse_args()

    reports = load_run_reports(args.runs)
    history = load_corpus_history()
    crashes = load_known_crashes()

    if args.json:
        output_json(reports, history, crashes)
        return

    print_health_summary(reports, crashes)
    print_run_summary(reports)
    print_coverage_trends(reports)
    print_corpus_stats(history)
    print_crash_history(crashes)


if __name__ == "__main__":
    main()
