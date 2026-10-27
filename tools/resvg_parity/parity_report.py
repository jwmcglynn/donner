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
ACTIVE_COMPARISON_MODES = (("tiny-golden", "tiny-skia"), ("geode-golden", "geode"))
ACTIVE_MODE_UNION = tuple(mode for mode, _ in ACTIVE_COMPARISON_MODES)


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
        _check_params_expression(params, f"params expression for {category}/{filename}")
        overrides[filename] = params
    return overrides


# The only calls a params expression may chain at its top level. Skip and render-only state must
# come from the named factories, whose reasons this report and its policy checks read; any other
# factory, helper or builder method could set that state where the report cannot see it. Call
# arguments only supply values, so they are not checked. Extend these sets when
# ImageComparisonParams (ImageComparisonTestFixture.h) or resvg_test_suite.cc gains a new one.
PARAMS_FACTORIES = {"Skip", "UndefinedBehavior", "DeprecatedInSVG2", "WithThreshold",
                    "WithGoldenOverride", "RenderOnly"}
PARAMS_FREE_HELPERS = {"Params", "WithMaxPixels"}
PARAMS_BUILDER_METHODS = {
    "withReason", "withGeodeGoldenOverride", "includeAntiAliasingDifferences",
    "enableGoldenUpdateFromEnv", "setCanvasSize", "disableBackend", "requireFeature",
    "withSimpleTextMaxPixels", "withMaxPixelsDifferent", "withGeodeMaxPixelsDifferent",
    "onlyTextFull",
}
_PARAMS_TOKEN_RE = re.compile(r"(\.\s*)?(?<![\w:])((?:[A-Za-z_]\w*::)*[A-Za-z_]\w*)(\s*[({])?")
_STATE_FIELD_RE = re.compile(r"\b(renderOnly\w*|skip\w*)\b")


def _unknown_params_token(match: re.Match[str]) -> bool:
    method, name, opener = match.groups()
    opener = (opener or "").strip()
    if method:
        return not (opener == "(" and name in PARAMS_BUILDER_METHODS)
    if opener == "(":
        factory = name.removeprefix("Params::")
        is_factory = factory != name and factory in PARAMS_FACTORIES
        return name not in PARAMS_FREE_HELPERS and not is_factory
    return opener != "{" or name != "Params" or not match.string.startswith("}", match.end())


def _blank_call_arguments(text: str) -> str:
    """Blank everything inside parentheses, keeping the parentheses and offsets."""

    result = list(text)
    depth = 0
    for index, character in enumerate(text):
        depth -= character == ")"
        if depth > 0:
            result[index] = " "
        depth += character == "("
    return "".join(result)


def _check_params_expression(expression: str, where: str) -> None:
    """Reject params expressions whose skip or render-only state the report cannot read."""

    masked = _mask_string_literals(_mask_comments(expression)).strip()
    field = _STATE_FIELD_RE.search(masked)
    if field is not None:
        raise ParityReportError(
            f"set {field.group(1)!r} through Params::Skip, Params::UndefinedBehavior or "
            f"Params::DeprecatedInSVG2, not the field: {expression[:60]!r}"
        )
    if masked == "{}":
        return
    top_level = _PARAMS_TOKEN_RE.finditer(_blank_call_arguments(masked))
    unknown = next(
        (token.group(0).strip() for token in top_level if _unknown_params_token(token)), None
    )
    if unknown is not None or "=" in masked or not masked.startswith(tuple(PARAMS_FREE_HELPERS)):
        raise ParityReportError(
            f"unrecognized {where} ({unknown or 'not a Params expression'}): {expression[:60]!r}"
        )


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
    _check_params_expression(default_params, f"default params expression for {category!r}")
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


# Factories that render a case without comparing pixels, mapped to the bucket the report uses.
# `RenderOnly` is the retired generic factory: it still parses so the policy check can name it.
RENDER_ONLY_FACTORIES = {
    "UndefinedBehavior": "undefined-behavior",
    "DeprecatedInSVG2": "deprecated-in-svg2",
    "RenderOnly": None,
}

# The positional argument that carries the reason for each reason-bearing call.
_REASON_ARGUMENT_INDEX = {
    "Skip": 0,
    **{name: 0 for name in RENDER_ONLY_FACTORIES},
    "WithThreshold": 2,
    "WithGoldenOverride": 2,
    "WithMaxPixels": 1,
    "withReason": 0,
    "withGeodeGoldenOverride": 1,
}
_REASON_CALLS = set(_REASON_ARGUMENT_INDEX)


def _reason_from_params(expression: str) -> str | None:
    reason: str | None = None
    for _, name, arguments in sorted(_iter_named_calls(expression, _REASON_CALLS)):
        argument_index = _REASON_ARGUMENT_INDEX[name]
        if len(arguments) <= argument_index:
            continue
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


def _mask_string_literals(text: str) -> str:
    """Blank string and character literal contents while preserving offsets."""

    result = list(text)
    index = 0
    while index < len(text):
        if text[index] in {'"', "'"}:
            end = _skip_quoted(text, index)
            for position in range(index + 1, end - 1):
                result[position] = " "
            index = end
        else:
            index += 1
    return "".join(result)


def _primary_state_from_params(expression: str) -> str:
    # _check_params_expression has already rejected state set outside the named factories.
    skip = _contains_call(expression, {"Skip"})
    render_only = _contains_call(expression, set(RENDER_ONLY_FACTORIES))
    if skip and render_only:
        raise ParityReportError("params expression cannot be both skip and render-only")
    return "skip" if skip else "render-only" if render_only else "compare"


def _render_only_kind_from_params(expression: str) -> str | None:
    """Return the render-only bucket, or None for a comparison, skip or retired factory."""

    names = {name for _, name, _ in _iter_named_calls(expression, set(RENDER_ONLY_FACTORIES))}
    if len(names) > 1:
        raise ParityReportError(f"params expression uses several render-only factories: {names}")
    return RENDER_ONLY_FACTORIES[names.pop()] if names else None


def _required_features_from_params(expression: str, category: str) -> tuple[set[str], bool, bool]:
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

    return required_features, only_text_full, bool(require_calls)


def _disabled_backends_from_params(expression: str) -> set[str]:
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
    return disabled_backends


def _exception_types_from_params(
    expression: str, only_text_full: bool, has_backend_gate: bool
) -> list[str]:
    exception_flags = (
        (_contains_call(expression, {"WithThreshold", "WithMaxPixels", "withMaxPixelsDifferent"}),
         "threshold"),
        (_contains_call(expression, {"withSimpleTextMaxPixels"}), "simple_text_threshold"),
        (_contains_call(expression, {"withGeodeMaxPixelsDifferent"}), "geode_threshold"),
        (_contains_call(expression, {"WithGoldenOverride"}), "shared_golden"),
        (_contains_call(expression, {"withGeodeGoldenOverride"}), "geode_golden"),
        (only_text_full, "text_full_gate"),
        (has_backend_gate, "backend_gate"),
    )
    return [name for enabled, name in exception_flags if enabled]


def _comparison_modes(disabled_backends: set[str]) -> list[str]:
    return [
        mode
        for mode, backend in ACTIVE_COMPARISON_MODES
        if backend not in disabled_backends
    ]


def _params_facts(expression: str, category: str) -> dict[str, object]:
    primary_state = _primary_state_from_params(expression)
    required_features, only_text_full, has_required_feature = _required_features_from_params(
        expression, category
    )
    disabled_backends = _disabled_backends_from_params(expression)

    exception_types = _exception_types_from_params(
        expression,
        only_text_full,
        has_required_feature or bool(disabled_backends),
    )

    return {
        "backend_requirement_reason": _backend_requirement_reason_from_params(expression),
        "backend_requirements": sorted(required_features),
        "comparison_modes": _comparison_modes(disabled_backends),
        "disabled_backends": sorted(disabled_backends),
        "exception_types": exception_types,
        "primary_state": primary_state,
        "reason": _reason_from_params(expression),
        "render_only_kind": _render_only_kind_from_params(expression),
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


def _check_category_coverage(
    registration_by_category: dict[str, Registration], vendored: dict[str, list[Path]]
) -> None:
    unregistered = sorted(set(vendored) - set(registration_by_category))
    if unregistered:
        raise ParityReportError("unregistered vendored categories: " + ", ".join(unregistered))
    missing_categories = sorted(set(registration_by_category) - set(vendored))
    if missing_categories:
        raise ParityReportError(
            "registered categories missing from vendored corpus: " + ", ".join(missing_categories)
        )


def _pixel_budget_source_count(registration: Registration) -> int:
    """Count the explicit pixel-budget expressions of an active registration."""

    if registration.registration != "active":
        return 0
    expressions = [registration.default_params, *registration.overrides.values()]
    return sum(
        "threshold" in _params_facts(params, registration.category)["exception_types"]
        for params in expressions
    )


def _case_params(registration: Registration, filename: str) -> tuple[str, str]:
    if filename in registration.overrides:
        return registration.overrides[filename], "override"
    if registration.default_params != "{}":
        return registration.default_params, "category-default"
    return "{}", "implicit-default"


def _registration_cases(
    registration: Registration, category_cases: list[Path]
) -> list[dict[str, object]]:
    filenames = {path.name for path in category_cases}
    missing_overrides = sorted(set(registration.overrides) - filenames)
    if missing_overrides:
        raise ParityReportError(
            f"overrides missing from {registration.category!r}: " + ", ".join(missing_overrides)
        )

    cases: list[dict[str, object]] = []
    for relative_path in category_cases:
        params, params_source = _case_params(registration, relative_path.name)
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
    return cases


def _summary(cases: list[dict[str, object]], pixel_budget_sources: int) -> dict[str, int]:
    active_cases = [case for case in cases if case["registration"] == "active"]
    disabled_cases = [case for case in cases if case["registration"] == "disabled"]

    def count_active(predicate: Callable[[dict[str, object]], bool]) -> int:
        return sum(bool(predicate(case)) for case in active_cases)

    def count_exception(name: str) -> int:
        return count_active(lambda case: name in case["exception_types"])

    def count_state(state: str) -> int:
        return count_active(lambda case: case["primary_state"] == state)

    def count_render_only_kind(kind: str) -> int:
        return count_active(lambda case: case["render_only_kind"] == kind)

    return {
        "active_cases": len(active_cases),
        "backend_disabled_cases": count_active(lambda case: bool(case["disabled_backends"])),
        "compare_cases": count_state("compare"),
        "disabled_cases": len(disabled_cases),
        "effective_pixel_budget_cases": count_exception("threshold"),
        "geode_golden_cases": count_exception("geode_golden"),
        "geode_pixel_budget_cases": count_exception("geode_threshold"),
        "pixel_budget_sources": pixel_budget_sources,
        "reference_generation_budget_cases": count_active(
            lambda case: case["primary_state"] == "compare"
            and str(case["reason"] or "").startswith(REFERENCE_GENERATION_PREFIX)
        ),
        "reference_label_glyph_budget_cases": count_active(
            lambda case: case["primary_state"] == "compare"
            and str(case["reason"] or "").startswith(REFERENCE_LABEL_GLYPHS_PREFIX)
        ),
        "render_only_cases": count_state("render-only"),
        "render_only_deprecated_in_svg2_cases": count_render_only_kind("deprecated-in-svg2"),
        "render_only_undefined_behavior_cases": count_render_only_kind("undefined-behavior"),
        "shared_golden_cases": count_exception("shared_golden"),
        "simple_text_pixel_budget_cases": count_exception("simple_text_threshold"),
        "skip_cases": count_state("skip"),
        "text_full_gated_cases": count_exception("text_full_gate"),
        "total_cases": len(cases),
    }


def build_report(source: str, tests_root: Path) -> dict[str, object]:
    """Join parsed source registrations to the vendored SVG corpus."""

    registrations = parse_registrations(source)
    if not registrations:
        raise ParityReportError("no resvg category registrations found")
    vendored = _vendored_cases(tests_root)
    _check_category_coverage({item.category: item for item in registrations}, vendored)

    cases: list[dict[str, object]] = []
    pixel_budget_sources = 0
    for registration in registrations:
        pixel_budget_sources += _pixel_budget_source_count(registration)
        cases.extend(_registration_cases(registration, vendored[registration.category]))
    cases.sort(key=lambda item: str(item["path"]))

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
        "summary": _summary(cases, pixel_budget_sources),
        "cases": cases,
    }


UB_MARKER_REFERENCE = Path("third_party/resvg-test-suite/resources/ub.png")

# `Params::UndefinedBehavior` cases whose vendored reference is not the resvg UB marker image. Each
# entry must be behavior the specification leaves undefined or implementation-defined.
SPEC_UB_ALLOWLIST = {
    # Filter Effects defines an exact Gaussian but no bound on stdDeviation. stdDeviation=1000 is
    # 2500 device pixels; every renderer clamps it to an implementation-defined resource limit
    # (Donner: kMaximumFilterPixelRadius), so the reference records resvg's clamp, not a result
    # the specification determines. The upstream test asserts only that rendering does not crash.
    "filters/feGaussianBlur/huge-stdDeviation.svg",
}

# Features removed from or deprecated in SVG 2 that Donner deliberately does not implement. Every
# `Params::DeprecatedInSVG2` case must fall under one of these keys: a category directory, or one
# case path ending in `.svg`. Keep it to named features so the bucket cannot absorb bugs.
DEPRECATED_IN_SVG2_FEATURES = {
    "filters/enable-background": "enable-background and the BackgroundImage it defines",
    "filters/filter/in=BackgroundAlpha-with-enable-background.svg": "enable-background",
    "filters/filter/in=BackgroundImage-with-enable-background.svg": "enable-background",
    "masking/clip": "CSS `clip` property (deprecated by CSS Masking in favor of clip-path)",
    "painting/display/none-on-tref.svg": "<tref>",
    "painting/fill/icc-color.svg": "icc-color() paint",
    "painting/fill/valid-FuncIRI-with-a-fallback-ICC-color.svg": "icc-color() paint",
    "structure/style-attribute/non-presentational-attribute.svg": "version=\"1.1\" semantics",
    "structure/style/non-presentational-attribute.svg": "version=\"1.1\" semantics",
    "text/dominant-baseline/reset-size.svg": "dominant-baseline: reset-size",
    "text/dominant-baseline/use-script.svg": "dominant-baseline: use-script",
    "text/glyph-orientation-horizontal": "glyph-orientation-horizontal",
    "text/kerning": "kerning property",
    "text/tref": "<tref>",
}


def _active_render_only_cases(report: dict[str, object]) -> list[dict[str, object]]:
    return [
        case
        for case in report["cases"]  # type: ignore[union-attr]
        if case["registration"] == "active" and case["primary_state"] == "render-only"
    ]


def render_only_policy_violations(report: dict[str, object]) -> list[str]:
    """Return render-only cases that do not use a named bucket with an informative reason.

    Rendering without a pixel comparison hides every regression in the case, so it needs one of
    two explicit factories: `Params::UndefinedBehavior` for behavior the specification leaves
    undefined or implementation-defined, or `Params::DeprecatedInSVG2` for features removed from or
    deprecated in SVG 2. Bugs, missing features, reference disagreements and harness mismatches
    are skips.
    """

    violations: list[str] = []
    for case in _active_render_only_cases(report):
        reason = str(case["reason"] or "").strip()
        if case["render_only_kind"] is None:
            violations.append(f"{case['path']}: render-only without Params::UndefinedBehavior or "
                              "Params::DeprecatedInSVG2")
        elif not reason:
            violations.append(f"{case['path']}: {case['render_only_kind']} case has no reason")
    return violations


def undefined_behavior_reference_violations(
    report: dict[str, object],
    tests_root: Path,
    marker_path: Path,
    allowlist: Iterable[str] = SPEC_UB_ALLOWLIST,
) -> list[str]:
    """Return undefined-behavior cases without the UB marker reference, and stale allowlist entries.

    An undefined-behavior case qualifies either because its vendored reference is a byte-for-byte
    copy of the resvg UB marker image, or because `allowlist` lists it as specification-level UB.
    Allowlist entries must be active undefined-behavior cases whose reference is not the marker, so
    the list cannot silently outlive a reclassification.
    """

    try:
        marker = marker_path.read_bytes()
    except OSError as error:
        raise ParityReportError(f"cannot read UB marker reference {marker_path}: {error}") from error

    allowed = set(allowlist)
    undefined: set[str] = set()
    violations: list[str] = []
    for case in _active_render_only_cases(report):
        if case["render_only_kind"] != "undefined-behavior":
            continue
        path = str(case["path"])
        undefined.add(path)
        reference = (tests_root / path).with_suffix(".png")
        try:
            is_marker = reference.read_bytes() == marker
        except OSError:
            violations.append(f"{path}: reference PNG is missing")
            continue
        if is_marker and path in allowed:
            violations.append(f"{path}: allowlisted, but its reference is the UB marker")
        elif not is_marker and path not in allowed:
            violations.append(f"{path}: reference is not the UB marker and the case is not "
                              "allowlisted as specification UB")

    for path in sorted(allowed - undefined):
        violations.append(f"{path}: allowlisted, but not an active undefined-behavior case")
    return sorted(violations)


def deprecated_in_svg2_violations(
    report: dict[str, object], features: dict[str, str] = DEPRECATED_IN_SVG2_FEATURES
) -> list[str]:
    """Return deprecated-in-SVG-2 cases outside the named features, and unused feature keys."""

    used: set[str] = set()
    violations: list[str] = []
    for case in _active_render_only_cases(report):
        if case["render_only_kind"] != "deprecated-in-svg2":
            continue
        path = str(case["path"])
        key = path if path in features else str(case["category"])
        if key in features:
            used.add(key)
        else:
            violations.append(f"{path}: not under a feature in DEPRECATED_IN_SVG2_FEATURES")

    for key in sorted(set(features) - used):
        violations.append(f"{key}: listed in DEPRECATED_IN_SVG2_FEATURES, but no active "
                          "deprecated-in-svg2 case uses it")
    return sorted(violations)


REFERENCE_GENERATION_PREFIX = "Reference rasterizer generation:"
REFERENCE_LABEL_GLYPHS_PREFIX = "Reference label glyphs:"
REFERENCE_BUDGET_PREFIXES = (REFERENCE_GENERATION_PREFIX, REFERENCE_LABEL_GLYPHS_PREFIX)
_ANALYTIC_RESIDUAL = re.compile(r"analytic residual (\d+) px")
_SUPERSAMPLED_RESIDUAL = re.compile(r"supersampled (\d+) px")
_PIXEL_BUDGET = re.compile(r"\b(?:withMaxPixelsDifferent|WithMaxPixels)\s*\(\s*(\d+)")
_REFERENCE_BUDGET_EXCEPTIONS = {"threshold", "text_full_gate", "backend_gate"}
# Supersampled coverage at or below the suite's default pixel allowance.
_GENERATION_SUPERSAMPLED_LIMIT = 100


def _reference_budget_limit(analytic_residual: int) -> int:
    """Return the largest allowed budget: the residual plus 3%, rounded up to 5 pixels."""

    return -(-analytic_residual * 103 // 500) * 5


def _reference_budget_form_violations(case: dict[str, object]) -> list[str]:
    """Return how a reference-qualified case departs from a per-case pixel count."""

    path = case["path"]
    expression = str(case["raw_params_expression"])
    violations: list[str] = []
    if case["params_source"] != "override":
        violations.append(f"{path}: comes from a category default, not a per-case override")
    extra = sorted(set(case["exception_types"]) - _REFERENCE_BUDGET_EXCEPTIONS)
    if extra or _contains_call(expression, {"WithGoldenOverride", "withGeodeGoldenOverride"}):
        violations.append(f"{path}: combines the budget with {', '.join(extra) or 'a golden'}")
    if _contains_call(expression, {"WithThreshold"}):
        violations.append(f"{path}: widens the color threshold instead of counting pixels")
    if _PIXEL_BUDGET.search(expression) is None:
        violations.append(f"{path}: compared without a pixel budget")
    return violations


def _reference_budget_residual_violations(case: dict[str, object], prefix: str) -> list[str]:
    """Return how a reference-qualified case's budget departs from the residuals it states."""

    path = case["path"]
    reason = str(case["reason"])
    budget = _PIXEL_BUDGET.search(str(case["raw_params_expression"]))
    violations: list[str] = []
    analytic = _ANALYTIC_RESIDUAL.search(reason)
    if analytic is None:
        violations.append(f"{path}: reason does not state the analytic residual")
    elif budget is not None:
        limit = _reference_budget_limit(int(analytic.group(1)))
        if int(budget.group(1)) > limit:
            violations.append(f"{path}: budget {budget.group(1)} exceeds the analytic residual "
                              f"{analytic.group(1)} plus 3% ({limit})")
    supersampled = _SUPERSAMPLED_RESIDUAL.search(reason)
    if supersampled is None:
        violations.append(f"{path}: reason does not state the supersampled residual")
    elif prefix == REFERENCE_GENERATION_PREFIX and int(supersampled.group(1)) > (
        _GENERATION_SUPERSAMPLED_LIMIT
    ):
        violations.append(f"{path}: supersampled residual {supersampled.group(1)} px is above the "
                          "default allowance, so the difference is not only the rasterizer "
                          "generation")
    return violations


def reference_budget_violations(report: dict[str, object]) -> list[str]:
    """Return reference-qualified pixel budgets that do not follow the evidence rule.

    Vendored references come from more than one resvg rasterizer generation: newer ones cover edges
    with 4x4 supersampling, older ones match Donner's analytic coverage. A compared case whose only
    measured difference is that generation (`Reference rasterizer generation:`), or the heavier
    label glyphs of one reference set (`Reference label glyphs:`), may carry a pixel budget. The
    budget is a per-case pixel count at the default color threshold with no other exception, no
    larger than the stated analytic residual plus 3% rounded up to 5 pixels, and the reason states
    the residual under 4x4 supersampled coverage too, which must fit the default allowance for the
    generation class.
    """

    violations: list[str] = []
    for case in report["cases"]:  # type: ignore[union-attr]
        reason = str(case["reason"] or "")
        prefix = next((p for p in REFERENCE_BUDGET_PREFIXES if reason.startswith(p)), None)
        if case["registration"] != "active" or prefix is None or case["primary_state"] != "compare":
            continue
        violations += _reference_budget_form_violations(case)
        violations += _reference_budget_residual_violations(case, prefix)
    return sorted(violations)


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
