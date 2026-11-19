"""Verify every selected native test shard executed on a remote worker."""

import json
import sys


def _remote_pass(result):
    execution = result.get("executionInfo", {})
    return (result.get("status") == "PASSED" and execution.get("strategy") == "remote"
            and not result.get("cachedLocally") and not execution.get("cachedRemotely"))


def _collect(events):
    members = {}
    planned = set()
    passed = set()
    for event in events:
        for item in event.get("expanded", {}).get("testSuiteExpansions", []):
            members[item["suiteLabel"]] = set(item["testLabels"])
        if "completed" in event:
            planned.update(_shard(child["testResult"]) for child in event.get("children", [])
                           if "testResult" in child)
        if "testResult" in event and _remote_pass(event["testResult"]):
            passed.add(_shard(event["id"]["testResult"]))
    return members, planned, passed


def verify(events, suites):
    members, planned, passed = _collect(events)
    expected = set()
    for suite in suites:
        if not members.get(suite):
            raise ValueError(f"No test suite expansion for {suite}")
        expected.update(members[suite])
    canary = {"//tools/ci:ci_linux_kernel_test", "//tools/ci:ci_macos_kernel_test",
              "//donner/base:ci_linux_ndebug_test", "//donner/base:ci_macos_ndebug_test"}
    if members.get("//tools/ci:dual_platform_canary") != canary:
        raise ValueError("Missing native platform canary coverage")
    labels = {shard[0] for shard in planned}
    if labels != expected or planned != passed:
        raise ValueError(f"Missing tests {sorted(expected - labels)}, unexpected tests "
                         f"{sorted(labels - expected)}, incomplete remote shards {sorted(planned - passed)}")
    print(f"{len(expected)} native tests passed across {len(passed)} uncached remote shards")


def _shard(identity):
    return (identity["label"], identity["configuration"]["id"], identity["run"], identity["shard"])


if __name__ == "__main__":
    with open(sys.argv[1], encoding="utf-8") as stream:
        verify((json.loads(line) for line in stream), sys.argv[2:])
