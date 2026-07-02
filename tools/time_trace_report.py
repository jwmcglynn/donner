#!/usr/bin/env python3
"""Aggregate Clang -ftime-trace output from a Bazel output tree.

Companion to `--config=time-trace` (see .bazelrc). After

    bazel build --config=time-trace //donner/svg/...

each compiled object has a Chrome-trace JSON next to it under bazel-out.
This tool aggregates those traces and prints markdown tables of:

  * top translation units by ExecuteCompiler time (frontend vs backend split),
  * top repeated header/source parse costs (Source events, summed per file),
  * top template instantiation costs (InstantiateClass/InstantiateFunction),
  * total time by Clang phase (Frontend / PerformPendingInstantiations /
    Backend / ...).

Raw traces stay local — only this summary is meant to be persisted. Measure
first; keep raw trace artifacts out of git.

Usage:
    python3 tools/time_trace_report.py [--top N] [trace_dir ...]

With no trace_dir arguments, scans bazel-out/*/bin for *_objs/**/*.json.
"""

import argparse
import collections
import glob
import json
import os
import sys


def find_trace_files(roots):
    if roots:
        for root in roots:
            yield from glob.iglob(os.path.join(root, "**", "*.json"),
                                  recursive=True)
        return
    for pattern in ("bazel-out/*/bin",):
        for bin_dir in glob.iglob(pattern):
            yield from glob.iglob(
                os.path.join(bin_dir, "**", "_objs", "**", "*.json"),
                recursive=True)


def load_events(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return json.load(f).get("traceEvents", [])
    except (json.JSONDecodeError, OSError):
        return []


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("trace_dirs", nargs="*")
    parser.add_argument("--top", type=int, default=20)
    args = parser.parse_args()

    # Per-TU rows: (execute_us, frontend_us, backend_us, tu_name)
    tu_rows = []
    # Aggregations across all TUs.
    source_cost = collections.Counter()  # header/source path -> total us
    source_count = collections.Counter()  # header/source path -> parse count
    instantiation_cost = collections.Counter()  # symbol -> total us
    phase_cost = collections.Counter()  # phase event name -> total us

    trace_files = sorted(set(find_trace_files(args.trace_dirs)))
    if not trace_files:
        print("No time-trace JSON files found. Build with "
              "`--config=time-trace` first.", file=sys.stderr)
        return 1

    for path in trace_files:
        events = load_events(path)
        if not events:
            continue
        execute = frontend = backend = 0
        for e in events:
            if e.get("ph") != "X":
                continue
            name, dur = e.get("name", ""), e.get("dur", 0)
            detail = e.get("args", {}).get("detail", "")
            if name == "ExecuteCompiler":
                execute = max(execute, dur)
            elif name == "Frontend":
                frontend += dur
            elif name == "Backend":
                backend += dur
            elif name == "Source" and detail:
                source_cost[detail] += dur
                source_count[detail] += 1
            elif name in ("InstantiateClass", "InstantiateFunction") and detail:
                instantiation_cost[detail] += dur
            if name in ("Frontend", "Backend", "Source", "ParseClass",
                        "InstantiateClass", "InstantiateFunction",
                        "PerformPendingInstantiations", "CodeGenPasses",
                        "OptModule"):
                phase_cost[name] += dur
        if execute:
            tu = os.path.relpath(path).replace(".json", ".o")
            tu_rows.append((execute, frontend, backend, tu))

    print(f"# Clang -ftime-trace summary ({len(tu_rows)} translation units)\n")

    print("## Total time by Clang phase\n")
    print("| phase | total |")
    print("|---|---:|")
    for name, us in phase_cost.most_common():
        print(f"| {name} | {us / 1e6:.1f}s |")

    print(f"\n## Top {args.top} translation units by ExecuteCompiler\n")
    print("| total | frontend | backend | object |")
    print("|---:|---:|---:|---|")
    for execute, frontend, backend, tu in sorted(tu_rows, reverse=True)[:args.top]:
        print(f"| {execute / 1e6:.1f}s | {frontend / 1e6:.1f}s "
              f"| {backend / 1e6:.1f}s | {tu} |")

    print(f"\n## Top {args.top} header/source parse costs (summed over TUs)\n")
    print("| total | parses | file |")
    print("|---:|---:|---|")
    for path, us in source_cost.most_common(args.top):
        print(f"| {us / 1e6:.1f}s | {source_count[path]} | {path} |")

    print(f"\n## Top {args.top} template instantiations (summed over TUs)\n")
    print("| total | symbol |")
    print("|---:|---|")
    for symbol, us in instantiation_cost.most_common(args.top):
        symbol = symbol.replace("|", "\\|")
        print(f"| {us / 1e6:.1f}s | `{symbol}` |")

    return 0


if __name__ == "__main__":
    sys.exit(main())
