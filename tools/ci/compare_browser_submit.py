#!/usr/bin/env python3
"""Run an opt-in same-host comparison of fixed browser submission revisions."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import unquote, urlparse

REVISIONS = {
    "control": "285f7441e208e615a2e6500849fa76aa97ee398b",
    "trial": "acea4aa48283cded92a559bbefee2112e6084031",
}
ORDER = ("control", "trial", "trial", "control")
LABEL_PREFIX = "//donner/editor/wasm/tests:"
TARGETS = (
    LABEL_PREFIX + "browser_responsiveness_perf_test_firefox",
    LABEL_PREFIX + "browser_responsiveness_perf_test_chromium",
)
COMMON_FLAGS = (
    "--config=ci",
    "-c",
    "opt",
    "--build_tag_filters=perf",
    "--test_tag_filters=perf",
    "--test_env=DONNER_BROWSER_LATENCY_GATES=report",
    "--test_env=DONNER_BROWSER_COMPLETION_CHECKS=report",
)
TEST_FLAGS = (
    "--local_test_jobs=1",
    "--keep_going",
    "--nocache_test_results",
    "--test_output=all",
    "--test_arg=--grep=masked crown drag",
)


def write_json(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def bazel_for_arm(root, arm):
    output_root = root / "bazel"
    return [
        "bazelisk",
        f"--output_user_root={output_root}",
        f"--output_base={output_root / arm}",
    ]


def test_command(root, arm, period):
    return [
        *bazel_for_arm(root, arm), "test", *COMMON_FLAGS, *TEST_FLAGS,
        f"--build_event_json_file={period / 'events.json'}", *TARGETS,
    ]


def run(root):
    checked_out = {}
    for arm, revision in REVISIONS.items():
        tree = root / arm
        subprocess.run(["git", "worktree", "add", "--detach", tree, revision], check=True)
        observed = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=tree, text=True)
        if observed.strip() != revision:
            raise RuntimeError(f"{arm} checkout does not match its fixed revision")
        checked_out[arm] = tree
        subprocess.run([*bazel_for_arm(root, arm), "build", *COMMON_FLAGS, *TARGETS], cwd=tree, check=True)

    for index, arm in enumerate(ORDER, 1):
        period = root / str(index)
        period.mkdir()
        status = {"arm": arm, "revision": REVISIONS[arm], "bazelExit": None}
        write_json(period / "status.json", status)
        result = subprocess.run(test_command(root, arm, period), cwd=checked_out[arm], check=False)
        status["bazelExit"] = result.returncode
        write_json(period / "status.json", status)
        # The next use of this arm's workspace can overwrite all native Bazel test outputs.
        targets = snapshot_period(root, index)
        if result.returncode not in (0, 3) or not targets:
            raise RuntimeError(f"{arm} period {index}: Bazel did not complete test selection/execution")


def collect_target(event, directory, output_root):
    result = event["testResult"]
    outputs = []
    for item in result.get("testActionOutput", ()):
        item_name = item.get("name")
        uri = urlparse(item.get("uri", ""))
        if item_name not in ("test.log", "test.xml") or uri.scheme != "file":
            continue
        source = Path(unquote(uri.path)).resolve()
        if source.is_relative_to(output_root) and source.name == item_name and source.is_file():
            shutil.copyfile(source, directory / item_name)
            outputs.append(item_name)

    log = directory / "test.log"
    report = {
        "status": result.get("status"),
        "uncached": not (
            result.get("cachedLocally") or result.get("executionInfo", {}).get("cachedRemotely")
        ),
        "output": outputs,
        "panes": [],
    }
    if log.is_file():
        body = log.read_text(encoding="utf-8", errors="replace")
        for line in body.splitlines():
            if line.startswith("geode-interaction {"):
                try:
                    value = json.loads(line[len("geode-interaction "):])
                except json.JSONDecodeError:
                    continue
                if isinstance(value, dict) and isinstance(value.get("sourceVisible"), bool):
                    stream = (value.get("final") or {}).get("lastDragStream") or {}
                    report["panes"].append({
                        "sourceVisible": value["sourceVisible"],
                        "moveInputs": len(stream.get("dispatchTimes", ())),
                        "pressMs": stream.get("pressDispatchedAtMs"),
                        "releaseMs": stream.get("releasedAtMs"),
                        "p95Ms": (value.get("drag") or {}).get(
                            "inputToGpuCompletedFrameMs", {}
                        ).get("p95"),
                    })
        report["exactCaseSelection"] = bool(re.search(r"^Running 2 tests using 1 worker", body, re.M))
    return report


def snapshot_period(root, index):
    """Copy a period only while its output paths still belong to that period."""
    period = root / str(index)
    target_dir = root / "evidence" / str(index)
    target_dir.mkdir(parents=True, exist_ok=True)
    seal = target_dir / "targets.json"
    if seal.is_file():
        return json.loads(seal.read_text(encoding="utf-8"))

    targets = {}
    output_root = (root / "bazel").resolve()
    events = period / "events.json"
    if events.is_file():
        with events.open(encoding="utf-8") as stream:
            for line in stream:
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    # A cancelled action may leave the final BEP event truncated.
                    continue
                label = event.get("id", {}).get("testResult", {}).get("label")
                if label not in TARGETS or "testResult" not in event:
                    continue
                directory = target_dir / label.removeprefix(LABEL_PREFIX)
                directory.mkdir(exist_ok=True)
                targets[label] = collect_target(event, directory, output_root)
    write_json(seal, targets)
    return targets


def collect(root):
    evidence = root / "evidence"
    evidence.mkdir(exist_ok=True)
    periods = []
    for index, arm in enumerate(ORDER, 1):
        status_path = root / str(index) / "status.json"
        status = json.loads(status_path.read_text(encoding="utf-8")) if status_path.is_file() else {}
        periods.append({"number": index, "expectedArm": arm, **status, "targets": {}})

    # Only the most recently started period can still own its live test outputs. Older periods
    # must have sealed evidence already; never try to reconstruct a missing seal from reused paths.
    last_started = max((row["number"] for row in periods if "arm" in row), default=0)
    for row in periods:
        index = row["number"]
        sealed = (evidence / str(index) / "targets.json").is_file()
        if sealed or index == last_started:
            row["targets"] = snapshot_period(root, index)

    complete = all(
        row.get("arm") == row["expectedArm"]
        and row.get("revision") == REVISIONS[row["expectedArm"]]
        and row.get("bazelExit") in (0, 3)
        and set(row["targets"]) == set(TARGETS)
        and all(
            target.get("exactCaseSelection")
            and target["uncached"]
            and target["status"] in ("PASSED", "FAILED")
            and sorted(pane["sourceVisible"] for pane in target["panes"]) == [False, True]
            and all(pane["moveInputs"] == 60 for pane in target["panes"])
            and set(target["output"]) == {"test.log", "test.xml"}
            for target in row["targets"].values()
        )
        for row in periods
    )
    latency_complete = complete and all(
        pane["p95Ms"] is not None
        for row in periods
        for result in row["targets"].values()
        for pane in result["panes"]
    )
    write_json(
        evidence / "comparison.json",
        {"complete": complete, "latencyComplete": latency_complete, "periods": periods},
    )
    return complete and all(row["bazelExit"] == 0 for row in periods)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("run", "collect"))
    args = parser.parse_args()
    root = Path(os.environ["RUNNER_TEMP"]) / "browser-submit-comparison"
    root.mkdir(exist_ok=True)
    if args.phase == "run":
        run(root)
    elif not collect(root):
        raise SystemExit("comparison is incomplete or contains failed tests; see the retained evidence")


if __name__ == "__main__":
    main()
