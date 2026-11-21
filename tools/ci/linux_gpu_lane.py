"""Select and verify the Linux tests that run on a remote execution instance with a hardware GPU.

The Linux GPU lane runs two Bazel invocations with `--config=ci-linux-gpu` and the same output
base: a build on the default instance, then a test of the same targets with the GPU instance
added (see "Linux GPU executors" in docs/building.md). An instance applies to every action of an
invocation, so the second one must execute nothing but the selected tests.

    linux_gpu_lane.py query PATTERN...      print the loading query for the selection
    linux_gpu_lane.py select < QUERY.jsonl  print the selected test labels, one per line
    linux_gpu_lane.py check --execution-log LOG.json --build-events BEP.json SELECTION

`select` reads the query's `--output=streamed_jsonproto` (with the tags, env and dep attributes).
A test is selected when it is a transitioned test wrapper that opens a GPU device, lies in the
reviewed scope, is not a reviewed test for another OS and leaves the driver choice to the executor.

`check` is the lane's status gate as well as its venue check, so a caller that ignores Bazel's
exit code still fails. It fails unless:
  - every spawn the test invocation executed is a selected test's runner, run remotely with the
    GPU request, and no test result came from a cache;
  - each selected test has, for every run and shard its summary reports, a fresh remote test
    runner spawn with the GPU request and a fresh remote result, and its summary passed;
  - at least one test ran, and the build event stream records a successful invocation.
The JSON execution log carries no invocation identifier, so the two files cannot be matched to one
invocation; the per-test spawn and result counts must agree instead.
"""

import argparse
import json
import sys

# The test wrapper rule that every GPU-opening test runs through. It returns no
# testing.ExecutionInfo, so its test actions run on the target platform and carry its GPU request.
WRAPPER_RULE = "_donner_multi_transitioned_test"
# The tag a test that opens a GPU device carries (LOCAL_GPU_TEST_TAG in build_defs/rules.bzl).
GPU_TAG = "resources:local_gpu:1"
# Tests that never run in this lane: opt-in, wall-clock and soak targets, remote twins of local
# isolation wrappers, and anything that must run on the client.
EXCLUDED_TAGS = frozenset(("manual", "perf", "fuzz_soak", "ci-remote-gpu", "local", "no-remote",
                           "no-remote-exec"))
# Reviewed scope: the Geode backend's own tests (device, unit and resource suites) and the suite
# that compares Geode output with the golden images. Widen it as hardware results are reviewed
# suite by suite. //donner/svg/renderer/tests:renderer_geode_tests waits for #2541: on a hardware
# GPU its many-stop radial gradient cases differ from tiny-skia, which they must match exactly.
SCOPE_PACKAGES = ("//donner/svg/renderer/geode",)
SCOPE_TARGETS = ("//donner/svg/renderer/tests:renderer_geode_golden_tests",)
# Reviewed tests in the scope that build only for another OS. The loading query cannot evaluate
# target compatibility, and the lane would otherwise skip them, so they are left out here and any
# skipped test fails the check.
OTHER_OS_TESTS = frozenset(("//donner/svg/renderer/geode:geode_native_counters_tests",))
# A test that chooses its own Vulkan driver compares against that driver on purpose; on a hardware
# executor the choice would be overridden or contradicted, so it stays on its software lane.
DRIVER_VARIABLES = frozenset(("VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_ADD_DRIVER_FILES",
                              "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE"))
GPU_PROPERTY = {"name": "gpu", "value": "present"}


def query(patterns):
    """The loading query: the scoped wrapper tests among `patterns`, with their implementations."""
    if not patterns:
        raise ValueError("Empty Linux GPU target selection")
    requested = "set(%s)" % " ".join(json.dumps(pattern) for pattern in patterns)
    scope = " + ".join(["%s:all" % package for package in SCOPE_PACKAGES] + list(SCOPE_TARGETS))
    return ('let tests = kind("%s rule", %s intersect (%s)) in $tests + labels(dep, $tests)'
            % (WRAPPER_RULE, requested, scope))


def _attribute(rule, name, kind):
    for attribute in rule.get("attribute", []):
        if attribute["name"] == name:
            return attribute.get(kind)
    return None


def _in_scope(label):
    package = label.split(":", 1)[0]
    return package in SCOPE_PACKAGES or label in SCOPE_TARGETS


def _env_keys(attribute):
    """The env keys of a query attribute, in every branch of a select(); None if unreadable."""
    if "stringDictValue" in attribute:
        return [entry["key"] for entry in attribute["stringDictValue"]]
    branches = [entry for element in attribute.get("selectorList", {}).get("elements", [])
                for entry in element.get("entries", [])]
    if not branches:
        return None
    return [entry["key"] for branch in branches for entry in branch.get("stringDictValue", [])]


def _names_driver(rule):
    """Whether the rule's env chooses a Vulkan driver; an env the query shows unreadably counts."""
    attribute = next((item for item in rule.get("attribute", []) if item["name"] == "env"), None)
    if attribute is None:
        return False
    keys = _env_keys(attribute)
    return keys is None or bool(DRIVER_VARIABLES.intersection(keys))


def _eligible(label, rule):
    tags = set(_attribute(rule, "tags", "stringListValue") or [])
    return (rule.get("ruleClass") == WRAPPER_RULE and GPU_TAG in tags and not tags & EXCLUDED_TAGS
            and _in_scope(label) and label not in OTHER_OS_TESTS)


def select(lines):
    """Selected test labels, sorted, from streamed_jsonproto query lines."""
    rules = {}
    for line in lines:
        if line.strip():
            rule = json.loads(line)["rule"]
            rules[rule["name"]] = rule
    selected = []
    for label, rule in rules.items():
        if not _eligible(label, rule):
            continue
        # The wrapper rule has no env attribute of its own; it forwards its implementation's.
        dep = rules.get(_attribute(rule, "dep", "stringValue"))
        if dep is None:
            raise ValueError("Missing implementation rule for %s in the query results" % label)
        if not _names_driver(dep):
            selected.append(label)
    return sorted(selected)


def _json_values(text):
    """Every JSON value of a concatenated stream, as Bazel writes its JSON execution log."""
    decoder = json.JSONDecoder()
    index = 0
    while True:
        while index < len(text) and text[index].isspace():
            index += 1
        if index == len(text):
            return
        value, index = decoder.raw_decode(text, index)
        yield value


def _spawn_problem(spawn, selection):
    """Why the test invocation should not have executed or reused this spawn, or None."""
    label = spawn.get("targetLabel", "")
    mnemonic = spawn.get("mnemonic", "")
    what = "%s %s" % (mnemonic or "spawn", label)
    if spawn.get("cacheHit"):
        return "%s reused a cached test result" % what if mnemonic == "TestRunner" else None
    if mnemonic != "TestRunner" or label not in selection:
        return ("%s executed (%s); the build invocation must leave only the selected tests to run"
                % (what, spawn.get("runner", "unknown runner")))
    if spawn.get("runner") != "remote":
        return "%s ran on %s, not remotely" % (what, spawn.get("runner"))
    if not _has_gpu_request(spawn):
        return "%s ran without the GPU request" % what
    return None


def execution_problems(spawns, selection):
    """Spawns the test invocation should not have executed or reused."""
    found = (_spawn_problem(spawn, selection) for spawn in spawns)
    return [problem for problem in found if problem]


def _has_gpu_request(spawn):
    return GPU_PROPERTY in spawn.get("platform", {}).get("properties", [])


def gpu_test_runs(spawns):
    """{label: number of fresh remote test runner spawns that carried the GPU request}."""
    runs = {}
    for spawn in spawns:
        if (spawn.get("mnemonic") == "TestRunner" and spawn.get("runner") == "remote"
                and not spawn.get("cacheHit") and _has_gpu_request(spawn)):
            runs[spawn.get("targetLabel", "")] = runs.get(spawn.get("targetLabel", ""), 0) + 1
    return runs


def _result_problems(label, result, selection):
    if label not in selection:
        return ["%s reported a result but was not selected" % label]
    info = result.get("executionInfo", {})
    if result.get("cachedLocally") or info.get("cachedRemotely"):
        return ["%s reported a cached result" % label]
    if info.get("strategy") != "remote":
        return ["%s ran with strategy %r, not remotely" % (label, info.get("strategy"))]
    return []


def _read_events(events, selection):
    """(problems, finish event payload, {label: {(run, shard)}}, {label: summary}, skipped)."""
    problems = []
    finished = None
    shards = {}
    summaries = {}
    skipped = set()
    for event in events:
        identity = event.get("id", {})
        if "buildFinished" in identity:
            finished = event.get("finished", {})
        elif "testResult" in event:
            label = identity["testResult"]["label"]
            problems += _result_problems(label, event["testResult"], selection)
            shards.setdefault(label, set()).add(
                (identity["testResult"].get("run", 1), identity["testResult"].get("shard", 1)))
        elif "testSummary" in event:
            summaries[identity["testSummary"]["label"]] = event["testSummary"]
        elif "targetCompleted" in identity and "aborted" in event:
            skipped.add(identity["targetCompleted"]["label"])
    return problems, finished, shards, summaries, skipped


def _finish_problems(finished):
    if finished is None:
        return ["the build event stream has no build-finished event"]
    exit_code = finished.get("exitCode", {})
    if exit_code.get("name") != "SUCCESS" or exit_code.get("code", 0) != 0:
        return ["the build-finished event does not record a successful invocation"]
    return []


def result_problems(events, selection, runs):
    """Problems of the build events, given the GPU test runs counted from the execution log."""
    problems, finished, shards, summaries, skipped = _read_events(events, selection)
    problems += _finish_problems(finished)
    if not sum(runs.values()):
        problems.append("no selected test ran on the remote executor with the GPU request")
    for label in sorted(selection & skipped):
        problems.append("%s was skipped or aborted instead of running" % label)
    return problems + _coverage_problems(selection - skipped, summaries, shards, runs)


def _coverage_problems(selection, summaries, shards, runs):
    """Selected tests whose summary, results or GPU runs are missing, short or failing."""
    problems = []
    for label in sorted(selection):
        summary = summaries.get(label)
        if summary is None:
            problems.append("%s reported no result" % label)
            continue
        expected = summary.get("runCount", 1) * (summary.get("shardCount") or 1)
        if len(shards.get(label, ())) != expected:
            problems.append("%s reported %d of %d run and shard results"
                            % (label, len(shards.get(label, ())), expected))
        if runs.get(label, 0) != expected:
            problems.append("%s has %d of %d remote test runs with the GPU request"
                            % (label, runs.get(label, 0), expected))
        if summary.get("overallStatus") != "PASSED":
            problems.append("%s did not pass: %s" % (label, summary.get("overallStatus")))
    return problems


def check(execution_log, build_events, selection):
    with open(execution_log, encoding="utf-8") as log:
        spawns = list(_json_values(log.read()))
    with open(build_events, encoding="utf-8") as stream:
        events = [json.loads(line) for line in stream if line.strip()]
    runs = gpu_test_runs(spawns)
    problems = execution_problems(spawns, selection) + result_problems(events, selection, runs)
    for problem in problems:
        print(problem, file=sys.stderr)
    print("Checked %d spawns (%d GPU test runs) for %d selected tests: %d problem(s)."
          % (len(spawns), sum(runs.values()), len(selection), len(problems)))
    return 1 if problems else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("query").add_argument("patterns", nargs="+")
    commands.add_parser("select")
    checker = commands.add_parser("check")
    checker.add_argument("--execution-log", required=True)
    checker.add_argument("--build-events", required=True)
    checker.add_argument("selection", help="file with one selected label per line")
    args = parser.parse_args(argv)
    if args.command == "query":
        print(query(args.patterns))
        return 0
    if args.command == "select":
        for label in select(sys.stdin):
            print(label)
        return 0
    with open(args.selection, encoding="utf-8") as listing:
        selection = {line.strip() for line in listing if line.strip()}
    if not selection:
        print("Empty Linux GPU target selection", file=sys.stderr)
        return 2
    return check(args.execution_log, args.build_events, selection)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
