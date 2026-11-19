"""Verify every selected native test shard executed on a remote worker."""

from collections import defaultdict
import json
import sys


def _remote_pass(result):
    execution = result.get("executionInfo", {})
    return (result.get("status") == "PASSED" and execution.get("strategy") == "remote"
            and not result.get("cachedLocally") and not execution.get("cachedRemotely"))


def _check_summary_shards(summaries, passed):
    by_test = defaultdict(set)
    for label, config, run, shard in passed:
        by_test[label, config].add((run, shard))
    if summaries.keys() != by_test.keys():
        raise ValueError("incomplete remote shards or missing test summary")
    for key, summary in summaries.items():
        runs = summary["runCount"]
        # Bazel omits the default shard count for an unsharded test.
        shards = summary.get("shardCount") or 1
        seen = by_test[key]
        if (summary["overallStatus"] != "PASSED" or len(seen) != runs * shards
                or any(not (1 <= run <= runs and 1 <= shard <= shards) for run, shard in seen)):
            raise ValueError(f"incomplete remote shards for {key}: expected {runs} runs of {shards} shards")


def _collect(events):
    members = {}
    summaries = {}
    passed = set()
    for event in events:
        for item in event.get("expanded", {}).get("testSuiteExpansions", []):
            members[item["suiteLabel"]] = set(item["testLabels"])
        if "testSummary" in event:
            identity = event["id"]["testSummary"]
            summaries[identity["label"], identity["configuration"]["id"]] = event["testSummary"]
        if "testResult" in event and _remote_pass(event["testResult"]):
            passed.add(_shard(event["id"]["testResult"]))
    _check_summary_shards(summaries, passed)
    return members, passed


def verify(events, suites):
    members, passed = _collect(events)
    expected = set()
    for suite in suites:
        if not members.get(suite):
            raise ValueError(f"No test suite expansion for {suite}")
        expected.update(members[suite])
    canary = {"//tools/ci:ci_linux_kernel_test", "//tools/ci:ci_macos_kernel_test",
              "//donner/base:ci_linux_ndebug_test", "//donner/base:ci_macos_ndebug_test"}
    if members.get("//tools/ci:dual_platform_canary") != canary:
        raise ValueError("Missing native platform canary coverage")
    labels = {shard[0] for shard in passed}
    if labels != expected:
        raise ValueError(f"Missing tests {sorted(expected - labels)}, unexpected tests "
                         f"{sorted(labels - expected)}")
    print(f"{len(expected)} native tests passed across {len(passed)} uncached remote shards")


def _shard(identity):
    return (identity["label"], identity["configuration"]["id"], identity["run"], identity["shard"])


if __name__ == "__main__":
    with open(sys.argv[1], encoding="utf-8") as stream:
        verify((json.loads(line) for line in stream), sys.argv[2:])
