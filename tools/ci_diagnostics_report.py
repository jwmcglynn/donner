#!/usr/bin/env python3
"""Summarize donner CI diagnostics into a compact markdown report.

Reads the artifact layout produced by the self-hosted CI jobs:

    <diag_dir>/manifest.txt
    <diag_dir>/target-list.txt
    <diag_dir>/{build,test}/profile.gz   Bazel --profile (Chrome trace JSON)
    <diag_dir>/{build,test}/bep.json     Bazel --build_event_json_file
    <diag_dir>/{build,test}/console.log  Bazel stdout/stderr

and prints markdown with phase timing, the slowest compile/link/test actions,
per-test wall times, and console tails for incomplete Bazel event streams.
Stdlib only; safe to run on partial artifacts (a failed build uploads whatever
exists).

Usage: python3 tools/ci_diagnostics_report.py <diag_dir>
"""

import json
import os
import sys
import zlib
from collections import deque

TOP_N = 20
CONSOLE_TAIL_LINES = 80


def load_profile_events(path):
    """Return the list of Chrome-trace events from a Bazel --profile file.

    Tolerates a TRUNCATED profile. This report exists for runs that went wrong,
    and the worst of those are killed by the job timeout while Bazel still has
    the profile open, which leaves the gzip stream without its end-of-stream
    marker. A plain `json.load` then raises EOFError, the caller's `|| true`
    swallows it, and the run that most needed a report produced an empty one
    (observed on every self-hosted run that hit the 210-minute backstop).

    Bazel writes one JSON object per line inside `traceEvents`, so a truncated
    file is still a readable prefix: decompress whatever is intact and parse
    line by line, discarding only the final partial record.
    """
    raw = _read_possibly_truncated(path)
    try:
        return json.loads(raw).get("traceEvents", [])
    except ValueError:
        pass

    events = []
    for line in raw.split("\n"):
        line = line.strip().rstrip(",")
        if line.startswith("{") and line.endswith("}"):
            try:
                events.append(json.loads(line))
            except ValueError:
                continue
    return events


def _read_possibly_truncated(path):
    """Decompress as much of `path` as is intact and return it as text."""
    if not path.endswith(".gz"):
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()

    # zlib's incremental decompressor keeps everything it produced before it
    # ran out of input; gzip.read() discards the partial chunk it was building.
    decompressor = zlib.decompressobj(16 + zlib.MAX_WBITS)
    out = bytearray()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            try:
                out += decompressor.decompress(chunk)
            except zlib.error:
                break
    return out.decode("utf-8", errors="replace")


def fmt_secs(us):
    return f"{us / 1e6:.1f}s"


def profile_summary(name, path, lines):
    if not os.path.exists(path):
        lines.append(f"_No {name} profile captured._")
        return

    events = load_profile_events(path)
    complete = [e for e in events if e.get("ph") == "X" and "dur" in e]

    # Bazel emits one span per lifecycle phase under this category.
    phases = [e for e in complete if e.get("cat") == "build phase marker"]
    if phases:
        lines.append(f"**{name} phases:** " + ", ".join(
            f"{e['name']}={fmt_secs(e['dur'])}"
            for e in sorted(phases, key=lambda e: e.get("ts", 0))))

    actions = [e for e in complete if e.get("cat") == "action processing"]
    total_action_us = sum(e["dur"] for e in actions)
    lines.append(
        f"**{name}:** {len(actions)} actions, "
        f"{total_action_us / 1e6:.0f}s total action time")

    def top(title, predicate):
        rows = sorted((e for e in actions if predicate(e["name"])),
                      key=lambda e: -e["dur"])[:TOP_N]
        if not rows:
            return
        lines.append("")
        lines.append(f"**{title} ({name}):**")
        lines.append("| duration | action |")
        lines.append("|---:|---|")
        for e in rows:
            lines.append(f"| {fmt_secs(e['dur'])} | {e['name']} |")

    top("Slowest compiles", lambda n: n.startswith("Compiling"))
    top("Slowest links", lambda n: n.startswith(("Linking", "Archiving")))
    top("Slowest other actions",
        lambda n: not n.startswith(("Compiling", "Linking", "Archiving",
                                    "Testing")))
    lines.append("")


def has_build_finished(path):
    if not os.path.exists(path):
        return False
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if event.get("id", {}).get("buildFinished") is not None:
                return True
    return False


def console_tail_summary(name, bep_path, console_path, lines):
    if not os.path.exists(console_path) or has_build_finished(bep_path):
        return

    tail = deque(maxlen=CONSOLE_TAIL_LINES)
    with open(console_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            tail.append(line.rstrip("\n"))

    if not tail:
        return

    lines.append(
        f"**{name} console tail (no terminal Bazel build event captured):**")
    lines.append("```")
    lines.extend(tail)
    lines.append("```")
    lines.append("")


def unfinished_tests_summary(path, lines):
    """Name every test target the invocation configured but never finished.

    This is the section that turns a silent stall into a diagnosis. A test that
    is queued and never scheduled produces a `targetConfigured` event and no
    `testSummary`, and no timeout ever fires against it, because per-test
    timeouts only start counting once a test RUNS. Bazel's console shows only
    the one target it happens to be printing. Listing the whole unfinished set
    is what distinguishes "one target is stuck" from "the executor is wedged"
    without needing live access to the runner.

    Targets that are simply incompatible with the host or tagged `manual` also
    land here, so the list is a starting point, not a verdict - but when a run
    dies at its job backstop, this is the shortest path to the culprit.
    """
    if not os.path.exists(path):
        return
    configured = set()
    finished = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            event_id = event.get("id", {})
            target = event_id.get("targetConfigured")
            if target and event.get("configured", {}).get("testSize"):
                configured.add(target.get("label", "?"))
            summary = event_id.get("testSummary")
            if summary:
                finished.add(summary.get("label", "?"))

    unfinished = sorted(configured - finished)
    if not unfinished:
        return
    lines.append(
        f"**Configured but never finished ({len(unfinished)} test targets):**")
    lines.append("")
    lines.append("These produced no test summary. A target that is queued but "
                 "never scheduled looks exactly like this, and no per-test "
                 "timeout applies to it.")
    lines.append("")
    for label in unfinished[:TOP_N * 5]:
        lines.append(f"- `{label}`")
    if len(unfinished) > TOP_N * 5:
        lines.append(f"- _...and {len(unfinished) - TOP_N * 5} more_")
    lines.append("")


def test_summary(path, lines):
    if not os.path.exists(path):
        return
    durations = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            test_id = event.get("id", {}).get("testResult")
            if not test_id:
                continue
            millis = int(
                event.get("testResult", {}).get("testAttemptDurationMillis", 0)
                or 0)
            if not millis:
                # Bazel >=7 uses testAttemptDuration ("123.4s" proto duration).
                duration = event.get("testResult", {}).get(
                    "testAttemptDuration", "0s")
                try:
                    millis = int(float(str(duration).rstrip("s")) * 1000)
                except ValueError:
                    millis = 0
            durations.append(
                (millis, test_id.get("label", "?"), test_id.get("shard", 0)))
    if not durations:
        return
    lines.append(f"**Slowest test executions (top {TOP_N}):**")
    lines.append("| duration | test | shard |")
    lines.append("|---:|---|---:|")
    for millis, label, shard in sorted(durations, reverse=True)[:TOP_N]:
        lines.append(f"| {millis / 1000:.1f}s | {label} | {shard} |")
    lines.append("")


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 1
    diag_dir = sys.argv[1]
    lines = ["## CI diagnostics report", ""]

    manifest = os.path.join(diag_dir, "manifest.txt")
    if os.path.exists(manifest):
        with open(manifest, encoding="utf-8") as f:
            lines.append("```")
            lines.append(f.read().strip())
            lines.append("```")
        lines.append("")

    profile_summary("Build", os.path.join(diag_dir, "build", "profile.gz"),
                    lines)
    console_tail_summary("Build", os.path.join(diag_dir, "build", "bep.json"),
                         os.path.join(diag_dir, "build", "console.log"), lines)
    profile_summary("Test", os.path.join(diag_dir, "test", "profile.gz"),
                    lines)
    console_tail_summary("Test", os.path.join(diag_dir, "test", "bep.json"),
                         os.path.join(diag_dir, "test", "console.log"), lines)
    unfinished_tests_summary(os.path.join(diag_dir, "test", "bep.json"), lines)
    test_summary(os.path.join(diag_dir, "test", "bep.json"), lines)

    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
