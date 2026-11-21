#!/usr/bin/env python3
"""Select fail-closed remote wrappers for CI-parallel GPU test suites."""

import argparse
import json
import os
import sys


REMOTE_TARGETS = {
    "//donner/editor/tests:rnr_replay_tests_geode":
        "//donner/editor/tests:rnr_replay_tests_geode_ci_remote",
    "//donner/svg/renderer/tests:resvg_test_suite_geode":
        "//donner/svg/renderer/tests:resvg_test_suite_geode_ci_remote",
    "//donner/svg/renderer/tests:resvg_test_suite_geode_max_text":
        "//donner/svg/renderer/tests:resvg_test_suite_geode_max_text_ci_remote",
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


def linux_native_patterns(lines, hardware_gpu=False):
    """Build the selected Linux rules, running only the tests selected on the trusted lane."""
    labels = set()
    for line in lines:
        rule = json.loads(line)["rule"]
        label = rule["name"]
        tags = next((item.get("stringListValue", []) for item in rule.get("attribute", [])
                     if item["name"] == "tags"), [])
        if hardware_gpu and "ci-linux-hardware-gpu-source" in tags:
            package, name = label.split(":", 1)
            labels.add(f"{package}:ci_linux_gpu_{name}")
        if set(tags).intersection(("perf", "local-gpu-isolated", "fuzz_soak")):
            package, name = label.split(":", 1)
            labels.add(f"{package}:ci_linux_build_{name}")
            if label in REMOTE_TARGETS:
                labels.add(REMOTE_TARGETS[label])
        else:
            labels.add(label)
    return sorted(labels)


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


SOAK = "//build_defs:soak_probe_fuzzer_soak"
REFERENCE = "//donner/svg/renderer/tests:resvg_test_suite_wgpu_reference_linux"
AUDIT = "//donner/svg/renderer/tests:resvg_wgpu_reference_dependency_audit_test"


def _fixed_result(label, result, shard, observed):
    if label not in (SOAK, REFERENCE, AUDIT):
        return
    if result["status"] != "PASSED":
        raise ValueError(f"{label} did not pass")
    if label == SOAK and (result.get("cachedLocally") or result.get("executionInfo", {}).get("cachedRemotely")):
        raise ValueError(f"{SOAK} must execute uncached")
    observed.setdefault(label, set()).add(shard)


def _verify_fixed(observed, reference, reference_only):
    expected = {} if reference_only else {SOAK: {1}}
    if reference:
        expected.update({REFERENCE: set(range(1, 17)), AUDIT: {1}})
    if observed != expected:
        raise ValueError(f"Fixed Linux test results differ: expected {expected}, got {observed}")


def _observed_results(events, remote):
    seen = set()
    observed = {}
    finished = False
    for line in events:
        event = json.loads(line)
        finished |= "finished" in event
        if "testResult" in event:
            identifier = event["id"]["testResult"]
            label = identifier["label"]
            result = event["testResult"]
            if remote:
                _check_result(label, result)
                if label in DISPLAY_TESTS:
                    seen.add(label)
            _fixed_result(label, result, identifier.get("shard", 1), observed)
    if not finished:
        raise ValueError("Missing Bazel build-finished event")
    return seen, observed


def check_results(events, selection, *, remote=True, fixed=False, reference=False, reference_only=False):
    """Verify the main graph's execution venues and selected fixed test results."""
    seen, observed = _observed_results(events, remote)
    if remote and not reference_only:
        expected = DISPLAY_TESTS if "//..." in selection else DISPLAY_TESTS.intersection(selection)
        if missing := expected - seen:
            raise ValueError("Missing display test results for " + ", ".join(sorted(missing)))
        print(f"Verified test execution routes, including {len(seen)} fresh local display tests.")
        for label in sorted(seen):
            print(f"  {label} passed locally, uncached")
    if fixed:
        _verify_fixed(observed, reference, reference_only)
        if SOAK in observed:
            print(f"{SOAK} passed uncached")
        if reference:
            print(f"{REFERENCE} passed all 16 shards; {AUDIT} passed")


def reference_reported(path):
    """A failed result is already evidence. Retry only if a selected shard never reported."""
    observed = {}
    try:
        with open(path, encoding="utf-8") as lines:
            for line in lines:
                event = json.loads(line)
                test = event.get("id", {}).get("testResult", {})
                if "testResult" in event and test.get("label") in (REFERENCE, AUDIT):
                    observed.setdefault(test["label"], set()).add(test.get("shard", 1))
    except (FileNotFoundError, ValueError):
        return False
    return observed.get(REFERENCE) == set(range(1, 17)) and observed.get(AUDIT) == {1}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("labels", nargs="*")
    parser.add_argument("--one-per-line", action="store_true")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--native-linux-patterns", action="store_true")
    mode.add_argument("--check-results", metavar="BEP")
    mode.add_argument("--check-hosted-results", metavar="BEP")
    mode.add_argument("--reference-reported", metavar="BEP")
    mode.add_argument("--check-reference-results", metavar="BEP")
    args = parser.parse_args()
    if args.native_linux_patterns:
        for label in linux_native_patterns(sys.stdin, os.environ.get("DONNER_CI_LINUX_HARDWARE_GPU") == "true"):
            print(label)
        return
    if args.reference_reported:
        raise SystemExit(0 if reference_reported(args.reference_reported) else 1)
    if args.check_reference_results:
        with open(args.check_reference_results, encoding="utf-8") as events:
            check_results(events, [], remote=False, fixed=True, reference=True, reference_only=True)
        return
    if args.check_results or args.check_hosted_results:
        reference_only = os.environ.get("LINUX_REFERENCE_ONLY") == "true"
        with open(args.check_results or args.check_hosted_results, encoding="utf-8") as events:
            check_results(events, args.labels, remote=bool(args.check_results), fixed=True,
                          reference=os.environ.get("WGPU_REFERENCE") == "true", reference_only=reference_only)
        if not reference_only and os.environ.get("GITHUB_OUTPUT"):
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                print("deterministic_soak_executed=true", file=output)
        return
    selected = select_remote_targets(args.labels)
    if not selected:
        return
    separator = "\n" if args.one_per_line else " "
    print(separator.join(selected))


if __name__ == "__main__":
    main()
