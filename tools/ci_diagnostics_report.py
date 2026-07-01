#!/usr/bin/env python3
"""Generate a compact markdown summary from CI diagnostic artifacts."""

from __future__ import annotations

import argparse
import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path


TEST_RESULT_RE = re.compile(
    r"^(//\S+)\s+(?:\(cached\)\s+)?"
    r"(PASSED|FAILED|TIMEOUT|FLAKY|SKIPPED|FAILED TO BUILD)"
    r"(?:\s+in\s+([0-9.]+)s)?"
)
ELAPSED_RE = re.compile(r"INFO: Elapsed time: ([0-9.]+)s, Critical Path: ([0-9.]+)s")
PROCESS_RE = re.compile(r"INFO: ([0-9]+) processes: (.*)")
SOURCE_RE = re.compile(r"\.(?:c|cc|cpp|cxx|m|mm)$")


def read_key_values(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.exists():
        return values

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value
    return values


def parse_step_timings(path: Path) -> list[tuple[str, float]]:
    timings: list[tuple[str, float]] = []
    if not path.exists():
        return timings

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        fields = line.split("\t")
        if len(fields) != 2:
            continue
        try:
            timings.append((fields[0], float(fields[1])))
        except ValueError:
            continue
    return timings


def parse_bazel_log(path: Path) -> dict[str, object]:
    tests: list[dict[str, object]] = []
    elapsed: list[tuple[float, float]] = []
    processes: list[str] = []

    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        test_match = TEST_RESULT_RE.match(line)
        if test_match:
            duration = test_match.group(3)
            tests.append(
                {
                    "label": test_match.group(1),
                    "status": test_match.group(2),
                    "duration_s": float(duration) if duration is not None else 0.0,
                    "log": str(path),
                }
            )
            continue

        elapsed_match = ELAPSED_RE.search(line)
        if elapsed_match:
            elapsed.append((float(elapsed_match.group(1)), float(elapsed_match.group(2))))
            continue

        process_match = PROCESS_RE.search(line)
        if process_match:
            processes.append(process_match.group(0))

    return {
        "tests": tests,
        "elapsed": elapsed,
        "processes": processes,
    }


def parse_bep_json(path: Path) -> list[dict[str, object]]:
    """Best-effort BEP parser for test summaries.

    Bazel's JSON BEP stream is newline-delimited. Keep this intentionally
    tolerant; the plain log parser is still the fallback for all current needs.
    """

    tests: list[dict[str, object]] = []
    if not path.exists():
        return tests

    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        summary = event.get("testSummary")
        event_id = event.get("id", {})
        summary_id = event_id.get("testSummary", {})
        label = summary_id.get("label")
        if not summary or not label:
            continue
        duration_ms = (
            summary.get("totalRunDurationMillis")
            or summary.get("runDurationMillis")
            or summary.get("totalRunDuration")
            or 0
        )
        try:
            duration_s = float(duration_ms) / 1000.0
        except (TypeError, ValueError):
            duration_s = 0.0
        tests.append(
            {
                "label": label,
                "status": summary.get("overallStatus", "UNKNOWN"),
                "duration_s": duration_s,
                "log": str(path),
            }
        )
    return tests


def parse_test_xml(path: Path) -> list[dict[str, object]]:
    cases: list[dict[str, object]] = []
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError:
        return cases

    for testcase in root.iter("testcase"):
        classname = testcase.attrib.get("classname", "")
        name = testcase.attrib.get("name", "")
        duration = testcase.attrib.get("time", "0")
        try:
            duration_s = float(duration)
        except ValueError:
            duration_s = 0.0

        status = "PASSED"
        if testcase.find("failure") is not None:
            status = "FAILED"
        elif testcase.find("error") is not None:
            status = "ERROR"
        elif testcase.find("skipped") is not None:
            status = "SKIPPED"

        label_parts = path.parts
        try:
            testlogs_index = label_parts.index("testlogs")
            target = "//" + "/".join(label_parts[testlogs_index + 1 : -1])
            if ":" not in target:
                package, name_part = target.rsplit("/", 1)
                target = f"{package}:{name_part}"
        except (ValueError, IndexError):
            target = str(path.parent)

        cases.append(
            {
                "target": target,
                "case": f"{classname}.{name}" if classname else name,
                "status": status,
                "duration_s": duration_s,
            }
        )
    return cases


def load_json_records(path: Path) -> list[object]:
    text = path.read_text(encoding="utf-8", errors="replace").strip()
    if not text:
        return []
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        records: list[object] = []
        for line in text.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                continue
        return records
    if isinstance(parsed, list):
        return parsed
    return [parsed]


def list_field(record: dict[str, object], *names: str) -> list[str]:
    for name in names:
        value = record.get(name)
        if isinstance(value, list):
            return [str(item) for item in value]
    return []


def scalar_field(record: dict[str, object], *names: str) -> str:
    for name in names:
        value = record.get(name)
        if value is not None:
            return str(value)
    return ""


def action_hint(record: dict[str, object]) -> str:
    args = list_field(record, "commandArgs", "command_args")
    mnemonic = scalar_field(record, "mnemonic")
    if mnemonic == "CppCompile":
        for arg in args:
            if SOURCE_RE.search(arg):
                return arg
    if mnemonic in {"CppLink", "ObjcLink", "CppArchive"}:
        outputs = list_field(record, "actualOutputs", "actual_outputs", "listedOutputs", "listed_outputs")
        if outputs:
            return outputs[0]
        for index, arg in enumerate(args[:-1]):
            if arg == "-o":
                return args[index + 1]
    return ""


def parse_execution_logs(diagnostics_dir: Path) -> tuple[list[dict[str, str]], dict[tuple[str, str], int]]:
    actions: list[dict[str, str]] = []
    counts: dict[tuple[str, str], int] = {}
    for path in sorted(diagnostics_dir.glob("*/execution-log.json")):
        for parsed in load_json_records(path):
            if not isinstance(parsed, dict):
                continue
            records = parsed.get("spawnExec")
            if isinstance(records, dict):
                candidates = [records]
            elif isinstance(records, list):
                candidates = [record for record in records if isinstance(record, dict)]
            else:
                candidates = [parsed]
            for record in candidates:
                mnemonic = scalar_field(record, "mnemonic") or "unknown"
                runner = scalar_field(record, "runner") or scalar_field(record, "strategy") or "unknown"
                target = scalar_field(record, "targetLabel", "target_label")
                hint = action_hint(record)
                counts[(mnemonic, runner)] = counts.get((mnemonic, runner), 0) + 1
                actions.append(
                    {
                        "step": path.parent.name,
                        "mnemonic": mnemonic,
                        "runner": runner,
                        "target": target,
                        "hint": hint,
                    }
                )
    return actions, counts


def markdown_table(headers: list[str], rows: list[list[str]]) -> list[str]:
    if not rows:
        return ["_No data found._"]
    lines = ["| " + " | ".join(headers) + " |"]
    lines.append("| " + " | ".join(["---"] * len(headers)) + " |")
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return lines


def generate_report(diagnostics_dir: Path) -> str:
    manifest = read_key_values(diagnostics_dir / "manifest.env")
    selection = read_key_values(diagnostics_dir / "target-selection.env")
    target_list = diagnostics_dir / "target-list.txt"
    step_timings = parse_step_timings(diagnostics_dir / "step-timings.tsv")

    parsed_logs = [parse_bazel_log(path) for path in sorted(diagnostics_dir.glob("*/bazel.log"))]
    log_tests = [test for parsed in parsed_logs for test in parsed["tests"]]  # type: ignore[index]
    bep_tests = [
        test
        for path in sorted(diagnostics_dir.glob("*/bep.json"))
        for test in parse_bep_json(path)
    ]
    tests = bep_tests or log_tests
    tests = sorted(tests, key=lambda item: float(item["duration_s"]), reverse=True)
    test_cases = [
        test_case
        for path in sorted(diagnostics_dir.glob("*/testlogs/**/test.xml"))
        for test_case in parse_test_xml(path)
    ]
    test_cases = sorted(test_cases, key=lambda item: float(item["duration_s"]), reverse=True)
    actions, action_counts = parse_execution_logs(diagnostics_dir)

    elapsed_rows: list[list[str]] = []
    for path, parsed in zip(sorted(diagnostics_dir.glob("*/bazel.log")), parsed_logs):
        for elapsed_s, critical_s in parsed["elapsed"]:  # type: ignore[index]
            elapsed_rows.append([path.parent.name, f"{elapsed_s:.1f}", f"{critical_s:.1f}"])

    process_rows: list[list[str]] = []
    for path, parsed in zip(sorted(diagnostics_dir.glob("*/bazel.log")), parsed_logs):
        for process_line in parsed["processes"]:  # type: ignore[index]
            process_rows.append([path.parent.name, str(process_line)])

    target_count = "unknown"
    if target_list.exists():
        target_count = str(
            len([line for line in target_list.read_text(encoding="utf-8").splitlines() if line])
        )

    lines: list[str] = []
    lines.append("# CI Diagnostics")
    lines.append("")
    lines.append("## Run")
    lines.extend(
        markdown_table(
            ["Field", "Value"],
            [
                ["workflow", manifest.get("workflow", "")],
                ["job", manifest.get("job", "")],
                ["runner", manifest.get("runner_name", "")],
                ["sha", manifest.get("sha", "")],
                ["fallback", manifest.get("target_fallback", "")],
                ["target mode", manifest.get("target_mode", "")],
                ["selection reason", selection.get("reason", "")],
                ["target count", target_count],
            ],
        )
    )
    lines.append("")
    lines.append("## Step Timings")
    lines.extend(markdown_table(["Step", "Seconds"], [[name, f"{seconds:.1f}"] for name, seconds in step_timings]))
    lines.append("")
    lines.append("## Bazel Phase Summaries")
    lines.extend(markdown_table(["Step", "Elapsed s", "Critical path s"], elapsed_rows))
    lines.append("")
    lines.append("## Slowest Tests")
    lines.extend(
        markdown_table(
            ["Status", "Seconds", "Label"],
            [[str(test["status"]), f"{float(test['duration_s']):.1f}", str(test["label"])] for test in tests[:20]],
        )
    )
    lines.append("")
    lines.append("## Slowest Test Cases")
    lines.extend(
        markdown_table(
            ["Status", "Seconds", "Target", "Case"],
            [
                [
                    str(test_case["status"]),
                    f"{float(test_case['duration_s']):.1f}",
                    str(test_case["target"]),
                    str(test_case["case"]),
                ]
                for test_case in test_cases[:30]
            ],
        )
    )
    lines.append("")
    lines.append("## Action Counts")
    action_count_rows = [
        [mnemonic, runner, str(count)]
        for (mnemonic, runner), count in sorted(
            action_counts.items(), key=lambda item: item[1], reverse=True
        )[:30]
    ]
    lines.extend(markdown_table(["Mnemonic", "Runner", "Count"], action_count_rows))
    lines.append("")
    lines.append("## C++ Compile Samples")
    compile_rows = [
        [action["step"], action["runner"], action["target"], action["hint"]]
        for action in actions
        if action["mnemonic"] == "CppCompile"
    ][:30]
    lines.extend(markdown_table(["Step", "Runner", "Target", "Source"], compile_rows))
    lines.append("")
    lines.append("## Link Samples")
    link_rows = [
        [action["step"], action["runner"], action["target"], action["hint"]]
        for action in actions
        if action["mnemonic"] in {"CppLink", "ObjcLink", "CppArchive"}
    ][:30]
    lines.extend(markdown_table(["Step", "Runner", "Target", "Output"], link_rows))
    lines.append("")
    lines.append("## Local Execution Samples")
    local_rows = [
        [action["step"], action["mnemonic"], action["runner"], action["target"], action["hint"]]
        for action in actions
        if "remote" not in action["runner"].lower() and action["runner"] != "unknown"
    ][:30]
    lines.extend(markdown_table(["Step", "Mnemonic", "Runner", "Target", "Hint"], local_rows))
    lines.append("")
    lines.append("## Action Summary Lines")
    lines.extend(markdown_table(["Step", "Summary"], process_rows[-10:]))
    lines.append("")
    lines.append("Raw profile, BEP, execution log, target list, and command log files are in this artifact.")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("diagnostics_dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    report = generate_report(args.diagnostics_dir)
    output = args.output or args.diagnostics_dir / "report.md"
    output.write_text(report, encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
