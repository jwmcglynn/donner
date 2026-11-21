"""Verify scheduled targets and test shards in a mixed-platform build event stream."""

import argparse
from collections import defaultdict
import json
import os
import re
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
    return execution.get("strategy") if execution.get("strategy") in ("remote", "local") else None


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


def _declared_log(label, locations, output_root):
    location = urlsplit(locations.get(label, ""))
    if location.scheme != "file" or location.netloc:
        raise ValueError(f"Missing local probe log for {label}")
    path = Path(unquote(location.path)).resolve(strict=True)
    package, name = label.removeprefix("//").split(":", 1)
    suffix = ("testlogs", *package.split("/"), name, "test.log")
    if not path.is_relative_to(output_root.resolve(strict=True)) or path.parts[-len(suffix):] != suffix:
        raise ValueError(f"Unexpected probe log location for {label}")
    return path


def report_kernels(locations, output_root):
    for label, kernel in KERNELS.items():
        marker = "test worker: " + kernel
        with _declared_log(label, locations, output_root).open("rb") as stream:
            body = stream.read(65537)
        if len(body) > 65536 or marker.encode() not in body.splitlines():
            raise ValueError(f"Missing kernel and architecture evidence for {label}")
        print(marker)


def firefox_cases(log):
    # The pinned list reporter writes the outcome totals last, even on a long diagnostic run.
    with log.open("rb") as stream:
        end = stream.seek(0, 2)
        stream.seek(max(0, end - 65536))
        trailer = re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]", b"", stream.read(65536))
    counts = re.findall(rb"(?m)^\s*(\d+) passed(?: \([^\r\n]*\))?\s*$", trailer)
    skipped = re.search(rb"(?m)^\s*[1-9]\d* (?:skipped|did not run|failed|flaky)\b", trailer)
    if not counts or not int(counts[-1]) or skipped:
        raise ValueError("Headed Firefox did not report a complete passing case set")
    return int(counts[-1])


class Evidence:
    def __init__(self):
        self.members = {}
        self.targets = {}
        self.summaries = {}
        self.passed = defaultdict(set)
        self.test_rules = set()
        self.tags = {}
        self.test_logs = {}

    def read(self, event):
        identity = event.get("id", {})
        for item in event.get("expanded", {}).get("testSuiteExpansions", []):
            self.members[item["suiteLabel"]] = set(item["testLabels"])
        if "targetConfigured" in identity and "testSize" in event.get("configured", {}):
            label = identity["targetConfigured"]["label"]
            self.test_rules.add(label)
            self.tags[label] = set(event["configured"].get("tag", []))
        target = identity.get("targetCompleted")
        # Bazel also emits targetCompleted for aspects applied to skipped targets.
        if target and not target.get("aspect"):
            self.targets[target["label"]] = _target_outcome(event)
        if "testSummary" in event:
            key = identity["testSummary"]
            self.summaries[key["label"], key["configuration"]["id"]] = event["testSummary"]
        if "testResult" in event:
            result = event["testResult"]
            shard = _shard(identity["testResult"])
            self.passed[_passed_remotely(result)].add(shard)
            if shard[0] in KERNELS or shard[0] in HEADED or shard[0] in EDITOR_BROWSER:
                self.test_logs[shard[0]] = next((f.get("uri", "") for f in result.get("testActionOutput", [])
                                                   if f.get("name") == "test.log"), "")


BROWSER_PACKAGE = "//donner/editor/wasm/tests:ci_darwin_"
ARCHIVE = BROWSER_PACKAGE + "mac_browser_archive_tests"
HEADED = {BROWSER_PACKAGE + "animation_preview_browser_test", BROWSER_PACKAGE + "canvas_snapshot_browser_test"}
EDITOR_BROWSER = {BROWSER_PACKAGE + name for name in (
    "chromium_editor_smoke_test", "chromium_editor_presentation_test", "chromium_editor_startup_test",
    "firefox_editor_compatibility_test", "webkit_editor_carousel_test",
    "chromium_composited_invariants_test", "firefox_composited_invariants_test",
)}
NATIVE_BROWSER = {ARCHIVE} | HEADED | EDITOR_BROWSER


def editor_browser_cases(log):
    with log.open("rb") as stream:
        stream.seek(max(0, stream.seek(0, 2) - 65536))
        counts = re.findall(rb"(?m)^Editor browser cases passed: ([1-9][0-9]*)\r?$", stream.read(65536))
    if len(counts) != 1:
        raise ValueError("Missing editor browser case count")
    return int(counts[0])

QUALIFY = "//tools/ci:unified_worker_qualification"


# Only the side that owns each configured artifact must pass. The other platform skips it.
WASM_CHECKS = {
    "//donner/editor/wasm:ci_darwin_graph_wasm_geode_package_size_tests",
    "//donner/editor/wasm:ci_darwin_graph_wasm_shipped_browser_dependency_audit_test",
} | {"//donner/svg/renderer/wasm:graph_" + name for name in (
    "render_test", "render_text_test", "wasm_size_tests", "geode_browser_default_audit_test",
)}


def _ordinary_built(evidence, selected):
    missing = {label for label in selected if evidence.targets.get(label) not in ("built", "incompatible")}
    compatible = {label for label in selected if evidence.targets.get(label) == "built"}
    missing.update((selected & (NATIVE_BROWSER | WASM_CHECKS)) - compatible)
    if missing:
        raise ValueError(f"Selected target did not finish: {sorted(missing)}")
    return compatible


def _qualification_runs(evidence):
    for label in evidence.members.get(QUALIFY, ()):
        if {run for target, _, run, _ in evidence.passed["remote"] if target == label} != {1, 2, 3, 4}:
            raise ValueError(f"Expected four uncached remote runs for {label}")


def _expected_tests(mandatory, built, test_rules, full_graph):
    editor = EDITOR_BROWSER | WASM_CHECKS
    if full_graph and editor - built:
        raise ValueError(f"Full graph omitted editor Wasm targets: {sorted(editor - built)}")
    return mandatory | (built & (test_rules | editor))


def verify(events, suites, ordinary=(), output_root=None, linux=(), full_graph=False):
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
    linux_built = _ordinary_built(evidence, set(linux))
    hardware = {label for label in linux if ":ci_linux_gpu_" in label}
    mandatory.update(hardware)
    fresh.update(hardware)
    expected = _expected_tests(mandatory, compatible | linux_built, evidence.test_rules, full_graph)
    summary_labels = {label for label, _ in evidence.summaries}
    # The Linux venue verifier and its analysis aspect restrict local actions to the Xvfb roots.
    local = evidence.passed["local"]
    if {shard[0] for shard in local} - (linux_built - hardware):
        raise ValueError("incomplete remote shards: unexpected local execution outside Linux selection")
    passed = evidence.passed["remote"] | evidence.passed["cached"] | local
    _check_summary_shards(evidence.summaries, passed)
    cached_required = {shard for shard in evidence.passed["cached"] if shard[0] in fresh}
    if summary_labels != expected or cached_required or evidence.passed[None]:
        raise ValueError(f"Test label mismatch {sorted(expected ^ summary_labels)}, "
                         f"incomplete remote shards {sorted(cached_required | evidence.passed[None])}")
    if output_root is not None:
        report_kernels(evidence.test_logs, output_root)
        for label in sorted(compatible & EDITOR_BROWSER):
            cases = editor_browser_cases(_declared_log(label, evidence.test_logs, output_root))
            print(f"{label}: {cases} editor browser cases passed")
        for label in sorted(compatible & HEADED):
            cases = firefox_cases(_declared_log(label, evidence.test_logs, output_root))
            print(f"{label}: {cases} headed Firefox cases passed")
    print(f"{len(expected)} native tests passed across {len(evidence.passed['remote'])} uncached "
          f"remote shards and {len(evidence.passed['cached'])} cached shards; "
          f"{len(compatible)} ordinary Darwin roots built, {len(selected - compatible)} incompatible")
    return darwin_classes(evidence, compatible)


def darwin_classes(evidence, compatible):
    if "//tools/ci:unified_editor_geode" not in evidence.members:
        return {}
    tests = compatible & evidence.test_rules
    counts = {"darwin-build": len(compatible), "darwin-test": len(tests)}
    suites = {
        "geode": "editor_geode", "metal-profile": "metal_full_validation", "metal-preflight": "metal_preflight",
        "metal-compiler": "metal_offline_compiler", "metal-native": "metal_native", "browser-archive": "mac_browser_archive",
    }
    for key, name in suites.items():
        members = evidence.members.get("//tools/ci:unified_" + name)
        if members:
            counts[key] = len(members)
    if "browser-archive" in counts:
        selected = {t for t in tests if t != ARCHIVE and "launches-browser" in evidence.tags[t]}
        headed = {t for t in selected if "requires_desktop_session" in evidence.tags[t]}
        counts["headless-browser"] = len(selected - headed)
        counts["desktop-browser"] = len(headed)
    return counts


def _suite_requirements(members, suites):
    mandatory = set()
    fresh = set()
    for suite in suites:
        if not members.get(suite):
            raise ValueError(f"No test suite expansion for {suite}")
        mandatory.update(members[suite])
        if suite not in ("//tools/ci:unified_editor_geode", "//tools/ci:linux_wgpu_resvg_reference"):
            fresh.update(members[suite])
    return mandatory, fresh


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ordinary", type=Path)
    parser.add_argument("--linux", type=Path)
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
        linux_roots = args.linux.read_text().splitlines() if args.linux else []
        full_graph = os.environ.get("TARGETS_FALLBACK") == "true" or os.environ.get("TARGETS_AFFECTED") == "//..."
        counts = verify((json.loads(line) for line in stream), args.suites, ordinary_roots,
                        Path("bazel-out"), linux_roots, full_graph)
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            print("darwin_classes=" + json.dumps(counts, sort_keys=True), file=output)
