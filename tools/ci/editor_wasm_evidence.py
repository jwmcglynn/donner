"""Collect declared browser outputs from the hosted Editor Wasm Bazel invocation."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
from urllib.parse import unquote, urlsplit
from zipfile import ZipFile

from verify_unified_tests import HOSTED_EDITOR_BROWSER_NAMES, editor_browser_cases

PACKAGE = "//donner/editor/wasm/tests:"
HOSTED = {PACKAGE + "hosted_" + name for name in HOSTED_EDITOR_BROWSER_NAMES}
QUARANTINE = PACKAGE + "hosted_firefox_quarantine_report"
RESIDENCY = {PACKAGE + "hosted_chromium_editor_startup_test", PACKAGE + "hosted_firefox_editor_compatibility_test"}
BUDGETS = {"//donner/svg/renderer/wasm:hosted_wasm_size_tests_report",
           "//donner/editor/wasm:hosted_wasm_geode_package_size_tests_report"}


def declared_file(uri, root, label, suffix):
    location = urlsplit(uri)
    if location.scheme != "file" or location.netloc:
        raise ValueError(f"Missing local Bazel output for {label}")
    actual = Path(unquote(location.path)).resolve(strict=True)
    package, name = label[2:].split(":", 1)
    tail = ("testlogs", *package.split("/"), name, *suffix)
    if not actual.is_relative_to(root) or actual.parts[-len(tail):] != tail or not actual.is_file():
        raise ValueError(f"Unexpected Bazel output for {label}")
    return actual


def test_results(stream):
    for line in stream:
        event = json.loads(line)
        result = event.get("testResult")
        if result is not None:
            outputs = {item["name"]: item["uri"] for item in result.get("testActionOutput", []) if "uri" in item}
            yield event["id"]["testResult"]["label"], result.get("status"), outputs


def report_budget(outputs, root, label):
    log = declared_file(outputs["test.log"], root, label, ("test.log",))
    for row in log.read_text(errors="replace").splitlines():
        if re.match(r"^(renderer|editor)-payload-budget ", row):
            print(row)


def stage_archive(uri, root, label, directory, require_accounting):
    archive = declared_file(uri, root, label, ("test.outputs", "outputs.zip"))
    shutil.copyfile(archive, directory / "outputs.zip")
    with ZipFile(archive) as contents:
        if require_accounting and not any(name.endswith("/gpu-residency-accounting.json") for name in contents.namelist()):
            raise ValueError(f"Missing GPU residency accounting for {label}")
        if label == QUARANTINE:
            report = contents.getinfo("quarantine-report.json")
            if report.file_size > 16 * 1024 * 1024:
                raise ValueError("Oversized quarantine report")
            (directory / "quarantine-report.json").write_bytes(contents.read(report))


def stage_browser(label, status, outputs, root, destination, complete):
    name = "firefox-quarantine-report" if label == QUARANTINE else label.split(":")[1].removeprefix("hosted_")
    directory = destination / name
    directory.mkdir(parents=True, exist_ok=True)
    if not complete and status != "PASSED" and "test.log" in outputs:
        shutil.copyfile(declared_file(outputs["test.log"], root, label, ("test.log",)), directory / "test.log")
    # Bazel names the output with double underscores in BEP and a slash on disk.
    archive = outputs.get("test.outputs__outputs.zip") or outputs.get("test.outputs/outputs.zip")
    if archive:
        stage_archive(archive, root, label, directory, complete and label in RESIDENCY)
    elif (complete and label in RESIDENCY) or label == QUARANTINE:
        raise ValueError(f"Missing declared browser output archive for {label}")
    if complete and label != QUARANTINE:
        if status != "PASSED":
            raise ValueError(f"Hosted browser target did not pass: {label}")
        log = declared_file(outputs["test.log"], root, label, ("test.log",))
        count = editor_browser_cases(log)
        print(f"{label}: {count} editor browser cases passed")
        (directory / "case-count.txt").write_text(f"{count}\n")


def selected_browsers(value):
    labels = value.split()
    selected = set(labels)
    if not labels or len(labels) != len(selected) or not selected <= HOSTED:
        raise ValueError("Invalid hosted Editor Wasm browser selection")
    return selected


def stage(stream, root, destination, *, quarantine=False, complete=True, subset=None):
    selected = {QUARANTINE} if quarantine else (subset or HOSTED)
    seen = set()
    for label, status, outputs in test_results(stream):
        if label in BUDGETS:
            report_budget(outputs, root, label)
        if subset is not None and label in HOSTED - selected:
            raise ValueError(f"Unexpected hosted browser result: {label}")
        if label not in selected:
            continue
        if label in seen:
            raise ValueError(f"Repeated hosted browser result: {label}")
        seen.add(label)
        stage_browser(label, status, outputs, root, destination, complete)
    if (complete or quarantine) and seen != selected:
        raise ValueError(f"Missing hosted browser evidence: {sorted(selected - seen)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bep", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--quarantine", action="store_true")
    parser.add_argument("--partial", action="store_true")
    args = parser.parse_args()
    selected = os.environ.get("HOSTED_EDITOR_BROWSER_TARGETS")
    subset = selected_browsers(selected) if selected else None
    if args.quarantine or not args.partial or args.bep.exists():
        with args.bep.open(encoding="utf-8") as events:
            stage(events, Path("bazel-out").resolve(strict=True), args.destination,
                  quarantine=args.quarantine, complete=not args.partial, subset=subset)
