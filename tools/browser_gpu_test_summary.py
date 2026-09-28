"""Report only bounded browser test statuses and timings from Bazel events."""

import argparse
import json
from pathlib import Path
import re


TARGETS = tuple("//donner/editor/wasm/tests:" + name for name in (
    "chromium_remote_smoke", "catalog_font_loading_test",
    "browser_presentation_regression_test", "standalone_geode_browser_renderer_test",
))
STATUSES = frozenset(("PASSED", "FAILED", "TIMEOUT", "INCOMPLETE", "REMOTE_FAILURE",
                      "FAILED_TO_BUILD", "TOOL_HALTED_BEFORE_TESTING"))
STRATEGIES = frozenset(("remote", "local", "darwin-sandbox", "linux-sandbox", "sandboxed"))
MAX_BYTES = 64 * 1024 * 1024


def _natural(value, maximum):
    if isinstance(value, bool):
        raise ValueError("Invalid timing field")
    if isinstance(value, str):
        if not re.fullmatch(r"[0-9]{1,16}", value):
            raise ValueError("Invalid timing field")
        value = int(value)
    if not isinstance(value, int) or not 0 <= value <= maximum:
        raise ValueError("Invalid timing field")
    return value


def _record(identity, result):
    label = identity.get("label")
    if label not in TARGETS or result.get("status") not in STATUSES:
        raise ValueError("Unexpected browser test identity or status")
    if any(identity.get(key, 1) != 1 for key in ("run", "shard", "attempt")):
        raise ValueError("Repeated browser test execution")
    execution = result.get("executionInfo", {})
    if not isinstance(execution, dict):
        raise ValueError("Invalid test execution record")
    for cached in (result.get("cachedLocally", False), result.get("cachedRemotely", False),
                   execution.get("cachedRemotely", False)):
        if cached is not False:
            raise ValueError("Browser comparison requires fresh execution")
    configuration = identity.get("configuration", {}).get("id", "")
    if not isinstance(configuration, str) or not re.fullmatch(r"[0-9a-f]{64}", configuration):
        raise ValueError("Invalid configuration identity")
    strategy = execution.get("strategy", "unknown")
    return {
        "target": label, "status": result["status"], "configuration": configuration,
        "start_ms": _natural(result.get("testAttemptStartMillisEpoch"), 10**15),
        "duration_ms": _natural(result.get("testAttemptDurationMillis"), 3600000),
        "strategy": strategy if strategy in STRATEGIES else "unknown",
    }


def summarize_events(events):
    records = {}
    for event in events:
        if not isinstance(event, dict):
            raise ValueError("Invalid event record")
        if "testResult" not in event:
            continue
        identity = event.get("id", {}).get("testResult", {})
        result = event["testResult"]
        if not isinstance(identity, dict) or not isinstance(result, dict):
            raise ValueError("Invalid test result record")
        record = _record(identity, result)
        if record["target"] in records:
            raise ValueError("Duplicate browser test result")
        records[record["target"]] = record
    if set(records) != set(TARGETS):
        raise ValueError("Missing browser test result")
    return {"schema_version": 1, "tests": [records[target] for target in TARGETS]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bep", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        if args.bep.stat().st_size > MAX_BYTES:
            raise ValueError("Event file exceeds its budget")
        events = (json.loads(line) for line in args.bep.read_text(encoding="utf-8").splitlines())
        summary = summarize_events(events)
    except (OSError, ValueError, TypeError, AttributeError):
        parser.error("Browser comparison evidence is invalid or incomplete")
    args.output.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
