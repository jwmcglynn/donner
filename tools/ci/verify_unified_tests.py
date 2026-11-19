"""Verify scheduled targets and test shards in a mixed-platform build event stream."""

import argparse
from collections import defaultdict
import json
from pathlib import Path
from urllib.parse import unquote, urlsplit

from macos_remote_shadow import MAX_ARTIFACT_TOTAL_BYTES, stage_failed_test_outputs


CANARY = {"//tools/ci:ci_linux_kernel_test", "//tools/ci:ci_macos_kernel_test",
          "//donner/base:ci_linux_ndebug_test", "//donner/base:ci_macos_ndebug_test"}


def _shard(identity):
    return (identity["label"], identity["configuration"]["id"], identity["run"], identity["shard"])


def _passed_remotely(result):
    execution = result.get("executionInfo", {})
    cached = result.get("cachedLocally") or execution.get("cachedRemotely")
    if result.get("status") != "PASSED":
        return None
    if cached:
        return "cached"
    return "remote" if execution.get("strategy") == "remote" else None


def _target_outcome(event):
    if event.get("completed", {}).get("success"):
        return "built"
    if event.get("aborted", {}).get("reason") == "SKIPPED":
        return "incompatible"
    return "failed"


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


KERNELS = {"//tools/ci:ci_linux_kernel_test": "Linux aarch64",
           "//tools/ci:ci_macos_kernel_test": "Darwin arm64"}


def report_kernels(locations, output_root):
    root = output_root.resolve(strict=True)
    for label, kernel in KERNELS.items():
        location = urlsplit(locations.get(label, ""))
        if location.scheme != "file" or location.netloc:
            raise ValueError(f"Missing local probe log for {label}")
        path = Path(unquote(location.path)).resolve(strict=True)
        suffix = ("testlogs", "tools", "ci", label.split(":")[1], "test.log")
        if not path.is_relative_to(root) or path.parts[-5:] != suffix:
            raise ValueError(f"Unexpected probe log location for {label}")
        marker = "test worker: " + kernel
        with path.open("rb") as stream:
            body = stream.read(65537)
        if len(body) > 65536 or marker.encode() not in body.splitlines():
            raise ValueError(f"Missing kernel and architecture evidence for {label}")
        print(marker)


class Evidence:
    def __init__(self):
        self.members = {}
        self.targets = {}
        self.summaries = {}
        self.passed = defaultdict(set)
        self.test_rules = set()
        self.kernel_logs = {}

    def read(self, event):
        identity = event.get("id", {})
        for item in event.get("expanded", {}).get("testSuiteExpansions", []):
            self.members[item["suiteLabel"]] = set(item["testLabels"])
        if "targetConfigured" in identity and "testSize" in event.get("configured", {}):
            self.test_rules.add(identity["targetConfigured"]["label"])
        if "targetCompleted" in identity:
            self.targets[identity["targetCompleted"]["label"]] = _target_outcome(event)
        if "testSummary" in event:
            key = identity["testSummary"]
            self.summaries[key["label"], key["configuration"]["id"]] = event["testSummary"]
        if "testResult" in event:
            result = event["testResult"]
            shard = _shard(identity["testResult"])
            self.passed[_passed_remotely(result)].add(shard)
            if shard[0] in KERNELS:
                self.kernel_logs[shard[0]] = next((f.get("uri", "") for f in result.get("testActionOutput", [])
                                                   if f.get("name") == "test.log"), "")


BROWSER_PACKAGE = "//donner/editor/wasm/tests:ci_darwin_"
ARCHIVE = BROWSER_PACKAGE + "mac_browser_archive_tests"
NATIVE_BROWSER = {ARCHIVE, BROWSER_PACKAGE + "animation_preview_browser_test",
                  BROWSER_PACKAGE + "canvas_snapshot_browser_test"}
QUALIFY = "//tools/ci:unified_worker_qualification"


def _ordinary_built(evidence, selected):
    missing = {label for label in selected if evidence.targets.get(label) not in ("built", "incompatible")}
    compatible = {label for label in selected if evidence.targets.get(label) == "built"}
    missing.update(selected & NATIVE_BROWSER - compatible)
    if missing:
        raise ValueError(f"Darwin target did not finish: {sorted(missing)}")
    return compatible


def _qualification_runs(evidence):
    for label in evidence.members.get(QUALIFY, ()):
        if {run for target, _, run, _ in evidence.passed["remote"] if target == label} != {1, 2, 3, 4}:
            raise ValueError(f"Expected four uncached remote runs for {label}")


def verify(events, suites, ordinary=(), output_root=None):
    evidence = Evidence()
    for event in events:
        evidence.read(event)
    if evidence.members.get("//tools/ci:dual_platform_canary") != CANARY:
        raise ValueError("Missing native platform canary coverage")
    mandatory, fresh = _suite_requirements(evidence.members, suites)
    selected = set(ordinary)
    compatible = _ordinary_built(evidence, selected)
    fresh.update(selected & {ARCHIVE})
    _qualification_runs(evidence)
    expected = mandatory | (compatible & evidence.test_rules)
    summary_labels = {label for label, _ in evidence.summaries}
    passed = evidence.passed["remote"] | evidence.passed["cached"]
    _check_summary_shards(evidence.summaries, passed)
    cached_required = {shard for shard in evidence.passed["cached"] if shard[0] in fresh}
    if summary_labels != expected or cached_required or evidence.passed[None]:
        raise ValueError(f"Test label mismatch {sorted(expected ^ summary_labels)}, "
                         f"incomplete remote shards {sorted(cached_required | evidence.passed[None])}")
    if output_root is not None:
        report_kernels(evidence.kernel_logs, output_root)
    print(f"{len(expected)} native tests passed across {len(evidence.passed['remote'])} uncached "
          f"remote shards and {len(evidence.passed['cached'])} cached shards; "
          f"{len(compatible)} ordinary Darwin roots built, {len(selected - compatible)} incompatible")


def _suite_requirements(members, suites):
    mandatory = set()
    fresh = set()
    for suite in suites:
        if not members.get(suite):
            raise ValueError(f"No test suite expansion for {suite}")
        mandatory.update(members[suite])
        if suite != "//tools/ci:unified_editor_geode":
            fresh.update(members[suite])
    return mandatory, fresh


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ordinary", type=Path)
    parser.add_argument("bep", type=Path)
    parser.add_argument("--stage-failed", type=Path)
    parser.add_argument("suites", nargs="*")
    args = parser.parse_args()
    if args.stage_failed:
        if args.bep.is_file():
            with args.bep.open(encoding="utf-8") as stream:
                stage_failed_test_outputs(stream, args.stage_failed, "mixed", Path("bazel-out"), MAX_ARTIFACT_TOTAL_BYTES)
        else:
            stage_failed_test_outputs((), args.stage_failed, "mixed", Path("bazel-out"), MAX_ARTIFACT_TOTAL_BYTES)
        raise SystemExit(0)
    if not args.suites:
        parser.error("at least one test suite is required")
    ordinary_roots = args.ordinary.read_text().splitlines() if args.ordinary else []
    with args.bep.open(encoding="utf-8") as stream:
        verify((json.loads(line) for line in stream), args.suites, ordinary_roots, Path("bazel-out"))
