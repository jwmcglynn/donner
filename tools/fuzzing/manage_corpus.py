#!/usr/bin/env python3
"""Corpus management for Donner continuous fuzzing.

Merges, minimizes, and maintains fuzz corpus across runs. Uses libFuzzer's
built-in -merge=1 to deduplicate inputs and keep only those contributing
unique coverage.

Usage:
    # Minimize a run's corpus into the persistent corpus:
    python3 tools/fuzzing/manage_corpus.py minimize --run-dir=~/.donner-fuzz/runs/20260406-203045

    # Minimize the latest run:
    python3 tools/fuzzing/manage_corpus.py minimize --latest

    # Update in-tree corpus from persistent corpus (for committing):
    python3 tools/fuzzing/manage_corpus.py update-intree

    # Show corpus stats:
    python3 tools/fuzzing/manage_corpus.py stats
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

# Re-use discovery from the runner
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_continuous_fuzz import (
    REPO_ROOT,
    STATE_DIR,
    discover_targets,
    build_targets,
    FuzzerTarget,
)


PERSISTENT_CORPUS_DIR = STATE_DIR / "corpus"
STATS_DIR = STATE_DIR / "stats"
RUNS_DIR = STATE_DIR / "runs"


# ---------------------------------------------------------------------------
# Minimize
# ---------------------------------------------------------------------------

def find_latest_run() -> Optional[Path]:
    """Find the most recent run directory."""
    if not RUNS_DIR.is_dir():
        return None
    runs = sorted(RUNS_DIR.iterdir(), reverse=True)
    for r in runs:
        if r.is_dir() and (r / "run_report.json").exists():
            return r
    return None


def minimize_target(
    target: FuzzerTarget,
    run_corpus_dir: Path,
    persistent_dir: Path,
) -> dict:
    """Minimize corpus for a single fuzzer target using libFuzzer -merge=1.

    Merges the run corpus and existing persistent corpus into a fresh
    minimized persistent corpus. Returns stats about the operation.
    """
    result = {
        "name": target.name,
        "before_run": 0,
        "before_persistent": 0,
        "after": 0,
        "status": "ok",
    }

    if target.binary_path is None:
        result["status"] = "no_binary"
        return result

    # Count inputs before
    run_files = list(run_corpus_dir.iterdir()) if run_corpus_dir.is_dir() else []
    result["before_run"] = len([f for f in run_files if f.is_file()])

    persistent_target_dir = persistent_dir / target.name
    if persistent_target_dir.is_dir():
        result["before_persistent"] = len(
            [f for f in persistent_target_dir.iterdir() if f.is_file()]
        )

    if result["before_run"] == 0 and result["before_persistent"] == 0:
        result["status"] = "empty"
        return result

    # Create a fresh output directory for the merge
    with tempfile.TemporaryDirectory(prefix=f"fuzz-merge-{target.name}-") as tmp:
        merge_output = Path(tmp) / "merged"
        merge_output.mkdir()

        # Build the merge command: output_corpus input_corpus1 [input_corpus2 ...]
        cmd = [
            str(target.binary_path),
            "-merge=1",
            str(merge_output),
        ]

        # Add run corpus as input
        if run_corpus_dir.is_dir() and result["before_run"] > 0:
            cmd.append(str(run_corpus_dir))

        # Add existing persistent corpus as input
        if persistent_target_dir.is_dir() and result["before_persistent"] > 0:
            cmd.append(str(persistent_target_dir))

        # Add in-tree corpus as input (to preserve seed coverage)
        if target.corpus_dir and target.corpus_dir.is_dir():
            cmd.append(str(target.corpus_dir))

        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=300,  # 5 minute timeout for merge
        )

        if proc.returncode != 0:
            result["status"] = f"error (exit code {proc.returncode})"
            # Print stderr for debugging
            if proc.stderr:
                for line in proc.stderr.strip().splitlines()[-5:]:
                    print(f"    {line}")
            return result

        # Replace persistent corpus with merged result
        if persistent_target_dir.exists():
            shutil.rmtree(persistent_target_dir)
        persistent_target_dir.mkdir(parents=True, exist_ok=True)

        merged_files = [f for f in merge_output.iterdir() if f.is_file()]
        for f in merged_files:
            shutil.copy2(f, persistent_target_dir / f.name)

        result["after"] = len(merged_files)

    return result


def cmd_minimize(args: argparse.Namespace) -> None:
    """Minimize corpus from a run into the persistent corpus."""
    # Find the run directory
    if args.latest:
        run_dir = find_latest_run()
        if run_dir is None:
            print("ERROR: No runs found in ~/.donner-fuzz/runs/", file=sys.stderr)
            sys.exit(1)
    elif args.run_dir:
        run_dir = Path(args.run_dir).expanduser()
    else:
        print("ERROR: Specify --run-dir or --latest", file=sys.stderr)
        sys.exit(1)

    if not run_dir.is_dir():
        print(f"ERROR: Run directory not found: {run_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Minimizing corpus from: {run_dir}")
    print(f"Persistent corpus: {PERSISTENT_CORPUS_DIR}")

    # Discover and build targets (need binaries for -merge=1)
    targets = discover_targets(REPO_ROOT)
    if not targets:
        print("ERROR: No fuzzer targets found.", file=sys.stderr)
        sys.exit(1)

    if not build_targets(REPO_ROOT, targets):
        sys.exit(1)

    PERSISTENT_CORPUS_DIR.mkdir(parents=True, exist_ok=True)

    # Minimize each target
    print(f"\nMinimizing {len(targets)} fuzzer corpora...")
    all_results = []
    total_before = 0
    total_after = 0

    for target in targets:
        run_corpus = run_dir / target.name / "corpus"
        print(f"  {target.name}...", end=" ", flush=True)

        result = minimize_target(target, run_corpus, PERSISTENT_CORPUS_DIR)
        all_results.append(result)

        before = result["before_run"] + result["before_persistent"]
        total_before += before
        total_after += result["after"]

        if result["status"] == "ok":
            delta = result["after"] - result.get("before_persistent", 0)
            sign = "+" if delta >= 0 else ""
            print(
                f"{before} -> {result['after']} inputs "
                f"({sign}{delta} from persistent)"
            )
        elif result["status"] == "empty":
            print("(no corpus)")
        else:
            print(f"FAILED: {result['status']}")

    # Summary
    print(f"\nTotal: {total_before} inputs merged -> {total_after} minimized inputs")

    # Log stats
    _log_corpus_stats(all_results)


# ---------------------------------------------------------------------------
# Update in-tree
# ---------------------------------------------------------------------------

def cmd_update_intree(args: argparse.Namespace) -> None:
    """Copy minimized persistent corpus back to in-tree corpus directories."""
    if not PERSISTENT_CORPUS_DIR.is_dir():
        print(
            "ERROR: No persistent corpus found. Run 'minimize' first.",
            file=sys.stderr,
        )
        sys.exit(1)

    targets = discover_targets(REPO_ROOT)
    if not targets:
        print("ERROR: No fuzzer targets found.", file=sys.stderr)
        sys.exit(1)

    print("Updating in-tree corpus from persistent corpus...")
    print(f"  Persistent: {PERSISTENT_CORPUS_DIR}")
    print(f"  Repo root:  {REPO_ROOT}\n")

    updated = 0
    skipped = 0

    for target in targets:
        persistent_target = PERSISTENT_CORPUS_DIR / target.name
        if not persistent_target.is_dir():
            continue

        if target.corpus_dir is None:
            print(f"  {target.name}: SKIP (no in-tree corpus dir found)")
            skipped += 1
            continue

        persistent_files = {f.name: f for f in persistent_target.iterdir() if f.is_file()}
        intree_files = {f.name: f for f in target.corpus_dir.iterdir() if f.is_file()}

        # Add new files from persistent corpus
        new_count = 0
        for name, src in persistent_files.items():
            dest = target.corpus_dir / name
            if not dest.exists():
                shutil.copy2(src, dest)
                new_count += 1

        # Remove in-tree files not in the minimized persistent corpus
        # (they were redundant and removed during minimization)
        removed_count = 0
        if not args.no_prune:
            for name, path in intree_files.items():
                if name not in persistent_files:
                    path.unlink()
                    removed_count += 1

        total_now = len(list(target.corpus_dir.iterdir()))
        changes = []
        if new_count > 0:
            changes.append(f"+{new_count} new")
        if removed_count > 0:
            changes.append(f"-{removed_count} pruned")
        if not changes:
            changes.append("no changes")

        print(f"  {target.name}: {', '.join(changes)} ({total_now} total)")
        if new_count > 0 or removed_count > 0:
            updated += 1

    print(f"\n{updated} corpora updated, {skipped} skipped")

    if updated > 0:
        print("\nTo commit the updated corpus:")
        print(f"  cd {REPO_ROOT}")
        print("  git add donner/*/tests/*_corpus/ donner/*/*/tests/*_corpus/ donner/*/testdata/")
        print('  git commit -m "fuzzing: update corpus from continuous fuzzing"')


# ---------------------------------------------------------------------------
# Stats
# ---------------------------------------------------------------------------

def cmd_stats(args: argparse.Namespace) -> None:
    """Show corpus statistics."""
    targets = discover_targets(REPO_ROOT)

    print(f"{'Fuzzer':<40} {'In-tree':>8} {'Persistent':>11} {'Latest run':>11}")
    print("-" * 74)

    # Find latest run
    latest_run = find_latest_run()

    for target in targets:
        intree = 0
        if target.corpus_dir and target.corpus_dir.is_dir():
            intree = len([f for f in target.corpus_dir.iterdir() if f.is_file()])

        persistent = 0
        p = PERSISTENT_CORPUS_DIR / target.name
        if p.is_dir():
            persistent = len([f for f in p.iterdir() if f.is_file()])

        run = 0
        if latest_run:
            r = latest_run / target.name / "corpus"
            if r.is_dir():
                run = len([f for f in r.iterdir() if f.is_file()])

        print(f"{target.name:<40} {intree:>8} {persistent:>11} {run:>11}")

    # History
    history_file = STATS_DIR / "corpus_history.jsonl"
    if history_file.exists():
        lines = history_file.read_text().strip().splitlines()
        if lines:
            print(f"\n{len(lines)} minimize operations logged in {history_file}")


def _log_corpus_stats(results: list[dict]) -> None:
    """Append corpus stats to the history log."""
    STATS_DIR.mkdir(parents=True, exist_ok=True)
    history_file = STATS_DIR / "corpus_history.jsonl"

    entry = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "targets": {
            r["name"]: {
                "before_run": r["before_run"],
                "before_persistent": r["before_persistent"],
                "after": r["after"],
                "status": r["status"],
            }
            for r in results
        },
        "total_after": sum(r["after"] for r in results),
    }

    with open(history_file, "a") as f:
        f.write(json.dumps(entry) + "\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Corpus management for Donner continuous fuzzing",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # minimize
    p_min = subparsers.add_parser(
        "minimize",
        help="Minimize a run's corpus into the persistent corpus",
    )
    p_min.add_argument("--run-dir", type=str, help="Path to a specific run directory")
    p_min.add_argument(
        "--latest", action="store_true", help="Use the most recent run"
    )

    # update-intree
    p_update = subparsers.add_parser(
        "update-intree",
        help="Copy persistent corpus back to in-tree corpus directories",
    )
    p_update.add_argument(
        "--no-prune",
        action="store_true",
        help="Don't remove in-tree files absent from the minimized corpus",
    )

    # stats
    subparsers.add_parser("stats", help="Show corpus statistics")

    args = parser.parse_args()

    if args.command == "minimize":
        cmd_minimize(args)
    elif args.command == "update-intree":
        cmd_update_intree(args)
    elif args.command == "stats":
        cmd_stats(args)


if __name__ == "__main__":
    main()
