#!/usr/bin/env python3
"""Retain a path-safe proof of a coverage run's selection, tests, and LCOV universe."""

import argparse
from collections import Counter
from hashlib import sha256
import json
from pathlib import Path
import re
import sys

from lcov_metrics import LcovMetrics, collect_lcov_metrics


_PATTERN = re.compile(r"[@A-Za-z0-9_./:+*=\-]+\Z")
_REVISION = re.compile(r"[0-9a-f]{40}\Z")
_CONFIGURATION = re.compile(r"[A-Za-z0-9_-]{0,64}\Z")


class CoverageProofError(ValueError):
    """A validation failure with a fixed, path-free message safe for CI logs."""


def _digest(path: Path) -> str:
    checksum = sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def _label(event_id: object) -> tuple[str, str] | None:
    if not isinstance(event_id, dict):
        return None
    for name in ("testSummary", "targetCompleted", "targetConfigured"):
        holder = event_id.get(name)
        if isinstance(holder, dict) and isinstance(holder.get("label"), str):
            config = holder.get("configuration")
            config_id = config.get("id", "") if isinstance(config, dict) else ""
            if not isinstance(config_id, str):
                return None
            return holder["label"], config_id
    return None


def _public_label(label: str) -> bool:
    return (
        (label.startswith("//") or label.startswith("@"))
        and len(label) <= 512
        and bool(_PATTERN.fullmatch(label))
    )


def _bep_events(bep_path: Path):
    with bep_path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except ValueError as exc:
                raise CoverageProofError("build-event stream contains invalid JSON") from exc
            if not isinstance(event, dict):
                raise CoverageProofError("build-event stream contains a non-object event")
            yield event

def _summary_entry(event: dict) -> tuple[tuple[str, str], str] | None:
    if "testSummary" not in event:
        return None
    identity = _label(event.get("id"))
    summary = event["testSummary"]
    status = summary.get("overallStatus") if isinstance(summary, dict) else None
    if (
        identity is None
        or not _public_label(identity[0])
        or not _CONFIGURATION.fullmatch(identity[1])
        or not isinstance(status, str)
    ):
        raise CoverageProofError("build-event stream has an invalid test summary")
    if status not in {"PASSED", "FLAKY", "FAILED", "TIMEOUT", "INCOMPLETE"}:
        raise CoverageProofError("build-event stream has an unknown test status")
    return identity, status


def _skipped_entry(event: dict) -> tuple[str, str] | None:
    aborted = event.get("aborted")
    if not isinstance(aborted, dict) or aborted.get("reason") != "SKIPPED":
        return None
    identity = _label(event.get("id"))
    if identity is None or not _public_label(identity[0]):
        return None
    return identity if _CONFIGURATION.fullmatch(identity[1]) else None


class _BepStatusCollector:
    def __init__(self) -> None:
        self.summaries: dict[tuple[str, str], str] = {}
        self.skipped: set[tuple[str, str]] = set()
        self.finished_successfully = False
        self.saw_last_message = False

    def consume(self, event: dict) -> None:
        event_id = event.get("id")
        if isinstance(event_id, dict) and "buildFinished" in event_id:
            finished = event.get("finished")
            self.finished_successfully = (
                isinstance(finished, dict) and finished.get("overallSuccess") is True
            )
        self.saw_last_message |= event.get("lastMessage") is True

        summary = _summary_entry(event)
        if summary is not None:
            identity, status = summary
            if identity in self.summaries and self.summaries[identity] != status:
                raise CoverageProofError("build-event stream has conflicting test summaries")
            self.summaries[identity] = status
        skipped = _skipped_entry(event)
        if skipped is not None:
            self.skipped.add(skipped)

    def result(self) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
        if not self.finished_successfully or not self.saw_last_message or not self.summaries:
            raise CoverageProofError("build-event stream does not prove a complete successful test run")
        if any(status not in {"PASSED", "FLAKY"} for status in self.summaries.values()):
            raise CoverageProofError("build-event stream contains a failed or incomplete test")
        tests = [
            {"label": label, "configuration": config, "status": status}
            for (label, config), status in sorted(self.summaries.items())
        ]
        skipped_targets = [
            {"label": label, "configuration": config, "status": "SKIPPED"}
            for label, config in sorted(self.skipped.difference(self.summaries))
        ]
        return tests, skipped_targets


def test_statuses(bep_path: Path) -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    """Read only label/status fields; never copy BEP file URIs into an artifact."""
    collector = _BepStatusCollector()
    for event in _bep_events(bep_path):
        collector.consume(event)
    return collector.result()


def _record_source(record: list[str]) -> str:
    sources = [line[3:].strip() for line in record if line.startswith("SF:")]
    if len(sources) != 1:
        raise CoverageProofError("filtered LCOV record must name one source file")
    source = sources[0]
    path = Path(source)
    if (
        path.is_absolute()
        or ".." in path.parts
        or not re.fullmatch(r"donner/[A-Za-z0-9_./+\-]+", source)
    ):
        raise CoverageProofError("filtered LCOV contains a non-public source path")
    return source


def _record_lines(record: list[str]) -> list[int]:
    numbers: set[int] = set()
    for line in record:
        if not line.startswith("DA:"):
            continue
        try:
            line_number = int(line[3:].split(",", 1)[0])
        except ValueError as exc:
            raise CoverageProofError("filtered LCOV has an invalid line number") from exc
        if line_number <= 0:
            raise CoverageProofError("filtered LCOV has an invalid line number")
        numbers.add(line_number)
    return sorted(numbers)


def _line_universe(report: Path) -> list[dict[str, object]]:
    """Keep exact executable line numbers while excluding LCOV's other metadata."""
    files: list[dict[str, object]] = []
    record: list[str] = []
    with report.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            record.append(line.rstrip("\n"))
            if line.strip() == "end_of_record":
                files.append({"source_file": _record_source(record), "lines": _record_lines(record)})
                record = []
    if record:
        raise CoverageProofError("filtered LCOV ends with an incomplete record")
    if not files:
        raise CoverageProofError("filtered LCOV contains no source files")
    if len({entry["source_file"] for entry in files}) != len(files):
        raise CoverageProofError("filtered LCOV repeats a source file record")
    return sorted(files, key=lambda entry: entry["source_file"])


def _file_coverage(metrics: LcovMetrics, universe: list[dict[str, object]]) -> list[dict[str, object]]:
    files = sorted(metrics.files, key=lambda file: file.source_file)
    if [file.source_file for file in files] != [entry["source_file"] for entry in universe]:
        raise CoverageProofError("filtered LCOV file counters do not match the public file universe")
    return [
        {
            "source_file": file.source_file,
            "executable_lines": file.codecov_lines.total,
            "fully_covered_lines": file.codecov_lines.hits,
            "partial_lines": file.codecov_lines.partials,
            "missed_lines": file.codecov_lines.misses,
            "branches_hit": file.branches.hit,
            "branches_found": file.branches.found,
        }
        for file in files
    ]


def _selection_scope(
    *,
    reason: str,
    patterns: str,
    expected_pattern_count: int,
    event: str,
    ref: str,
    revision: str,
) -> tuple[list[str], str]:
    selected = patterns.split()
    if not selected or any(not _PATTERN.fullmatch(pattern) for pattern in selected):
        raise CoverageProofError("coverage target selection contains an invalid pattern")
    if len(selected) != expected_pattern_count:
        raise CoverageProofError("coverage target selection count does not match its pattern list")
    if not re.fullmatch(r"[a-z_]+", reason):
        raise CoverageProofError("coverage target selection has an invalid reason")
    if not re.fullmatch(r"[A-Za-z0-9_./-]+", ref) or not _REVISION.fullmatch(revision):
        raise CoverageProofError("coverage run has an invalid source identity")

    on_main = ref == "refs/heads/main" and event in {"push", "workflow_dispatch"}
    if on_main and (reason != "non_pr" or selected != ["//donner/..."]):
        raise CoverageProofError("main coverage selected less than the complete product target tree")
    return selected, "complete-main" if on_main else "partial-pr"


def make_proof(
    *,
    bep: Path,
    report: Path,
    reason: str,
    patterns: str,
    expected_pattern_count: int,
    event: str,
    ref: str,
    revision: str,
) -> dict[str, object]:
    selected, scope = _selection_scope(
        reason=reason,
        patterns=patterns,
        expected_pattern_count=expected_pattern_count,
        event=event,
        ref=ref,
        revision=revision,
    )

    universe = _line_universe(report)
    tests, skipped = test_statuses(bep)
    metrics = collect_lcov_metrics(report)
    if not metrics.files or not metrics.codecov_lines.total:
        raise CoverageProofError("filtered LCOV has no executable line universe")
    if (
        len(universe) != len(metrics.files)
        or sum(len(item["lines"]) for item in universe) != metrics.codecov_lines.total
    ):
        raise CoverageProofError("filtered LCOV universe does not match its coverage counters")

    return {
        "schema": 1,
        "scope": scope,
        "revision": revision,
        "ref": ref,
        "event": event,
        "selection": {
            "reason": reason,
            "patterns": selected,
            "pattern_count": len(selected),
        },
        "test_statuses": tests,
        "skipped_targets": skipped,
        "test_counts": dict(sorted(Counter(test["status"] for test in tests).items())),
        "line_universe": universe,
        "file_coverage": _file_coverage(metrics, universe),
        "report": {
            "source_files": len(metrics.files),
            "executable_lines": metrics.codecov_lines.total,
            "fully_covered_lines": metrics.codecov_lines.hits,
            "partial_lines": metrics.codecov_lines.partials,
            "missed_lines": metrics.codecov_lines.misses,
            "raw_executed_lines": metrics.lines.hit,
            "branches_hit": metrics.branches.hit,
            "branches_found": metrics.branches.found,
        },
        "bep_sha256": _digest(bep),
        "filtered_lcov_sha256": _digest(report),
    }


def summary_markdown(proof: dict[str, object]) -> str:
    selection = proof["selection"]
    report = proof["report"]
    tests = proof["test_counts"]
    complete = proof["scope"] == "complete-main"
    label = "Complete main baseline" if complete else "Partial PR coverage for patch annotation"
    scope_note = (
        "This report covers the complete selected product tree."
        if complete
        else "This selected-subset report is not a project coverage percentage."
    )
    return "\n".join(
        [
            "## Coverage run proof",
            "",
            f"**{label}.** {scope_note}",
            f"Source revision: `{proof['revision']}`",
            f"Selection reason: `{selection['reason']}`; patterns ({selection['pattern_count']}): "
            + ", ".join(f"`{pattern}`" for pattern in selection["patterns"]),
            f"BEP test summaries: {len(proof['test_statuses'])} "
            f"(passed {tests.get('PASSED', 0)}, flaky {tests.get('FLAKY', 0)}); "
            f"skipped configured targets: {len(proof['skipped_targets'])}.",
            f"Processed LCOV universe: {report['source_files']} source files, "
            f"{report['executable_lines']} executable lines; "
            f"fully covered {report['fully_covered_lines']}, "
            f"partial {report['partial_lines']}, missed {report['missed_lines']}.",
            "The retained proof artifact contains BEP test status, the exact file and line "
            "universe, and per-file line/branch counters; raw runner paths are excluded.",
            "",
        ]
    )


def _io_failure_reason(error: OSError, args: argparse.Namespace) -> str:
    filename = error.filename
    if filename == str(args.bep):
        return "build-event stream could not be read"
    if filename == str(args.report):
        return "filtered LCOV could not be read"
    if filename in {str(args.output), str(args.output.parent)}:
        return "proof output could not be written"
    if args.step_summary is not None and filename == str(args.step_summary):
        return "step summary could not be written"
    return "coverage proof file I/O failed"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bep", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--reason", required=True)
    parser.add_argument("--patterns", required=True)
    parser.add_argument("--expected-pattern-count", type=int, required=True)
    parser.add_argument("--event", required=True)
    parser.add_argument("--ref", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--step-summary", type=Path)
    args = parser.parse_args(argv)
    try:
        proof = make_proof(
            bep=args.bep,
            report=args.report,
            reason=args.reason,
            patterns=args.patterns,
            expected_pattern_count=args.expected_pattern_count,
            event=args.event,
            ref=args.ref,
            revision=args.revision,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(proof, indent=2) + "\n", encoding="utf-8")
        if args.step_summary is not None:
            with args.step_summary.open("a", encoding="utf-8") as stream:
                stream.write(summary_markdown(proof))
    except (OSError, ValueError) as error:
        if isinstance(error, CoverageProofError):
            reason = str(error)
        elif isinstance(error, OSError):
            reason = _io_failure_reason(error, args)
        else:
            reason = "coverage data is malformed"
        print(f"ERROR: coverage proof could not verify the run inputs: {reason}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
