#!/usr/bin/env python3
"""Summarize donner CI diagnostics into a compact markdown report.

Reads the artifact layout produced by the self-hosted CI jobs:

    <diag_dir>/manifest.txt
    <diag_dir>/target-list.txt
    <diag_dir>/{build,test}/profile.gz   Bazel --profile (Chrome trace JSON)
    <diag_dir>/{build,test}/bep.json     Bazel --build_event_json_file

and prints markdown with phase timing, the slowest compile/link/test actions,
and per-test wall times. Stdlib only; safe to run on partial artifacts (a
failed build uploads whatever exists).

Usage: python3 tools/ci_diagnostics_report.py <diag_dir>
"""

import gzip
import json
import os
import sys

TOP_N = 20


def load_profile_events(path):
    """Return the list of Chrome-trace events from a Bazel --profile file."""
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as f:
        data = json.load(f)
    return data.get("traceEvents", data if isinstance(data, list) else [])


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
    profile_summary("Test", os.path.join(diag_dir, "test", "profile.gz"),
                    lines)
    test_summary(os.path.join(diag_dir, "test", "bep.json"), lines)

    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
