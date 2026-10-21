#!/usr/bin/env python3
"""Runs one Fuzz workflow phase and fails unless every test result came from this run.

A fuzz soak is time-bounded mutation: its result covers only what that run explored. A result
replayed from Bazel's local action cache, disk cache or remote cache did no fuzzing, yet Bazel
prints it as `(cached) PASSED` and the phase still succeeds. The fuzz lane configs in .bazelrc
disable test result caching. This lane also reads the phase's Build Event Protocol file and fails
when any test result was served from a cache, or when no test ran at all, so a lane that loses
that setting fails the workflow instead of passing on earlier results.

Usage: tools/ci/fuzz_lane.py LANE PATTERN...
  Runs `bazelisk test --config=LANE -- PATTERN...` from the repository root, then checks its
  results.
"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def test_results(lines):
    """(label, cached) for every testResult event in Build Event Protocol JSON lines."""
    results = []
    for number, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError("line %d is not a build event: %s" % (number, error)) from None
        result_id = event.get("id", {}).get("testResult")
        if result_id is None:
            continue
        result = event.get("testResult", {})
        cached = bool(result.get("cachedLocally") or
                      result.get("executionInfo", {}).get("cachedRemotely"))
        results.append((result_id.get("label", "<unknown>"), cached))
    return results


def problems(lines):
    """The ways a phase's build events fail to show that every test executed in this run."""
    results = test_results(lines)
    if not results:
        return ["the phase reported no test results, so no soak ran"]
    return ["%s was served from a cache instead of executing" % label
            for label in sorted({label for label, cached in results if cached})]


def main(argv):
    if len(argv) < 2 or argv[0].startswith("-") or any(
            argument.startswith("--") for argument in argv[1:]):
        print(__doc__.strip().split("\n\n")[-1], file=sys.stderr)
        return 2
    lane, patterns = argv[0], argv[1:]
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="fuzz-lane-") as directory:
        events = Path(directory) / "build_events.json"
        command = ["bazelisk", "test", "--config=" + lane,
                   "--build_event_json_file=" + str(events), "--", *patterns]
        status = subprocess.run(command, cwd=root, check=False).returncode
        if status != 0:
            return status
        try:
            found = problems(events.read_text(encoding="utf-8").splitlines())
        except (OSError, ValueError) as error:
            found = ["cannot read the phase's build events: %s" % error]
    for problem in found:
        print("::error::%s: %s" % (lane, problem))
    if found:
        print("A fuzz soak result must come from this run; check that the %s config in .bazelrc "
              "keeps --nocache_test_results." % lane, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
