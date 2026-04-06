#!/usr/bin/env python3
"""Crash detection, deduplication, and reporting for Donner continuous fuzzing.

Processes crash artifacts from fuzzing runs, reproduces them to capture stack
traces, deduplicates by stack signature, and files GitHub Issues via `gh` CLI.

Usage:
    # Process crashes from the latest run:
    python3 tools/fuzzing/crash_reporter.py report --latest

    # Process a specific run:
    python3 tools/fuzzing/crash_reporter.py report --run-dir=~/.donner-fuzz/runs/20260406-203045

    # Dry run (detect and dedup but don't file issues):
    python3 tools/fuzzing/crash_reporter.py report --latest --dry-run

    # Show known crashes:
    python3 tools/fuzzing/crash_reporter.py list
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_continuous_fuzz import (
    REPO_ROOT,
    STATE_DIR,
    FuzzerTarget,
    discover_targets,
    build_targets,
)
from manage_corpus import find_latest_run, RUNS_DIR


KNOWN_CRASHES_FILE = STATE_DIR / "known_crashes.json"
CONFIG_FILE = STATE_DIR / "config.json"

# Regex for parsing ASAN/UBSAN stack frames
# Example: #0 0x5555557a1234 in FunctionName /path/to/file.cc:42:13
FRAME_RE = re.compile(
    r"#(\d+)\s+0x[0-9a-f]+\s+in\s+(\S+)\s+(\S+)"
)

# Frames to exclude from stack signatures (common runtime/fuzzer frames)
IGNORE_FRAME_PREFIXES = (
    "__asan_",
    "__ubsan_",
    "__sanitizer_",
    "__interceptor_",
    "__GI_",
    "__libc_start_",
    "fuzzer::Fuzzer::",
    "fuzzer::RunOneTest",
    "LLVMFuzzerTestOneInput",
    "main(",
    "_start",
)


# ---------------------------------------------------------------------------
# Data types
# ---------------------------------------------------------------------------

class CrashInfo:
    """Information about a single crash."""
    def __init__(self):
        self.fuzzer_name: str = ""
        self.fuzzer_label: str = ""
        self.crash_file: Path = Path()
        self.crash_type: str = "unknown"  # crash, timeout, oom, leak
        self.signal: str = ""
        self.stack_trace: str = ""
        self.stack_frames: list[str] = []
        self.signature: str = ""
        self.commit: str = ""
        self.binary_path: Path = Path()

    @property
    def top_frame(self) -> str:
        return self.stack_frames[0] if self.stack_frames else "unknown"


# ---------------------------------------------------------------------------
# Stack trace parsing
# ---------------------------------------------------------------------------

def classify_crash_type(filename: str) -> str:
    """Classify crash type from the artifact filename."""
    name = Path(filename).name
    if name.startswith("crash-"):
        return "crash"
    elif name.startswith("timeout-"):
        return "timeout"
    elif name.startswith("oom-"):
        return "oom"
    elif name.startswith("leak-"):
        return "leak"
    return "unknown"


def parse_stack_trace(stderr: str) -> tuple[str, list[str], str]:
    """Parse a sanitizer stack trace from fuzzer stderr.

    Returns (full_trace, filtered_frames, signal_info).
    """
    lines = stderr.splitlines()
    trace_lines = []
    frames = []
    signal = ""
    in_trace = False

    for line in lines:
        # Detect start of stack trace
        if "ERROR:" in line and ("AddressSanitizer" in line or "UndefinedBehaviorSanitizer" in line):
            in_trace = True
            trace_lines.append(line)
            # Extract signal/error type
            signal = line.strip()
            continue

        if "SUMMARY:" in line:
            trace_lines.append(line)
            in_trace = False
            continue

        if in_trace:
            trace_lines.append(line)
            m = FRAME_RE.match(line.strip())
            if m:
                func_name = m.group(2)
                # Filter out noise frames
                if any(func_name.startswith(p) for p in IGNORE_FRAME_PREFIXES):
                    continue
                frames.append(func_name)

    full_trace = "\n".join(trace_lines)
    return full_trace, frames, signal


def compute_signature(frames: list[str], top_n: int = 5) -> str:
    """Compute a deduplication signature from the top N stack frames."""
    sig_input = "\n".join(frames[:top_n])
    return hashlib.sha256(sig_input.encode()).hexdigest()[:16]


# ---------------------------------------------------------------------------
# Crash reproduction
# ---------------------------------------------------------------------------

def reproduce_crash(binary_path: Path, crash_file: Path, timeout: int = 30) -> str:
    """Run the fuzzer binary on a crash input to capture the stack trace."""
    try:
        proc = subprocess.run(
            [str(binary_path), str(crash_file)],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return proc.stderr
    except subprocess.TimeoutExpired:
        return "(reproduction timed out)"
    except Exception as e:
        return f"(reproduction failed: {e})"


# ---------------------------------------------------------------------------
# Known crashes ledger
# ---------------------------------------------------------------------------

def load_known_crashes() -> dict:
    """Load the known crashes ledger. Maps signature -> issue info."""
    if KNOWN_CRASHES_FILE.exists():
        with open(KNOWN_CRASHES_FILE) as f:
            return json.load(f)
    return {}


def save_known_crashes(crashes: dict) -> None:
    """Save the known crashes ledger."""
    KNOWN_CRASHES_FILE.parent.mkdir(parents=True, exist_ok=True)
    with open(KNOWN_CRASHES_FILE, "w") as f:
        json.dump(crashes, f, indent=2)


# ---------------------------------------------------------------------------
# GitHub Issue filing
# ---------------------------------------------------------------------------

def file_github_issue(crash: CrashInfo, repo: str = "jwmcglynn/donner") -> Optional[str]:
    """File a GitHub Issue for a crash via `gh` CLI. Returns the issue URL or None."""
    title = f"Fuzzing crash: {crash.fuzzer_name} — {crash.crash_type} in {crash.top_frame}"
    if len(title) > 120:
        title = title[:117] + "..."

    body = f"""## Fuzzing Crash Report

**Fuzzer:** `{crash.fuzzer_label}`
**Crash type:** {crash.crash_type}
**Signal:** {crash.signal or "N/A"}
**Commit:** {crash.commit}
**Date:** {datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")}
**Signature:** `{crash.signature}`

### Stack Trace
```
{crash.stack_trace}
```

### Reproduction
```sh
bazel build --config=asan-fuzzer {crash.fuzzer_label}
{crash.binary_path} <crash_input_file>
```

The crash input ({crash.crash_file.stat().st_size} bytes) should be added to the
fuzzer's corpus directory as a regression test after the fix.
"""

    if not shutil.which("gh"):
        print("    WARNING: `gh` CLI not found, cannot file issue", file=sys.stderr)
        return None

    try:
        proc = subprocess.run(
            [
                "gh", "issue", "create",
                "--repo", repo,
                "--title", title,
                "--label", "fuzzing,crash,automated",
                "--body", body,
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        if proc.returncode == 0:
            url = proc.stdout.strip()
            return url
        else:
            print(f"    WARNING: gh issue create failed: {proc.stderr.strip()}", file=sys.stderr)
            return None
    except Exception as e:
        print(f"    WARNING: gh issue create failed: {e}", file=sys.stderr)
        return None


# ---------------------------------------------------------------------------
# Webhook notifications
# ---------------------------------------------------------------------------

def load_config() -> dict:
    """Load configuration from ~/.donner-fuzz/config.json."""
    if CONFIG_FILE.exists():
        with open(CONFIG_FILE) as f:
            return json.load(f)
    return {}


def send_webhook(crash: CrashInfo, webhook_url: str) -> None:
    """Send a crash notification to a webhook URL (Slack-compatible JSON)."""
    payload = {
        "text": (
            f"Fuzzing crash in `{crash.fuzzer_name}`: "
            f"{crash.crash_type} in `{crash.top_frame}` "
            f"(sig: `{crash.signature}`)"
        ),
        "blocks": [
            {
                "type": "section",
                "text": {
                    "type": "mrkdwn",
                    "text": (
                        f"*Fuzzing crash: {crash.fuzzer_name}*\n"
                        f"Type: `{crash.crash_type}` | Top frame: `{crash.top_frame}`\n"
                        f"Signature: `{crash.signature}` | Commit: `{crash.commit}`"
                    ),
                },
            },
        ],
    }
    data = json.dumps(payload).encode()
    req = urllib.request.Request(
        webhook_url,
        data=data,
        headers={"Content-Type": "application/json"},
    )
    try:
        urllib.request.urlopen(req, timeout=10)
    except Exception as e:
        print(f"    WARNING: webhook notification failed: {e}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Process crashes from a run
# ---------------------------------------------------------------------------

def get_current_commit() -> str:
    """Get the current HEAD commit SHA."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()[:12] if result.returncode == 0 else "unknown"
    except Exception:
        return "unknown"


def find_crashes_in_run(run_dir: Path) -> list[tuple[str, Path]]:
    """Find all crash artifacts in a run directory.

    Returns list of (fuzzer_name, crash_file_path) tuples.
    """
    crashes = []
    for fuzzer_dir in sorted(run_dir.iterdir()):
        if not fuzzer_dir.is_dir():
            continue
        crash_dir = fuzzer_dir / "crashes"
        if not crash_dir.is_dir():
            continue
        for f in sorted(crash_dir.iterdir()):
            if f.is_file() and any(
                f.name.startswith(p) for p in ("crash-", "timeout-", "oom-", "leak-")
            ):
                crashes.append((fuzzer_dir.name, f))
    return crashes


def process_crashes(
    run_dir: Path,
    targets: list[FuzzerTarget],
    dry_run: bool = False,
) -> list[CrashInfo]:
    """Process all crashes from a run: reproduce, dedup, report."""
    target_map = {t.name: t for t in targets}
    known = load_known_crashes()
    config = load_config()
    webhook_url = config.get("webhook_url")
    commit = get_current_commit()

    raw_crashes = find_crashes_in_run(run_dir)
    if not raw_crashes:
        print("No crash artifacts found.")
        return []

    print(f"Found {len(raw_crashes)} crash artifact(s). Processing...\n")

    processed = []
    new_count = 0
    dup_count = 0

    for fuzzer_name, crash_file in raw_crashes:
        target = target_map.get(fuzzer_name)
        if target is None or target.binary_path is None:
            print(f"  {fuzzer_name}/{crash_file.name}: SKIP (no binary)")
            continue

        crash = CrashInfo()
        crash.fuzzer_name = fuzzer_name
        crash.fuzzer_label = target.label
        crash.crash_file = crash_file
        crash.crash_type = classify_crash_type(crash_file.name)
        crash.binary_path = target.binary_path
        crash.commit = commit

        # Reproduce to get stack trace
        print(f"  Reproducing {fuzzer_name}/{crash_file.name}...", end=" ", flush=True)
        stderr = reproduce_crash(target.binary_path, crash_file)
        crash.stack_trace, crash.stack_frames, crash.signal = parse_stack_trace(stderr)

        if not crash.stack_frames:
            crash.signature = f"no-trace-{hashlib.sha256(crash_file.read_bytes()).hexdigest()[:16]}"
        else:
            crash.signature = compute_signature(crash.stack_frames)

        # Check deduplication
        if crash.signature in known:
            existing = known[crash.signature]
            print(f"DUPLICATE (sig={crash.signature}, issue={existing.get('issue_url', 'N/A')})")
            dup_count += 1
            processed.append(crash)
            continue

        print(f"NEW (sig={crash.signature}, top={crash.top_frame})")
        new_count += 1

        if dry_run:
            print(f"    [dry-run] Would file issue for {crash.crash_type} in {crash.top_frame}")
        else:
            issue_url = file_github_issue(crash)
            known[crash.signature] = {
                "fuzzer": fuzzer_name,
                "crash_type": crash.crash_type,
                "top_frame": crash.top_frame,
                "issue_url": issue_url,
                "commit": commit,
                "date": datetime.now(timezone.utc).isoformat(),
                "crash_file": str(crash_file),
            }
            if issue_url:
                print(f"    Filed: {issue_url}")

            if webhook_url:
                send_webhook(crash, webhook_url)

        processed.append(crash)

    if not dry_run:
        save_known_crashes(known)

    print(f"\nSummary: {new_count} new, {dup_count} duplicates, {len(processed)} total")
    return processed


# ---------------------------------------------------------------------------
# CLI commands
# ---------------------------------------------------------------------------

def cmd_report(args: argparse.Namespace) -> None:
    """Process and report crashes from a fuzzing run."""
    if args.latest:
        run_dir = find_latest_run()
        if run_dir is None:
            print("ERROR: No runs found.", file=sys.stderr)
            sys.exit(1)
    elif args.run_dir:
        run_dir = Path(args.run_dir).expanduser()
    else:
        print("ERROR: Specify --run-dir or --latest", file=sys.stderr)
        sys.exit(1)

    if not run_dir.is_dir():
        print(f"ERROR: Run directory not found: {run_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Processing crashes from: {run_dir}")

    targets = discover_targets(REPO_ROOT)
    if not targets:
        print("ERROR: No fuzzer targets found.", file=sys.stderr)
        sys.exit(1)

    if not build_targets(REPO_ROOT, targets):
        sys.exit(1)

    process_crashes(run_dir, targets, dry_run=args.dry_run)


def cmd_list(args: argparse.Namespace) -> None:
    """List known crashes."""
    known = load_known_crashes()
    if not known:
        print("No known crashes.")
        return

    print(f"{'Signature':<18} {'Fuzzer':<35} {'Type':<10} {'Top Frame':<40} {'Issue'}")
    print("-" * 120)
    for sig, info in sorted(known.items(), key=lambda x: x[1].get("date", "")):
        print(
            f"{sig:<18} {info.get('fuzzer', '?'):<35} {info.get('crash_type', '?'):<10} "
            f"{info.get('top_frame', '?'):<40} {info.get('issue_url', 'N/A')}"
        )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Crash reporting for Donner continuous fuzzing",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    p_report = subparsers.add_parser("report", help="Process and report crashes")
    p_report.add_argument("--run-dir", type=str, help="Path to a specific run directory")
    p_report.add_argument("--latest", action="store_true", help="Use the most recent run")
    p_report.add_argument("--dry-run", action="store_true", help="Detect and dedup but don't file issues")

    subparsers.add_parser("list", help="List known crashes")

    args = parser.parse_args()
    if args.command == "report":
        cmd_report(args)
    elif args.command == "list":
        cmd_list(args)


if __name__ == "__main__":
    main()
