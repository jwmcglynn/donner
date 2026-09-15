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
import base64
import socket
import tempfile
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

from execution import fuzzer_command

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
        self.confirmed: bool = False
        self.isolated: bool = False
        self.input_bytes: bytes = b""
        self.binary_sha256: str = ""

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
        if "ERROR:" in line and (any(name in line for name in ("AddressSanitizer", "UndefinedBehaviorSanitizer", "LeakSanitizer", "libFuzzer"))):
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
    """Replay one input; successful exits and tool failures are never crash evidence."""
    try:
        with tempfile.TemporaryFile() as stderr:
            proc = subprocess.run(
                fuzzer_command(binary_path, ["-runs=1", "-timeout=10", "-rss_limit_mb=2048", str(crash_file)],
                               read_only=[crash_file.parent]),
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=stderr, timeout=timeout,
            )
            if proc.returncode == 0:
                return ""
            stderr.seek(0)
            return stderr.read(262144).decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired:
        return "(reproduction timed out)"
    except (OSError, ValueError, RuntimeError):
        return "(reproduction failed)"


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
    """Atomically persist the ledger before and after an outward submission."""
    KNOWN_CRASHES_FILE.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=KNOWN_CRASHES_FILE.parent,
                                     prefix=".crashes-", delete=False) as f:
        temporary = Path(f.name)
        json.dump(crashes, f, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(temporary, KNOWN_CRASHES_FILE)


# ---------------------------------------------------------------------------
# GitHub Issue filing
# ---------------------------------------------------------------------------

MAX_REPRO_BYTES = 24 * 1024
REPO = "jwmcglynn/donner"
LABEL_RE = re.compile(r"//donner/[A-Za-z0-9_./-]+:[A-Za-z0-9_]+_bin")
SECRET_RE = re.compile(rb"PRIVATE KEY|github_pat_|gh[pousr]_[A-Za-z0-9]{20}|(?:api[_-]?key|token|password)\s*[=:]|Authorization:|/home/|/Users/", re.I)


def publication_input(data: bytes) -> bool:
    """Only bounded generated inputs without credential/host-path patterns can leave the host."""
    private_terms = [str(Path.home()).encode(), str(REPO_ROOT).encode(), socket.gethostname().encode()]
    return (len(data) <= MAX_REPRO_BYTES and not SECRET_RE.search(data)
            and not any(term and term in data for term in private_terms))


def safe_frames(frames: list[str]) -> list[str]:
    # Function identities are sufficient for deduplication. Omit paths, addresses,
    # argument values, raw input excerpts and arbitrary sanitizer diagnostics.
    return [frame.split("(", 1)[0][:180] for frame in frames[:5]
            if re.fullmatch(r"[A-Za-z0-9_:<>,~* &().+-]+", frame)]


def crash_signal(text: str) -> str:
    match = re.search(r"(?:AddressSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|libFuzzer): ([a-z][a-z-]+)", text)
    return match.group(1) if match else ""


def render_issue(crash: CrashInfo) -> tuple[str, str]:
    if not (crash.confirmed and crash.isolated and LABEL_RE.fullmatch(crash.fuzzer_label)
            and crash.fuzzer_label.split(":")[-1].removesuffix("_bin") == crash.fuzzer_name
            and re.fullmatch(r"[0-9a-f]{40}", crash.commit)
            and re.fullmatch(r"[0-9a-f]{16}", crash.signature)
            and re.fullmatch(r"[0-9a-f]{64}", crash.binary_sha256)
            and publication_input(crash.input_bytes)):
        raise ValueError("report lacks verified, publishable evidence")
    frames = safe_frames(crash.stack_frames)
    signal = crash_signal(crash.signal)
    if not frames or not signal:
        raise ValueError("report lacks a recognized sanitizer failure")
    title = f"Fuzzing crash: {crash.fuzzer_name} - {signal}"[:120]
    marker = f"<!-- donner-fuzz:v2:{crash.signature} -->"
    encoded = base64.b64encode(crash.input_bytes).decode("ascii")
    digest = hashlib.sha256(crash.input_bytes).hexdigest()
    package, name = crash.fuzzer_label[2:].split(":")
    body = f"""A sanitizer failure reproduced twice with the same binary and input.

{marker}

- Source: `{crash.commit}`
- Fuzzer: `{crash.fuzzer_label}`
- Finding: `{signal}`
- Binary SHA-256: `{crash.binary_sha256}`
- Input SHA-256: `{digest}` ({len(crash.input_bytes)} bytes)

### Stack signature
```text
{chr(10).join(frames)}
```

### Reproduce
Check out the source revision above, then:
```sh
python3 -c 'import base64; open("repro.bin", "wb").write(base64.b64decode("{encoded}"))'
bazel build --config=asan-fuzzer {crash.fuzzer_label}
./bazel-bin/{package}/{name} -runs=1 -timeout=10 -rss_limit_mb=2048 repro.bin
```

Keep the reproducer as a corpus regression when fixing the bug. Source paths,
host details, raw process output and environment values are omitted.
"""
    return title, body


def find_existing_issue(signature: str) -> Optional[str]:
    """Check GitHub as well as the local ledger; an unavailable lookup fails closed."""
    marker = f"donner-fuzz:v2:{signature}"
    proc = subprocess.run(
        ["gh", "issue", "list", "--repo", REPO, "--state", "all", "--limit", "100",
         "--search", f'"{marker}" in:body', "--json", "url,body"],
        capture_output=True, text=True, timeout=30,
    )
    if proc.returncode != 0 or len(proc.stdout) > 2 * 1024 * 1024:
        raise RuntimeError("GitHub duplicate lookup unavailable")
    items = json.loads(proc.stdout)
    for issue in items:
        if f"<!-- {marker} -->" in issue.get("body", ""):
            url = issue.get("url", "")
            if re.fullmatch(r"https://github.com/jwmcglynn/donner/issues/[0-9]+", url):
                return url
    return None


def file_github_issue(crash: CrashInfo, repo: str = REPO) -> Optional[str]:
    """Publish one verified report, with a durable marker against ambiguous retries."""
    try:
        title, body = render_issue(crash)
    except ValueError:
        return None
    if repo != REPO or not shutil.which("gh"):
        return None
    try:
        existing = find_existing_issue(crash.signature)
        if existing:
            return existing
        ledger = load_known_crashes()
        if ledger.get(crash.signature, {}).get("pending"):
            print("    Publication outcome uncertain; awaiting reconciliation", file=sys.stderr)
            return None
        ledger[crash.signature] = {"pending": True, "commit": crash.commit}
        save_known_crashes(ledger)
        with tempfile.NamedTemporaryFile(mode="w", suffix=".md") as f:
            f.write(body)
            f.flush()
            proc = subprocess.run(
                ["gh", "issue", "create", "--repo", REPO, "--title", title,
                 "--label", "bug", "--body-file", f.name],
                capture_output=True, text=True, timeout=30,
            )
        url = proc.stdout.strip()
        if proc.returncode == 0 and re.fullmatch(r"https://github.com/jwmcglynn/donner/issues/[0-9]+", url):
            return url
        print("    Issue creation unconfirmed; receipt retained for reconciliation", file=sys.stderr)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired):
        print("    Issue lookup/publication unavailable; no blind retry", file=sys.stderr)
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
        return result.stdout.strip() if result.returncode == 0 else "unknown"
    except Exception:
        return "unknown"


def find_crashes_in_run(run_dir: Path) -> list[tuple[str, Path]]:
    """Find all crash artifacts in a run directory.

    Returns list of (fuzzer_name, crash_file_path) tuples.
    """
    crashes = []
    for fuzzer_dir in sorted(run_dir.iterdir()):
        if fuzzer_dir.is_symlink() or not fuzzer_dir.is_dir():
            continue
        crash_dir = fuzzer_dir / "crashes"
        if crash_dir.is_symlink() or not crash_dir.is_dir():
            continue
        for f in sorted(crash_dir.iterdir()):
            if not f.is_symlink() and f.is_file() and any(
                f.name.startswith(p) for p in ("crash-", "timeout-", "oom-", "leak-")
            ):
                crashes.append((fuzzer_dir.name, f))
    return crashes


def process_crashes(run_dir: Path, targets: list[FuzzerTarget], dry_run: bool = False) -> list[CrashInfo]:
    """Replay twice; publish only source/binary-bound sanitizer evidence."""
    target_map = {t.name: t for t in targets}
    known = load_known_crashes()
    commit = get_current_commit()
    manifest = run_dir / "run_report.json"
    try:
        if manifest.is_symlink() or manifest.stat().st_size > 1024 * 1024:
            return []
        report = json.loads(manifest.read_text())
    except (OSError, ValueError):
        print("Run lacks a provenance report; publication withheld")
        return []
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or report.get("commit") != commit:
        print("Run revision differs from current source; publication withheld")
        return []
    recorded = {item.get("name"): item for item in report.get("fuzzers", [])}
    processed = []
    submitted = 0
    for fuzzer_name, crash_file in find_crashes_in_run(run_dir)[:100]:
        target = target_map.get(fuzzer_name)
        if target is None or target.binary_path is None or not LABEL_RE.fullmatch(target.label):
            continue
        if classify_crash_type(crash_file.name) not in {"crash", "leak"}:
            continue
        if crash_file.stat().st_size > MAX_REPRO_BYTES:
            continue
        data = crash_file.read_bytes()
        if not publication_input(data):
            print("Input exceeds publication boundary; retained locally")
            continue
        binary_digest = hashlib.sha256(target.binary_path.read_bytes()).hexdigest()
        saved = recorded.get(fuzzer_name, {})
        if saved.get("label") != target.label or saved.get("binary_sha256") != binary_digest:
            print("Binary differs from recorded run; publication withheld")
            continue
        first = reproduce_crash(target.binary_path, crash_file)
        trace, frames, signal = parse_stack_trace(first)
        normalized = safe_frames(frames)
        kind = crash_signal(signal)
        if not normalized or not kind:
            print("No reproducible sanitizer finding; retained locally")
            continue
        second = reproduce_crash(target.binary_path, crash_file)
        _, second_frames, second_signal = parse_stack_trace(second)
        if safe_frames(second_frames) != normalized or crash_signal(second_signal) != kind:
            print("Second replay did not confirm the finding; retained locally")
            continue
        crash = CrashInfo()
        crash.fuzzer_name = fuzzer_name
        crash.fuzzer_label = target.label
        crash.crash_file = crash_file
        crash.crash_type = classify_crash_type(crash_file.name)
        crash.binary_path = target.binary_path
        crash.binary_sha256 = binary_digest
        crash.commit = commit
        crash.stack_frames = frames
        crash.stack_trace = "\n".join(normalized)
        crash.signal = signal
        crash.input_bytes = data
        crash.confirmed = True
        crash.isolated = os.environ.get("FUZZ_SANDBOX") == "1"
        crash.signature = compute_signature([target.label, kind, *normalized], top_n=7)
        processed.append(crash)
        existing = known.get(crash.signature, {})
        if existing.get("issue_url"):
            print(f"Duplicate: {existing['issue_url']}")
            continue
        if dry_run:
            render_issue(crash)
            print(f"Verified report ready: {crash.signature}")
            continue
        if submitted >= 5:
            print("Per-run issue budget reached; remaining reports retained")
            break
        issue_url = file_github_issue(crash)
        # Reload so a pending receipt from an uncertain response is never lost.
        known = load_known_crashes()
        if issue_url:
            known[crash.signature] = {
                "fuzzer": fuzzer_name, "crash_type": crash.crash_type,
                "top_frame": normalized[0], "issue_url": issue_url,
                "commit": commit, "date": datetime.now(timezone.utc).isoformat(),
                "crash_file": str(crash_file),
            }
            save_known_crashes(known)
            submitted += 1
            print(f"Filed or reconciled: {issue_url}")
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
