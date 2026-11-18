#!/usr/bin/env python3
"""Select fail-closed remote wrappers for CI-parallel GPU test suites."""

import argparse
import json


REMOTE_TARGETS = {
    "//donner/editor/tests:rnr_replay_tests_geode":
        "//donner/editor/tests:rnr_replay_tests_geode_ci_remote",
    "//donner/svg/renderer/tests:resvg_test_suite_geode":
        "//donner/svg/renderer/tests:resvg_test_suite_geode_ci_remote",
}


def select_remote_targets(labels):
    """Replace opted-in local-isolation wrappers and preserve stable order."""
    result = []
    seen = set()
    for label in labels:
        selected = REMOTE_TARGETS.get(label, label)
        if selected not in seen:
            result.append(selected)
            seen.add(selected)
    return result


# These test rules own the runner's Xvfb server and local GPU resource.
DISPLAY_TESTS = {
    "//donner/editor/tests:editor_window_vulkan_default_tests",
    "//donner/editor/tests:editor_window_vulkan_surface_tests",
}


def _check_result(label, result):
    info = result.get("executionInfo", {})
    strategy = info.get("strategy")
    cached = result.get("cachedLocally") or info.get("cachedRemotely")
    if label in DISPLAY_TESTS:
        if cached or strategy != "local" or result.get("status") != "PASSED":
            raise ValueError(f"{label} must pass uncached on the local display")
    elif not cached and strategy != "remote":
        raise ValueError(f"{label} used {strategy!r} instead of remote execution")


def check_results(events, selection):
    """Check each executed test's venue and require the selected display results."""
    expected = DISPLAY_TESTS if "//..." in selection else DISPLAY_TESTS.intersection(selection)
    seen = set()
    finished = False
    for line in events:
        event = json.loads(line)
        finished |= "finished" in event
        if "testResult" in event:
            label = event["id"]["testResult"]["label"]
            _check_result(label, event["testResult"])
            if label in DISPLAY_TESTS:
                seen.add(label)
    if not finished:
        raise ValueError("Missing Bazel build-finished event")
    if missing := expected - seen:
        raise ValueError("Missing display test results for " + ", ".join(sorted(missing)))
    print(f"Verified test execution routes, including {len(seen)} fresh local display tests.")
    for label in sorted(seen):
        print(f"  {label} passed locally, uncached")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("labels", nargs="*")
    parser.add_argument("--one-per-line", action="store_true")
    parser.add_argument("--check-results", metavar="BEP")
    args = parser.parse_args()
    if args.check_results:
        with open(args.check_results, encoding="utf-8") as events:
            check_results(events, args.labels)
        return
    selected = select_remote_targets(args.labels)
    if not selected:
        return
    separator = "\n" if args.one_per_line else " "
    print(separator.join(selected))


if __name__ == "__main__":
    main()
