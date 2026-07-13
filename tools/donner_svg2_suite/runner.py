#!/usr/bin/env python3
"""Reference runner for the Donner SVG2 test suite (design 0057, Milestone 2).

The runner reads a corpus manifest and an optional renderer profile, stages each
case in a per-case sandbox, invokes a render adapter as an executable plus an
argument array (never a shell string), compares the adapter's PNG output against
the canonical oracle, and emits one explicit status per test as JSON (result-v1)
and JUnit.

Policy precedence follows the design: locked spec facts and required
capabilities, then corpus facts, then the renderer profile, then command-line
selection. Command-line flags may select tests or tighten a comparison budget
(strict audit), but may never relax one; an attempt to relax raises
:class:`ComparisonRelaxationError`.

Statuses are non-overlapping: ``pass``, ``comparison-fail``, ``unsupported``,
``expected-fail``, ``render-only``, ``adapter-error``, ``timeout``, and
``infrastructure-error``. Aggregate process exit status is never used to infer
that individual cases passed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from xml.sax.saxutils import escape, quoteattr

from adapter_protocol import (
    AdapterProtocolError,
    RenderRequest,
    parse_response,
    render_request,
)
from path_safety import UnsafePathError, read_text_capped, resolve_within_root
import png_image


RESULT_SCHEMA = "https://donner.graphics/svg2-suite/result-v1.schema.json"

# Corpus facts define an exact-match budget. A profile may loosen it; the CLI
# may only tighten it.
CORPUS_EXACT_BUDGET = {"threshold": 0.0, "max_mismatched_pixels": 0}

# Per-test status -> whether the case ended in an acceptable terminal state.
# The run fails if any case is not acceptable.
ACCEPTABLE_STATUSES = frozenset({"pass", "unsupported", "expected-fail", "render-only"})

# Per-test status -> JUnit projection.
_JUNIT_KIND = {
    "pass": "success",
    "render-only": "success",
    "unsupported": "skipped",
    "expected-fail": "skipped",
    "comparison-fail": "failure",
    "adapter-error": "error",
    "timeout": "error",
    "infrastructure-error": "error",
}


class ComparisonRelaxationError(Exception):
    """Raised when a command-line flag tries to relax a comparison budget."""


class AdapterTimeout(Exception):
    pass


class AdapterFailure(Exception):
    pass


@dataclass
class RunResult:
    results: list[dict] = field(default_factory=list)
    ok: bool = True


def resolve_comparison(profile_case: dict | None, cli_override: dict | None) -> dict:
    """Resolve the comparison budget with precedence corpus < profile < CLI.

    The CLI may only tighten (lower) a budget. Any attempt to raise the
    threshold or the mismatched-pixel allowance is rejected.
    """

    budget = dict(CORPUS_EXACT_BUDGET)
    if profile_case and "comparison" in profile_case:
        budget.update(profile_case["comparison"])
    if cli_override:
        for key, value in cli_override.items():
            if value > budget[key]:
                raise ComparisonRelaxationError(
                    f"command line may not relax {key} from {budget[key]} to {value}"
                )
            budget[key] = value
    return budget


def compare_png(actual_bytes: bytes, expected_bytes: bytes, budget: dict) -> tuple[bool, dict]:
    """Compare two RGBA PNGs under ``budget`` and return (passed, metrics)."""

    actual_width, actual_height, actual = png_image.decode_rgba(actual_bytes)
    expected_width, expected_height, expected = png_image.decode_rgba(expected_bytes)
    if (actual_width, actual_height) != (expected_width, expected_height):
        return False, {
            "reason": "dimension-mismatch",
            "actual": [actual_width, actual_height],
            "expected": [expected_width, expected_height],
        }

    threshold = budget["threshold"]
    mismatched = 0
    for i in range(0, len(expected), 4):
        diff = max(abs(actual[i + k] - expected[i + k]) for k in range(4)) / 255.0
        if diff > threshold:
            mismatched += 1
    passed = mismatched <= budget["max_mismatched_pixels"]
    return passed, {
        "mismatched_pixels": mismatched,
        "max_mismatched_pixels": budget["max_mismatched_pixels"],
        "threshold": threshold,
    }


def _stage_file(sandbox: Path, relative: str, source: str) -> Path:
    destination = sandbox / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    return destination


def stage_case(corpus_root: Path, test: dict, sandbox: Path) -> tuple[RenderRequest, Path]:
    """Stage one case into ``sandbox`` and return its render request and oracle.

    Only manifest-declared inputs and resources are copied in, so the adapter
    cannot reach anything undeclared. Every source path is confined to the
    corpus root before it is read. The oracle stays in the corpus tree and is
    read only by the runner, never handed to the adapter.
    """

    oracle = test["oracle"]
    if oracle.get("kind") != "png":
        raise UnsafePathError("pilot runner only executes png oracles")

    sandbox.mkdir(parents=True, exist_ok=True)
    input_source = resolve_within_root(corpus_root, test["input"])
    input_destination = _stage_file(sandbox, test["input"], input_source)

    resource_root = sandbox / "resources"
    font_root = sandbox / "fonts"
    resource_root.mkdir(exist_ok=True)
    font_root.mkdir(exist_ok=True)
    for resource in test.get("resources", []):
        _stage_file(sandbox, resource, resolve_within_root(corpus_root, resource))

    oracle_source = Path(resolve_within_root(corpus_root, oracle["path"]))
    output = sandbox / "out.png"

    request = render_request(
        test_id=test["id"],
        input_path=str(input_destination),
        output_path=str(output),
        resource_root=str(resource_root),
        font_root=str(font_root),
        processing_mode="static",
        width=oracle["width"],
        height=oracle["height"],
        capabilities=test["capabilities"],
    )
    return request, oracle_source


def invoke_adapter(adapter_argv: list[str], request: RenderRequest, sandbox: Path, timeout: float):
    request_path = sandbox / "request.json"
    response_path = sandbox / "response.json"
    request_path.write_text(request.to_json(), encoding="utf-8")

    argv = list(adapter_argv) + ["render", "--request", str(request_path), "--response", str(response_path)]
    try:
        completed = subprocess.run(argv, timeout=timeout, capture_output=True, text=True)
    except subprocess.TimeoutExpired as error:
        raise AdapterTimeout(f"adapter timed out after {timeout}s") from error

    if completed.returncode != 0:
        raise AdapterFailure(
            f"adapter exited with status {completed.returncode}: {completed.stderr.strip()[:200]}"
        )
    if not response_path.is_file():
        raise AdapterFailure("adapter produced no response.json")

    response = parse_response(response_path.read_text(encoding="utf-8"))
    if response.status == "error":
        raise AdapterFailure(f"adapter reported error: {response.diagnostics}")
    return response


def run_case(
    corpus_root: Path,
    test: dict,
    profile_case: dict | None,
    adapter_argv: list[str],
    work_dir: Path,
    cli_override: dict | None,
    timeout: float,
) -> tuple[dict, bool]:
    """Run one case and return (result_record, acceptable)."""

    test_id = test["id"]
    expectation = (profile_case or {}).get("expectation", "pass")
    result: dict = {"test": test_id, "spec_requirements": test["spec_requirements"]}

    if expectation == "unsupported":
        result["status"] = "unsupported"
        result["diagnostics"] = (profile_case or {}).get("reason", "")
        return result, True

    # Resolve the comparison budget before doing any work so a CLI relaxation
    # aborts the whole run rather than silently affecting some cases.
    budget = resolve_comparison(profile_case, cli_override)

    sandbox = work_dir / hashlib.sha256(test_id.encode("utf-8")).hexdigest()[:16]
    try:
        request, oracle_source = stage_case(corpus_root, test, sandbox)
    except UnsafePathError as error:
        result["status"] = "infrastructure-error"
        result["diagnostics"] = str(error)
        return result, False

    started = time.monotonic()
    try:
        response = invoke_adapter(adapter_argv, request, sandbox, timeout)
    except AdapterTimeout as error:
        result["status"] = "timeout"
        result["diagnostics"] = str(error)
        return result, False
    except (AdapterFailure, AdapterProtocolError) as error:
        result["status"] = "adapter-error"
        result["diagnostics"] = str(error)
        return result, False
    result["duration_ms"] = (time.monotonic() - started) * 1000.0

    if response.status == "unsupported":
        result["status"] = "unsupported"
        result["diagnostics"] = response.diagnostics
        return result, True

    output_path = Path(request.output)
    if not output_path.is_file():
        result["status"] = "adapter-error"
        result["diagnostics"] = "adapter reported ok but wrote no output image"
        return result, False
    actual_bytes = output_path.read_bytes()
    try:
        actual_width, actual_height, _ = png_image.decode_rgba(actual_bytes)
    except png_image.PngError as error:
        result["status"] = "adapter-error"
        result["diagnostics"] = f"output is not a valid RGBA PNG: {error}"
        return result, False
    if (actual_width, actual_height) != (request.width, request.height):
        result["status"] = "adapter-error"
        result["diagnostics"] = (
            f"output dimensions {actual_width}x{actual_height} do not match "
            f"requested {request.width}x{request.height}"
        )
        return result, False

    if expectation == "render-only":
        result["status"] = "render-only"
        return result, True

    passed, metrics = compare_png(actual_bytes, oracle_source.read_bytes(), budget)
    result["comparison"] = metrics

    if expectation == "expected-fail":
        if passed:
            result["status"] = "comparison-fail"
            result["diagnostics"] = (
                "expected failure unexpectedly matched the oracle; "
                "remove or narrow the profile policy"
            )
            return result, False
        result["status"] = "expected-fail"
        return result, True

    if passed:
        result["status"] = "pass"
        return result, True
    result["status"] = "comparison-fail"
    return result, False


def run_manifest(
    manifest_path: Path,
    adapter_argv: list[str],
    *,
    profile_path: Path | None = None,
    work_dir: Path | None = None,
    cli_override: dict | None = None,
    timeout: float = 30.0,
) -> RunResult:
    corpus_root = manifest_path.resolve().parent
    manifest = json.loads(read_text_capped(manifest_path))

    cases: dict = {}
    if profile_path is not None:
        cases = json.loads(read_text_capped(profile_path)).get("cases", {})

    work_dir = Path(work_dir) if work_dir is not None else Path(tempfile.mkdtemp(prefix="svg2-runner-"))
    run = RunResult()
    for test in manifest["tests"]:
        result, acceptable = run_case(
            corpus_root, test, cases.get(test["id"]), adapter_argv, work_dir, cli_override, timeout
        )
        run.results.append(result)
        run.ok = run.ok and acceptable
    return run


def _sha256_of_file(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def result_document(
    run: RunResult,
    *,
    bundle_digest: str,
    adapter_id: str,
    profile_name: str,
    baseline_formal: str,
    dependency_lock: str,
    editorial_delta: str | None = None,
    adapter_capabilities: list[str] | None = None,
    conformance_classes: list[str] | None = None,
    processing_modes: list[str] | None = None,
) -> dict:
    baseline: dict = {"formal": baseline_formal, "dependency_lock": dependency_lock}
    if editorial_delta is not None:
        baseline["editorial_delta"] = editorial_delta
    document: dict = {
        "schema": RESULT_SCHEMA,
        "bundle": {"digest": bundle_digest},
        "adapter": {"id": adapter_id},
        "profile": profile_name,
        "baseline": baseline,
        "results": run.results,
    }
    if adapter_capabilities is not None:
        document["adapter"]["capabilities"] = adapter_capabilities
    if conformance_classes is not None:
        document["conformance_classes"] = conformance_classes
    if processing_modes is not None:
        document["processing_modes"] = processing_modes
    return document


def junit_document(run: RunResult, *, suite_name: str = "donner-svg2-suite") -> str:
    failures = sum(1 for r in run.results if _JUNIT_KIND[r["status"]] == "failure")
    errors = sum(1 for r in run.results if _JUNIT_KIND[r["status"]] == "error")
    skipped = sum(1 for r in run.results if _JUNIT_KIND[r["status"]] == "skipped")

    lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<testsuite name={quoteattr(suite_name)} tests="{len(run.results)}" '
        f'failures="{failures}" errors="{errors}" skipped="{skipped}">',
    ]
    for result in run.results:
        kind = _JUNIT_KIND[result["status"]]
        name = quoteattr(result["test"])
        message = quoteattr(f"status={result['status']}")
        detail = escape(result.get("diagnostics", ""))
        lines.append(f'  <testcase name={name} classname={quoteattr(suite_name)}>')
        if kind == "skipped":
            lines.append(f"    <skipped message={message}/>")
        elif kind == "failure":
            lines.append(f"    <failure message={message}>{detail}</failure>")
        elif kind == "error":
            lines.append(f"    <error message={message}>{detail}</error>")
        lines.append("  </testcase>")
    lines.append("</testsuite>")
    return "\n".join(lines) + "\n"


def _parse_cli_override(args: argparse.Namespace) -> dict | None:
    override: dict = {}
    if args.strict_audit:
        override.update(CORPUS_EXACT_BUDGET)
    if args.threshold is not None:
        override["threshold"] = args.threshold
    if args.max_mismatched_pixels is not None:
        override["max_mismatched_pixels"] = args.max_mismatched_pixels
    return override or None


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run an SVG2 suite corpus through a render adapter.")
    parser.add_argument("manifest", type=Path)
    parser.add_argument(
        "--adapter",
        required=True,
        nargs="+",
        help="Adapter executable and its fixed leading arguments (argument array, never a shell string).",
    )
    parser.add_argument("--adapter-id", default=None)
    parser.add_argument("--profile", type=Path, default=None)
    parser.add_argument("--baseline-lock", type=Path, default=None)
    parser.add_argument("--out-json", type=Path, default=None)
    parser.add_argument("--out-junit", type=Path, default=None)
    parser.add_argument("--threshold", type=float, default=None)
    parser.add_argument("--max-mismatched-pixels", type=int, default=None)
    parser.add_argument("--strict-audit", action="store_true")
    parser.add_argument("--timeout", type=float, default=30.0)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        run = run_manifest(
            args.manifest,
            args.adapter,
            profile_path=args.profile,
            cli_override=_parse_cli_override(args),
            timeout=args.timeout,
        )
    except ComparisonRelaxationError as error:
        print(f"runner refused to relax a comparison budget: {error}", file=sys.stderr)
        return 2

    baseline_formal = "unknown"
    dependency_lock = "sha256:" + "0" * 64
    editorial_delta = None
    if args.baseline_lock is not None:
        lock = json.loads(args.baseline_lock.read_text(encoding="utf-8"))
        publication = lock["formal_baseline"]["publication_date"].replace("-", "")
        baseline_formal = f"svg2-cr-{publication}"
        dependency_lock = _sha256_of_file(args.baseline_lock)
        editorial_delta = lock["editorial_delta_baseline"]["revision"]

    document = result_document(
        run,
        bundle_digest=_sha256_of_file(args.manifest),
        adapter_id=args.adapter_id or Path(args.adapter[-1]).name,
        profile_name=args.profile.stem if args.profile else "none",
        baseline_formal=baseline_formal,
        dependency_lock=dependency_lock,
        editorial_delta=editorial_delta,
        conformance_classes=["interpreter", "viewer"],
        processing_modes=["static", "secure-static"],
    )

    if args.out_json is not None:
        args.out_json.write_text(json.dumps(document, indent=2, sort_keys=True), encoding="utf-8")
    if args.out_junit is not None:
        args.out_junit.write_text(junit_document(run), encoding="utf-8")
    if args.out_json is None and args.out_junit is None:
        print(json.dumps(document, indent=2, sort_keys=True))

    for result in run.results:
        print(f"{result['status']:20} {result['test']}", file=sys.stderr)
    return 0 if run.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
