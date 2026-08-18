#!/usr/bin/env python3
"""Generate a deterministic report for the Donner resvg parity registrations."""

from __future__ import annotations

import argparse
import ast
from collections.abc import Callable
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import sys
from typing import Iterable, Sequence, TextIO


SCHEMA_VERSION = 1
REGISTRATION_SOURCE = Path("donner/svg/renderer/tests/resvg_test_suite.cc")
TESTS_ROOT = Path("third_party/resvg-test-suite/tests")
ACTIVE_MODE_UNION = ("tiny-golden", "geode-golden")


class ParityReportError(ValueError):
    """Raised when the source registrations and vendored corpus disagree."""


@dataclass(frozen=True)
class Registration:
    """One active or intentionally disabled resvg category registration."""

    category: str
    registration: str
    suite_name: str
    overrides: dict[str, str]
    default_params: str


def _skip_quoted(text: str, quote_index: int) -> int:
    quote = text[quote_index]
    index = quote_index + 1
    while index < len(text):
        if text[index] == "\\":
            index += 2
        elif text[index] == quote:
            return index + 1
        else:
            index += 1
    raise ParityReportError("unterminated C++ string or character literal")


def _mask_comments(text: str) -> str:
    """Replace comments with spaces while preserving offsets and newlines."""

    result = list(text)
    index = 0
    while index < len(text):
        if text[index] in {'"', "'"}:
            index = _skip_quoted(text, index)
            continue

        if text.startswith("//", index):
            end = text.find("\n", index)
            if end < 0:
                end = len(text)
            for position in range(index, end):
                result[position] = " "
            index = end
            continue

        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                raise ParityReportError("unterminated C++ block comment")
            end += 2
            for position in range(index, end):
                if result[position] != "\n":
                    result[position] = " "
            index = end
            continue

        index += 1

    return "".join(result)


def _find_matching(text: str, open_index: int, opener: str = "(", closer: str = ")") -> int:
    if open_index >= len(text) or text[open_index] != opener:
        raise ParityReportError(f"expected {opener!r} at offset {open_index}")

    depth = 1
    index = open_index + 1
    while index < len(text):
        if text[index] in {'"', "'"}:
            index = _skip_quoted(text, index)
            continue
        if text[index] == opener:
            depth += 1
        elif text[index] == closer:
            depth -= 1
            if depth == 0:
                return index
        index += 1

    raise ParityReportError(f"unbalanced {opener}{closer} expression")


def _split_top_level(text: str) -> list[str]:
    """Split a C++ expression at top-level commas."""

    pieces: list[str] = []
    start = 0
    stack: list[str] = []
    matching = {")": "(", "]": "[", "}": "{"}
    index = 0
    while index < len(text):
        character = text[index]
        if character in {'"', "'"}:
            index = _skip_quoted(text, index)
            continue
        if character in "([{":
            stack.append(character)
        elif character in ")]}":
            if not stack or stack[-1] != matching[character]:
                raise ParityReportError("unbalanced C++ initializer")
            stack.pop()
        elif character == "," and not stack:
            pieces.append(text[start:index].strip())
            start = index + 1
        index += 1

    if stack:
        raise ParityReportError("unbalanced C++ initializer")
    pieces.append(text[start:].strip())
    return pieces


def _strip_outer_braces(text: str) -> str:
    stripped = text.strip()
    if not stripped.startswith("{"):
        raise ParityReportError(f"expected brace initializer, got {stripped[:40]!r}")
    close_index = _find_matching(stripped, 0, "{", "}")
    if stripped[close_index + 1 :].strip():
        raise ParityReportError("unexpected tokens after brace initializer")
    return stripped[1:close_index]


_CPP_STRING_RE = re.compile(r'(?:u8|u|U|L)?("(?:\\.|[^"\\])*")')


def _parse_cpp_string_expression(expression: str) -> str:
    """Evaluate an expression made only from adjacent ordinary C++ strings."""

    values: list[str] = []
    position = 0
    while position < len(expression):
        whitespace = re.match(r"\s*", expression[position:])
        assert whitespace is not None
        position += whitespace.end()
        if position == len(expression):
            break
        match = _CPP_STRING_RE.match(expression, position)
        if match is None:
            raise ParityReportError(
                f"expected adjacent C++ string literals, got {expression[position:position + 40]!r}"
            )
        try:
            value = ast.literal_eval(match.group(1))
        except (SyntaxError, ValueError) as error:
            raise ParityReportError("invalid C++ string literal") from error
        if not isinstance(value, str):
            raise ParityReportError("C++ string expression did not decode to text")
        values.append(value)
        position = match.end()

    if not values:
        raise ParityReportError("expected a non-empty C++ string expression")
    return "".join(values)


def _normalize_cpp_expression(expression: str) -> str:
    """Collapse formatting whitespace outside literals without changing their contents."""

    result: list[str] = []
    pending_space = False
    index = 0
    while index < len(expression):
        if expression[index].isspace():
            pending_space = bool(result)
            index += 1
            continue
        if pending_space and result[-1] not in "([{:." and expression[index] not in ")]},:." :
            result.append(" ")
        pending_space = False
        if expression[index] in {'"', "'"}:
            end = _skip_quoted(expression, index)
            result.append(expression[index:end])
            index = end
        else:
            result.append(expression[index])
            index += 1
    return "".join(result).strip()


def _iter_named_calls(expression: str, names: Iterable[str]) -> list[tuple[int, str, list[str]]]:
    masked = _mask_comments(expression)
    name_pattern = "|".join(re.escape(name) for name in sorted(names, key=len, reverse=True))
    pattern = re.compile(rf"\b({name_pattern})\s*\(")
    calls: list[tuple[int, str, list[str]]] = []
    for match in pattern.finditer(masked):
        open_index = masked.find("(", match.start(), match.end())
        close_index = _find_matching(masked, open_index)
        calls.append(
            (match.start(), match.group(1), _split_top_level(masked[open_index + 1 : close_index]))
        )
    return calls


def _find_macro_calls(source: str) -> list[tuple[str, list[str]]]:
    masked = _mask_comments(source)
    pattern = re.compile(r"\bINSTANTIATE_TEST_SUITE_P\s*\(")
    calls: list[tuple[str, list[str]]] = []
    for match in pattern.finditer(masked):
        open_index = masked.find("(", match.start(), match.end())
        close_index = _find_matching(masked, open_index)
        macro_text = masked[open_index + 1 : close_index]
        calls.append((macro_text, _split_top_level(macro_text)))
    return calls


def _comment_text_blocks(source: str) -> list[str]:
    blocks: list[str] = []
    line_block = re.compile(r"(?m)(?:^[ \t]*//.*(?:\n|$))+")
    for match in line_block.finditer(source):
        lines = []
        for line in match.group(0).splitlines():
            lines.append(re.sub(r"^[ \t]*// ?", "", line))
        blocks.append("\n".join(lines))

    block_comment = re.compile(r"/\*(.*?)\*/", re.DOTALL)
    for match in block_comment.finditer(source):
        blocks.append(match.group(1))
    return blocks


def _get_tests_call(macro_text: str) -> list[str]:
    calls = _iter_named_calls(macro_text, {"getTestsInCategory"})
    if len(calls) != 1:
        raise ParityReportError(
            "each ImageComparisonTestFixture registration must contain exactly one "
            "getTestsInCategory call"
        )
    return calls[0][2]


def _parse_overrides(expression: str, category: str) -> dict[str, str]:
    if expression.strip() == "{}":
        return {}

    body = _strip_outer_braces(expression)
    overrides: dict[str, str] = {}
    for entry in _split_top_level(body):
        if not entry:
            continue
        fields = _split_top_level(_strip_outer_braces(entry))
        if len(fields) != 2:
            raise ParityReportError(
                f"override in {category!r} must contain filename and params expression"
            )
        filename = _parse_cpp_string_expression(fields[0])
        if not filename.endswith(".svg") or "/" in filename or "\\" in filename:
            raise ParityReportError(f"invalid override filename {filename!r} in {category!r}")
        if filename in overrides:
            raise ParityReportError(f"duplicate override {category}/{filename}")
        params = _normalize_cpp_expression(fields[1])
        if not _is_params_expression(params):
            raise ParityReportError(
                f"unrecognized params expression for {category}/{filename}: {params[:60]!r}"
            )
        overrides[filename] = params
    return overrides


def _is_params_expression(expression: str) -> bool:
    stripped = expression.strip()
    return stripped == "{}" or stripped.startswith(("Params", "WithMaxPixels("))


def _parse_registration(macro_text: str, macro_args: list[str], state: str) -> Registration | None:
    if len(macro_args) < 2 or "ImageComparisonTestFixture" not in macro_args[1]:
        return None
    if len(macro_args) != 4:
        raise ParityReportError("resvg registration macro must contain exactly four arguments")
    if "ActiveComparisonModes" not in macro_text:
        raise ParityReportError("resvg registration is missing ActiveComparisonModes")

    call_args = _get_tests_call(macro_text)
    if not 1 <= len(call_args) <= 3:
        raise ParityReportError("getTestsInCategory must contain one to three arguments")
    category = _parse_cpp_string_expression(call_args[0])
    overrides = _parse_overrides(call_args[1], category) if len(call_args) >= 2 else {}
    default_params = _normalize_cpp_expression(call_args[2]) if len(call_args) == 3 else "{}"
    if not _is_params_expression(default_params):
        raise ParityReportError(
            f"unrecognized default params expression for {category!r}: {default_params[:60]!r}"
        )
    return Registration(
        category=category,
        registration=state,
        suite_name=_normalize_cpp_expression(macro_args[0]),
        overrides=overrides,
        default_params=default_params,
    )


def parse_registrations(source: str) -> list[Registration]:
    """Parse active and intentionally commented-out category registrations."""

    registrations: list[Registration] = []
    for macro_text, macro_args in _find_macro_calls(source):
        registration = _parse_registration(macro_text, macro_args, "active")
        if registration is not None:
            registrations.append(registration)

    for comment_block in _comment_text_blocks(source):
        macro_start = comment_block.find("INSTANTIATE_TEST_SUITE_P")
        if macro_start < 0:
            continue
        # Prose around a commented registration is not C++ and can contain
        # unmatched quotes. Start at the macro, whose body must still parse as
        # a complete balanced C++ invocation.
        for macro_text, macro_args in _find_macro_calls(comment_block[macro_start:]):
            registration = _parse_registration(macro_text, macro_args, "disabled")
            if registration is not None:
                registrations.append(registration)

    seen: dict[str, str] = {}
    for registration in registrations:
        previous = seen.get(registration.category)
        if previous is not None:
            raise ParityReportError(
                f"duplicate category registration {registration.category!r} "
                f"({previous} and {registration.registration})"
            )
        seen[registration.category] = registration.registration
    return sorted(registrations, key=lambda item: item.category)


_REASON_CALLS = {
    "Skip",
    "RenderOnly",
    "WithThreshold",
    "WithGoldenOverride",
    "WithMaxPixels",
    "withReason",
    "withGeodeGoldenOverride",
}


def _reason_from_params(expression: str) -> str | None:
    reason: str | None = None
    for _, name, arguments in sorted(_iter_named_calls(expression, _REASON_CALLS)):
        argument_index: int | None = None
        if name in {"Skip", "RenderOnly", "withReason"} and arguments:
            argument_index = 0
        elif name == "WithThreshold" and len(arguments) >= 3:
            argument_index = 2
        elif name == "WithGoldenOverride" and len(arguments) >= 3:
            argument_index = 2
        elif name in {"WithMaxPixels", "withGeodeGoldenOverride"} and len(arguments) >= 2:
            argument_index = 1

        if argument_index is not None:
            candidate = _parse_cpp_string_expression(arguments[argument_index])
            if candidate:
                reason = candidate
    return reason


def _backend_requirement_reason_from_params(expression: str) -> str | None:
    reason: str | None = None
    for _, _, arguments in sorted(
        _iter_named_calls(expression, {"requireFeature", "disableBackend"})
    ):
        if len(arguments) < 2:
            continue
        candidate = _parse_cpp_string_expression(arguments[1])
        if candidate:
            reason = candidate
    return reason


def _contains_call(expression: str, names: set[str]) -> bool:
    return bool(_iter_named_calls(expression, names))


def _params_facts(expression: str, category: str) -> dict[str, object]:
    skip = _contains_call(expression, {"Skip"})
    render_only = _contains_call(expression, {"RenderOnly"})
    if skip and render_only:
        raise ParityReportError("params expression cannot be both skip and render-only")
    primary_state = "skip" if skip else "render-only" if render_only else "compare"

    pixel_budget = _contains_call(
        expression,
        {
            "WithThreshold",
            "WithMaxPixels",
            "withMaxPixelsDifferent",
        },
    )
    simple_text_pixel_budget = _contains_call(expression, {"withSimpleTextMaxPixels"})
    geode_pixel_budget = _contains_call(expression, {"withGeodeMaxPixelsDifferent"})
    shared_golden = _contains_call(expression, {"WithGoldenOverride"})
    geode_golden = _contains_call(expression, {"withGeodeGoldenOverride"})
    only_text_full = _contains_call(expression, {"onlyTextFull"})

    required_features: set[str] = set()
    if category.startswith("filters/"):
        required_features.add("filter-effects")
    if category.startswith("text/"):
        required_features.add("text")
    if only_text_full:
        required_features.add("text-full")

    require_calls = _iter_named_calls(expression, {"requireFeature"})
    for _, _, arguments in require_calls:
        if not arguments:
            raise ParityReportError("requireFeature is missing its feature argument")
        feature = arguments[0]
        if "TextFull" in feature:
            required_features.add("text-full")
            only_text_full = True
        elif "Text" in feature:
            required_features.add("text")
        elif "FilterEffects" in feature:
            required_features.add("filter-effects")
        else:
            required_features.add(_normalize_cpp_expression(feature))

    disabled_backends: set[str] = set()
    for _, _, arguments in _iter_named_calls(expression, {"disableBackend"}):
        if not arguments:
            raise ParityReportError("disableBackend is missing its backend argument")
        backend = arguments[0]
        if "TinySkia" in backend:
            disabled_backends.add("tiny-skia")
        elif "Geode" in backend:
            disabled_backends.add("geode")
        else:
            raise ParityReportError(f"unknown disabled backend {backend!r}")

    exception_types: list[str] = []
    if pixel_budget:
        exception_types.append("threshold")
    if simple_text_pixel_budget:
        exception_types.append("simple_text_threshold")
    if geode_pixel_budget:
        exception_types.append("geode_threshold")
    if shared_golden:
        exception_types.append("shared_golden")
    if geode_golden:
        exception_types.append("geode_golden")
    if only_text_full:
        exception_types.append("text_full_gate")
    if require_calls or disabled_backends:
        exception_types.append("backend_gate")

    comparison_modes = list(ACTIVE_MODE_UNION)
    if "tiny-skia" in disabled_backends:
        comparison_modes.remove("tiny-golden")
    if "geode" in disabled_backends:
        comparison_modes.remove("geode-golden")

    return {
        "backend_requirement_reason": _backend_requirement_reason_from_params(expression),
        "backend_requirements": sorted(required_features),
        "comparison_modes": comparison_modes,
        "disabled_backends": sorted(disabled_backends),
        "exception_types": exception_types,
        "primary_state": primary_state,
        "reason": _reason_from_params(expression),
    }


def _vendored_cases(tests_root: Path) -> dict[str, list[Path]]:
    if not tests_root.is_dir():
        raise ParityReportError(f"vendored tests root does not exist: {tests_root}")
    categories: dict[str, list[Path]] = {}
    for path in sorted(tests_root.rglob("*.svg")):
        relative = path.relative_to(tests_root)
        if len(relative.parts) < 2:
            raise ParityReportError(f"vendored SVG has no category directory: {relative}")
        category = relative.parent.as_posix()
        categories.setdefault(category, []).append(relative)
    if not categories:
        raise ParityReportError(f"vendored tests root contains no SVG cases: {tests_root}")
    return categories


def build_report(source: str, tests_root: Path) -> dict[str, object]:
    """Join parsed source registrations to the vendored SVG corpus."""

    registrations = parse_registrations(source)
    if not registrations:
        raise ParityReportError("no resvg category registrations found")
    registration_by_category = {item.category: item for item in registrations}
    vendored = _vendored_cases(tests_root)

    unregistered = sorted(set(vendored) - set(registration_by_category))
    if unregistered:
        raise ParityReportError("unregistered vendored categories: " + ", ".join(unregistered))
    missing_categories = sorted(set(registration_by_category) - set(vendored))
    if missing_categories:
        raise ParityReportError(
            "registered categories missing from vendored corpus: " + ", ".join(missing_categories)
        )

    cases: list[dict[str, object]] = []
    pixel_budget_sources = 0
    for registration in registrations:
        category_cases = vendored[registration.category]
        filenames = {path.name for path in category_cases}
        missing_overrides = sorted(set(registration.overrides) - filenames)
        if missing_overrides:
            raise ParityReportError(
                f"overrides missing from {registration.category!r}: " + ", ".join(missing_overrides)
            )

        if registration.registration == "active":
            if "threshold" in _params_facts(registration.default_params, registration.category)[
                "exception_types"
            ]:
                pixel_budget_sources += 1
            pixel_budget_sources += sum(
                "threshold" in _params_facts(params, registration.category)["exception_types"]
                for params in registration.overrides.values()
            )

        for relative_path in category_cases:
            filename = relative_path.name
            if filename in registration.overrides:
                params = registration.overrides[filename]
                params_source = "override"
            elif registration.default_params != "{}":
                params = registration.default_params
                params_source = "category-default"
            else:
                params = "{}"
                params_source = "implicit-default"

            facts = _params_facts(params, registration.category)
            comparison_modes = facts.pop("comparison_modes")
            if registration.registration == "disabled":
                comparison_modes = []
            cases.append(
                {
                    "category": registration.category,
                    "path": relative_path.as_posix(),
                    "registration": registration.registration,
                    "suite_name": registration.suite_name,
                    "params_source": params_source,
                    "raw_params_expression": params,
                    "comparison_modes": comparison_modes,
                    **facts,
                }
            )

    cases.sort(key=lambda item: str(item["path"]))
    active_cases = [case for case in cases if case["registration"] == "active"]
    disabled_cases = [case for case in cases if case["registration"] == "disabled"]

    def count_active(predicate: Callable[[dict[str, object]], bool]) -> int:
        return sum(bool(predicate(case)) for case in active_cases)

    summary = {
        "active_cases": len(active_cases),
        "backend_disabled_cases": count_active(lambda case: bool(case["disabled_backends"])),
        "compare_cases": count_active(lambda case: case["primary_state"] == "compare"),
        "disabled_cases": len(disabled_cases),
        "effective_pixel_budget_cases": count_active(
            lambda case: "threshold" in case["exception_types"]
        ),
        "geode_golden_cases": count_active(
            lambda case: "geode_golden" in case["exception_types"]
        ),
        "geode_pixel_budget_cases": count_active(
            lambda case: "geode_threshold" in case["exception_types"]
        ),
        "pixel_budget_sources": pixel_budget_sources,
        "render_only_cases": count_active(lambda case: case["primary_state"] == "render-only"),
        "shared_golden_cases": count_active(
            lambda case: "shared_golden" in case["exception_types"]
        ),
        "simple_text_pixel_budget_cases": count_active(
            lambda case: "simple_text_threshold" in case["exception_types"]
        ),
        "skip_cases": count_active(lambda case: case["primary_state"] == "skip"),
        "text_full_gated_cases": count_active(
            lambda case: "text_full_gate" in case["exception_types"]
        ),
        "total_cases": len(cases),
    }

    return {
        "schema_version": SCHEMA_VERSION,
        "source": {
            "active_mode_union": list(ACTIVE_MODE_UNION),
            "disabled_categories": [
                item.category for item in registrations if item.registration == "disabled"
            ],
            "registration_source": REGISTRATION_SOURCE.as_posix(),
            "tests_root": TESTS_ROOT.as_posix(),
        },
        "summary": summary,
        "cases": cases,
    }


def generate_report(repo_root: Path) -> dict[str, object]:
    source_path = repo_root / REGISTRATION_SOURCE
    if not source_path.is_file():
        raise ParityReportError(f"registration source does not exist: {source_path}")
    try:
        source = source_path.read_text(encoding="utf-8")
    except OSError as error:
        raise ParityReportError(f"cannot read registration source: {error}") from error
    return build_report(source, repo_root / TESTS_ROOT)


def _default_repo_root() -> Path:
    workspace = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if workspace:
        candidate = Path(workspace)
        if (candidate / REGISTRATION_SOURCE).is_file():
            return candidate
    return Path(__file__).resolve().parents[2]


def _write_json(report: dict[str, object], output: Path | None, stdout: TextIO) -> None:
    serialized = json.dumps(report, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    if output is None:
        stdout.write(serialized)
        return
    try:
        output.write_text(serialized, encoding="utf-8")
    except OSError as error:
        raise ParityReportError(f"cannot write report to {output}: {error}") from error


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=_default_repo_root(),
        help="Donner repository root (default: inferred from this script)",
    )
    parser.add_argument("--output", type=Path, help="write JSON to this file instead of stdout")
    return parser


def main(argv: Sequence[str] | None = None, stdout: TextIO = sys.stdout) -> int:
    args = _argument_parser().parse_args(argv)
    try:
        report = generate_report(args.repo_root.resolve())
        _write_json(report, args.output, stdout)
    except ParityReportError as error:
        print(f"parity_report: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
