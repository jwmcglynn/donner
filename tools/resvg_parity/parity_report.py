#!/usr/bin/env python3
"""Generate a deterministic report for the Donner resvg parity registrations."""

from __future__ import annotations

import argparse
import ast
from collections import Counter
from collections.abc import Callable, Mapping
import csv
import hashlib
from dataclasses import dataclass
import io
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Iterable, Sequence, TextIO
from xml.parsers import expat


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


# A helper in the registration source that returns one params expression, such as
# `ImageComparisonParams Name(std::string_view path) { return Params::...; }`.
_PARAMS_HELPER_RE = re.compile(
    r"\bImageComparisonParams\s+([A-Za-z_]\w*)\s*\(([^()]*)\)\s*\{\s*return\s+(.*?);\s*\}",
    re.DOTALL,
)

ParamsHelpers = dict[str, tuple[list[str], str]]


def parse_params_helpers(source: str) -> ParamsHelpers:
    """Return the params helpers defined in the source: parameter names and returned expression.

    Helpers listed in PARAMS_FREE_HELPERS keep their own handling and are not expanded. Two helpers
    with one name, such as overloads, are rejected: a call could not tell which one it expands.
    """

    helpers: ParamsHelpers = {}
    masked = _mask_comments(source)
    for match in _PARAMS_HELPER_RE.finditer(masked):
        name, parameters, _ = match.groups()
        if name in PARAMS_FREE_HELPERS:
            continue
        names = [
            parameter.split()[-1].lstrip("&*")
            for parameter in _split_top_level(parameters)
            if parameter.strip()
        ]
        if name in helpers:
            raise ParityReportError(f"params helper {name} is defined more than once")
        body = source[match.start(3) : match.end(3)]
        helpers[name] = (names, _normalize_cpp_expression(body))
    return helpers


def _expand_params_helpers(expression: str, helpers: ParamsHelpers) -> str:
    """Replace a leading call to a params helper with the expression it returns.

    The report classifies a case by the factories and builder calls in its params expression, so a
    helper that hides them is substituted with its arguments before any check reads the expression.
    """

    match = re.match(r"([A-Za-z_]\w*)\s*\(", expression)
    if match is None or match.group(1) not in helpers:
        return expression
    open_index = expression.find("(", match.start(), match.end())
    close_index = _find_matching(expression, open_index)
    arguments = [
        argument
        for argument in _split_top_level(expression[open_index + 1 : close_index])
        if argument.strip()
    ]
    names, body = helpers[match.group(1)]
    if len(arguments) != len(names):
        raise ParityReportError(
            f"params helper {match.group(1)} takes {len(names)} arguments: {expression[:60]!r}"
        )
    nested = _iter_named_calls(body, helpers)
    if nested:
        raise ParityReportError(
            f"params helper {match.group(1)} calls params helper {nested[0][1]}; "
            "a helper must return factories and builder calls only"
        )
    substitutions = dict(zip(names, arguments))
    masked = _mask_string_literals(body)
    pieces: list[str] = []
    last = 0
    for token in re.finditer(r"\b[A-Za-z_]\w*\b", masked):
        if token.group(0) in substitutions:
            pieces.append(body[last : token.start()])
            pieces.append(substitutions[token.group(0)])
            last = token.end()
    pieces.append(body[last:])
    return _normalize_cpp_expression("".join(pieces) + expression[close_index + 1 :])


def _parse_overrides(
    expression: str, category: str, helpers: ParamsHelpers | None = None
) -> dict[str, str]:
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
        params = _expand_params_helpers(_normalize_cpp_expression(fields[1]), helpers or {})
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
PARAMS_FREE_HELPERS = {"Params", "WithMaxPixels", "ResvgV049Reference"}
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


def _parse_registration(
    macro_text: str, macro_args: list[str], state: str, helpers: ParamsHelpers
) -> Registration | None:
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
    overrides = _parse_overrides(call_args[1], category, helpers) if len(call_args) >= 2 else {}
    default_params = (
        _expand_params_helpers(_normalize_cpp_expression(call_args[2]), helpers)
        if len(call_args) == 3
        else "{}"
    )
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
    helpers = parse_params_helpers(source)
    for macro_text, macro_args in _find_macro_calls(source):
        registration = _parse_registration(macro_text, macro_args, "active", helpers)
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
            registration = _parse_registration(macro_text, macro_args, "disabled", helpers)
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
        features = {
            "RendererBackendFeature::Text": "text",
            "RendererBackendFeature::TextFull": "text-full",
            "RendererBackendFeature::FilterEffects": "filter-effects",
            "RendererBackendFeature::AsciiSnapshot": "ascii-snapshot",
        }
        feature = features.get(_normalize_cpp_expression(arguments[0]))
        if feature is None:
            raise ParityReportError(f"unknown required feature: {arguments[0]!r}")
        required_features.add(feature)
        only_text_full |= feature == "text-full"

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


# The normal Bazel corpus wrappers select these backend/text combinations.
# Text and TextFull are deliberately false in GeodeSupportsFeature for this harness.
CORPUS_LANES = {
    "default_text": ("tiny-golden", frozenset({"text", "filter-effects", "ascii-snapshot"})),
    "max": ("tiny-golden", frozenset({"text", "text-full", "filter-effects", "ascii-snapshot"})),
    "geode": ("geode-golden", frozenset({"filter-effects", "ascii-snapshot"})),
}
DEFAULT_THRESHOLD = 0.02
DEFAULT_MAX_PIXELS = 100


def _number(expression: str, *, integer: bool = False) -> int | float | None:
    constants = {"kDefaultThreshold": DEFAULT_THRESHOLD,
                 "kDefaultMismatchedPixels": DEFAULT_MAX_PIXELS}
    if expression in constants:
        return constants[expression]
    try:
        return int(expression) if integer else float(expression.removesuffix("f"))
    except ValueError:
        # Raw C++ expressions are still reportable. Strict policies reject unresolved effective
        # settings; the static inventory never guesses the runtime value.
        return None


def _pixel_budget(expression: str) -> int | float | None:
    return _number(expression, integer=True)


_ARGUMENT_SETTINGS = {
    "WithThreshold": (("threshold", _number), ("max_pixels", _pixel_budget)),
    "WithMaxPixels": (("max_pixels", _pixel_budget),),
    "WithGoldenOverride": (("shared_golden", _parse_cpp_string_expression), ("threshold", _number)),
    "withGeodeGoldenOverride": (("geode_golden", _parse_cpp_string_expression),),
    "withMaxPixelsDifferent": (("max_pixels", _pixel_budget),),
    "withSimpleTextMaxPixels": (("simple_max_pixels", _pixel_budget),),
    "withGeodeMaxPixelsDifferent": (("geode_max_pixels", _pixel_budget),),
}
_CONSTANT_SETTINGS = {
    "includeAntiAliasingDifferences": {"include_antialiasing": True},
    "ResvgV049Reference": {"canvas_size": [300, 300]},
}
_SETTING_CALLS = {*_ARGUMENT_SETTINGS, *_CONSTANT_SETTINGS, "setCanvasSize"}


def _settings_for_call(name: str, args: list[str]) -> dict[str, object]:
    if name == "setCanvasSize":
        return {"canvas_size": [_pixel_budget(arg) for arg in args]}
    if name in _CONSTANT_SETTINGS:
        return _CONSTANT_SETTINGS[name]
    return {field: convert(arg) for (field, convert), arg in zip(_ARGUMENT_SETTINGS[name], args)}


def _check_numeric_settings(settings: dict[str, object], expression: str) -> None:
    threshold = settings["threshold"]
    budget = settings["max_pixels"]
    canvas = settings["canvas_size"]
    if ((threshold is not None and (not math.isfinite(threshold) or not 0 <= threshold <= 1))
            or (budget is not None and budget < 0)
            or len(canvas) != 2 or any(value is not None and value <= 0 for value in canvas)):
        raise ParityReportError(f"invalid numeric comparison settings: {expression!r}")


def _settings_from_params(expression: str) -> dict[str, object]:
    settings = {
        "threshold": DEFAULT_THRESHOLD,
        "max_pixels": DEFAULT_MAX_PIXELS,
        "simple_max_pixels": -1,
        "geode_max_pixels": -1,
        "shared_golden": None,
        "geode_golden": None,
        "include_antialiasing": False,
        "canvas_size": [500, 500],
    }
    for _, name, args in sorted(_iter_named_calls(expression, _SETTING_CALLS)):
        settings.update(_settings_for_call(name, args))
    _check_numeric_settings(settings, expression)
    return settings


def _lane_state(case: dict[str, object], backend: str, missing: list[str]) -> str:
    if case["registration"] != "active":
        return "unregistered"
    if case["primary_state"] == "skip":
        return "skip"
    if backend in case["disabled_backends"]:
        return "backend-disabled"
    if missing:
        return "capability-gated"
    return case["primary_state"]


def _lane_budget(config: dict[str, object], lane: str) -> int | None:
    max_pixels = config["max_pixels"]
    simple_pixels = config["simple_max_pixels"]
    geode_pixels = config["geode_max_pixels"]
    if lane != "max" and (simple_pixels is None or simple_pixels >= 0):
        max_pixels = simple_pixels
    if lane == "geode" and (geode_pixels is None or geode_pixels >= 0):
        max_pixels = geode_pixels
    return max_pixels


def _lane_reference(path: str, config: dict[str, object], geode: bool) -> dict[str, str]:
    golden = (config["geode_golden"] if geode else None) or config["shared_golden"]
    if golden:
        return {
            "path": golden,
            "provenance_catalog": "donner/svg/renderer/testdata/golden/README.md",
        }
    return {
        "path": (TESTS_ROOT / path).with_suffix(".png").as_posix(),
        "provenance_catalog": "third_party/resvg-test-suite/BUILD.bazel",
    }


def _case_lanes(case: dict[str, object]) -> dict[str, object]:
    config = _settings_from_params(case["raw_params_expression"])
    lanes = {}
    for lane, (mode, capabilities) in CORPUS_LANES.items():
        geode = lane == "geode"
        missing = sorted(set(case["backend_requirements"]) - capabilities)
        lanes[lane] = {
            "mode": mode,
            "text_configuration": "full" if lane == "max" else "simple",
            "configured_state": _lane_state(case, "geode" if geode else "tiny-skia", missing),
            "missing_features": missing,
            "settings": {
                "canvas_size": config["canvas_size"],
                "include_antialiasing": config["include_antialiasing"],
                "max_pixels": _lane_budget(config, lane),
                "threshold": config["threshold"],
                "reference": _lane_reference(case["path"], config, geode),
            },
            "observed": "unobserved",
        }
    return lanes


def _runtime_name(case: dict[str, object], lane: str) -> tuple[str, str]:
    stem = re.sub(r"[^0-9A-Za-z]", "_", Path(case["path"]).stem)
    if lane == "geode":
        stem += "_GeodeGolden"
    if (case["primary_state"] == "skip"
            or (lane != "max" and _contains_call(case["raw_params_expression"], {"onlyTextFull"}))):
        stem = "DISABLED_" + stem
    return (f"{case['suite_name']}/ImageComparisonTestFixture", "ResvgTest/" + stem)


def _test_outcome(
    attributes: dict[str, str], children: set[str], filename: str
) -> tuple[tuple[str, str], str]:
    suite, name = attributes.get("classname"), attributes.get("name")
    if not suite or not name:
        raise ParityReportError(f"unnamed testcase in {filename}")
    status, result = attributes.get("status"), attributes.get("result")
    if children & {"failure", "error"}:
        return (suite, name), "failed"
    if status == "run" and "skipped" in children:
        return (suite, name), "skipped"
    outcome = {
        ("notrun", "suppressed"): "suppressed",
        ("run", "skipped"): "skipped",
        ("run", "completed"): "passed",
    }.get((status, result))
    if outcome is None:
        raise ParityReportError(f"unknown execution status for {suite}/{name}")
    return (suite, name), outcome


class _TestXmlResults:
    """Collect testcase tags and direct status markers without loading XML entities or text."""

    def __init__(self, filename: str) -> None:
        self.filename = filename
        self.root = None
        self.depth = 0
        self.case_depth = 0
        self.attributes = None
        self.children = set()
        self.results = []

    def start(self, name: str, attributes: dict[str, str]) -> None:
        self.depth += 1
        if self.depth == 1:
            self.root = name
        if name == "testcase":
            if self.attributes is not None:
                raise ParityReportError(f"nested testcase in {self.filename}")
            self.case_depth = self.depth
            self.attributes = attributes
            self.children.clear()
        elif self.attributes is not None and self.depth == self.case_depth + 1:
            self.children.add(name)

    def end(self, name: str) -> None:
        if name == "testcase":
            self.results.append(_test_outcome(self.attributes, self.children, self.filename))
            self.attributes = None
        self.depth -= 1


def _reject_xml_declaration(*_args) -> None:
    raise ParityReportError("DTD and entity declarations are not accepted in corpus test XML")


def _parse_test_xml(data: bytes, filename: str) -> list[tuple[tuple[str, str], str]]:
    result = _TestXmlResults(filename)
    parser = expat.ParserCreate()
    parser.StartElementHandler = result.start
    parser.EndElementHandler = result.end
    parser.StartDoctypeDeclHandler = _reject_xml_declaration
    parser.EntityDeclHandler = _reject_xml_declaration
    parser.ExternalEntityRefHandler = lambda *_args: 0
    parser.Parse(data, True)
    if result.root not in {"testsuites", "testsuite"}:
        raise ParityReportError(f"not GoogleTest XML: {filename}")
    return result.results


def _read_test_xml(path: Path) -> tuple[list[tuple[tuple[str, str], str]], str]:
    limit = 8 * 1024 * 1024
    try:
        with path.open("rb") as stream:
            data = stream.read(limit + 1)
        if len(data) > limit:
            raise ParityReportError(f"unexpected XML document: {path.name}")
        records = _parse_test_xml(data, path.name)
    except (OSError, expat.ExpatError) as error:
        raise ParityReportError(f"cannot read corpus test XML {path.name}: {error}") from error
    return records, hashlib.sha256(data).hexdigest()


def _execution_summary(report: dict[str, object]) -> dict[str, object]:
    summary = {}
    for lane in CORPUS_LANES:
        entries = [case["lanes"][lane] for case in report["cases"]]
        counts = Counter(
            (entry["configured_state"], entry["observed"]) for entry in entries
        )
        states = {}
        for (configured, observed), count in sorted(counts.items()):
            states.setdefault(configured, {})[observed] = count
        summary[lane] = states
    return summary


def _execution_keys(report: dict[str, object], lane: str) -> dict[tuple[str, str], dict[str, object]]:
    keys = {}
    for case in report["cases"]:
        if case["registration"] != "active":
            continue
        name = _runtime_name(case, lane)
        if name in keys:
            raise ParityReportError(f"{lane}: ambiguous testcase {name}")
        keys[name] = case["lanes"][lane]
    return keys


def _settings_unresolved(settings: dict[str, object]) -> bool:
    return (settings["threshold"] is None or settings["max_pixels"] is None
            or None in settings["canvas_size"])


def _check_observation(lane: str, keys: dict, observations: dict, name: tuple, outcome: str) -> None:
    if name not in keys:
        raise ParityReportError(f"{lane}: unexpected runtime testcase {name}")
    if name in observations:
        raise ParityReportError(f"{lane}: duplicate runtime testcase {name}")
    if outcome != "passed":
        return
    entry = keys[name]
    state = entry["configured_state"]
    if state not in {"compare", "render-only"}:
        raise ParityReportError(f"{lane}: passed testcase contradicts the static gate {name}")
    if state == "compare" and _settings_unresolved(entry["settings"]):
        raise ParityReportError(f"{lane}: unresolved comparison settings for {name}")


def _read_observations(lane: str, keys: dict, inputs: Path) -> tuple[dict, list[str]]:
    files = sorted(inputs.glob("*.xml")) if inputs.is_dir() else [inputs]
    if not files:
        raise ParityReportError(f"{lane}: no GoogleTest XML receipts")
    evidence = []
    observations = {}
    for file in files:
        records, digest = _read_test_xml(file)
        evidence.append(digest)
        for name, outcome in records:
            _check_observation(lane, keys, observations, name, outcome)
            observations[name] = outcome
    if not observations:
        raise ParityReportError(f"{lane}: zero runtime test records")
    return observations, evidence


def add_execution(
    report: dict[str, object], lane: str, revision: str, inputs: Path
) -> None:
    """Join caller-attested testcase statuses to one configured lane."""
    if lane not in CORPUS_LANES:
        raise ParityReportError(f"unknown corpus lane: {lane!r}")
    expected_revision = report["source"]["revision"] or report["source"].get("caller_revision")
    if expected_revision != revision or not revision:
        raise ParityReportError(f"{lane}: receipt revision must match the report source revision")
    receipts = report.setdefault("execution_receipts", {})
    if lane in receipts:
        raise ParityReportError(f"repeated corpus lane: {lane}")

    keys = _execution_keys(report, lane)
    observations, evidence = _read_observations(lane, keys, inputs)
    for name, outcome in observations.items():
        keys[name]["observed"] = outcome
        keys[name]["observation_binding"] = "caller-attested"
    receipts[lane] = {
        "asserted_revision": revision,
        "binding": "caller-attested",
        "kind": "google-test-status",
        "xml_sha256": evidence,
    }
    report["lane_summary"] = _execution_summary(report)


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
    for case in cases:
        case["lanes"] = _case_lanes(case)

    report = {
        "schema_version": SCHEMA_VERSION,
        "source": {
            "normal_lanes": {name: {"mode": mode, "capabilities": sorted(capabilities)}
                             for name, (mode, capabilities) in CORPUS_LANES.items()},
            "active_mode_union": list(ACTIVE_MODE_UNION),
            "disabled_categories": [
                item.category for item in registrations if item.registration == "disabled"
            ],
            "registration_source": REGISTRATION_SOURCE.as_posix(),
            "revision": None,
            "registration_sha256": hashlib.sha256(source.encode("utf-8")).hexdigest(),
            "tests_root": TESTS_ROOT.as_posix(),
        },
        "summary": _summary(cases, pixel_budget_sources),
        "cases": cases,
    }
    report["lane_summary"] = _execution_summary(report)
    return report


UB_MARKER_REFERENCE = Path("third_party/resvg-test-suite/resources/ub.png")

# `Params::UndefinedBehavior` cases whose vendored reference is not the resvg UB marker image, each
# mapped to its policy classification and the specification text that leaves the behavior
# undefined or implementation-defined.
SPEC_UB_ALLOWLIST: Mapping[str, tuple[str, str, str]] = {
    # Filter Effects defines an exact Gaussian but no bound on stdDeviation. stdDeviation=1000 is
    # 2500 device pixels; every renderer clamps it to an implementation-defined resource limit
    # (Donner: kMaximumFilterPixelRadius), so the reference records resvg's clamp, not a result
    # the specification determines. The upstream test asserts only that rendering does not crash.
    "filters/feGaussianBlur/huge-stdDeviation.svg": (
        "implementation-resource-limit",
        "https://www.w3.org/TR/filter-effects-1/#feGaussianBlurElement",
        "Archived image records an implementation-dependent blur resource limit.",
    ),
    # SVG 2 aliases glyph-orientation-vertical="0" to upright text, but Noto Sans has no vertical
    # metrics, and CSS Writing Modes 3 section 5.1.1 does not define how a user agent synthesizes
    # them, so the glyph positions are user-agent defined.
    "text/glyph-orientation-vertical/simple-case.svg": (
        "user-agent-defined",
        "https://www.w3.org/TR/css-writing-modes-3/#vertical-font-features",
        "Archived image records one renderer's synthesized vertical metrics.",
    ),
    # CSS Text Decoration 3 leaves the position and thickness of an `auto` underline to the user
    # agent, and the underline is most of the difference between renderers.
    "text/writing-mode/mixed-languages-with-tb-and-underline.svg": (
        "user-agent-defined",
        "https://www.w3.org/TR/css-text-decor-3/#text-underline-position-property",
        "Archived image records one renderer's underline position and thickness.",
    ),
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
    deprecated in SVG 2. Reference disagreements, harness mismatches and API-policy differences
    compare against a Donner-rendered golden; unimplemented features are allowlisted skips.
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


def golden_reason_violations(report: dict[str, object]) -> list[str]:
    """Return active cases that compare against a custom golden without a Params reason."""

    return sorted(
        f"{case['path']}: golden override has no reason; record why the vendored reference is "
        "replaced"
        for case in report["cases"]  # type: ignore[union-attr]
        if case["registration"] == "active"
        and {"shared_golden", "geode_golden"} & set(case["exception_types"])
        and not str(case["reason"] or "").strip()
    )


# The two ratchets below record every comparison setting looser than its limit, per case and per
# configured lane, at its current value. They may only shrink: lower or remove an entry when its
# row improves. A new entry or a larger value needs maintainer approval in the pull request that
# adds it. Pixel budgets in the hundreds are not accepted for new work: fix the engine, or hold
# the row back from the change until it is fixed.
PIXEL_BUDGET_RATCHET_LIMIT = DEFAULT_MAX_PIXELS
PIXEL_BUDGET_ALLOWLIST: dict[str, dict[str, int]] = {
    "filters/filter/in=BackgroundAlpha.svg": {"default_text": 1200, "max": 1200, "geode": 1200},
    "painting/marker/with-an-image-child.svg": {"geode": 500},
    "text/baseline-shift/nested-with-baseline-1.svg": {"default_text": 400, "max": 400},
    "text/baseline-shift/nested-with-baseline-2.svg": {"default_text": 400, "max": 400},
    "text/dominant-baseline/hanging.svg": {"default_text": 350, "max": 350},
    "text/font-variant/inherit.svg": {"default_text": 1200},
    "text/font-variant/small-caps.svg": {"default_text": 1200},
    "text/textLength/zero.svg": {"default_text": 410, "max": 410},
    "text/textPath/closed-path.svg": {"default_text": 400, "max": 400},
}

THRESHOLD_RATCHET_LIMIT = 0.05
THRESHOLD_ALLOWLIST: dict[str, dict[str, float]] = {
    "filters/feDiffuseLighting/complex-transform.svg": {
        "default_text": 0.11, "max": 0.11, "geode": 0.11,
    },
    "filters/feMerge/complex-transform.svg": {"default_text": 0.14, "max": 0.14, "geode": 0.14},
    "filters/fePointLight/complex-transform.svg": {
        "default_text": 0.11, "max": 0.11, "geode": 0.11,
    },
    "text/text-decoration/tspan-decoration.svg": {"default_text": 0.09, "max": 0.09},
    "text/text/compound-emojis-and-coordinates-list.svg": {"max": 0.1},
    "text/text/compound-emojis.svg": {"max": 0.1},
    "text/text/emojis.svg": {"max": 0.1},
}


def _compared_lane_settings(
    report: dict[str, object], setting: str
) -> Iterable[tuple[str, str, float | None]]:
    """Yield (path, lane, value) for every lane of an active case that compares pixels."""

    for case in report["cases"]:  # type: ignore[union-attr]
        if case["registration"] != "active":
            continue
        for lane, entry in case["lanes"].items():
            if entry["configured_state"] == "compare":
                yield str(case["path"]), lane, entry["settings"][setting]


def _ratchet_message(
    where: str, value: float | None, recorded: float | None, limit: float, allowlist_name: str
) -> str | None:
    if value is None:
        return f"{where} is unresolved"
    if recorded is None:
        return (f"{where} is above {limit} and not in {allowlist_name}; new entries need "
                "maintainer approval, so fix the engine instead")
    if value > recorded:
        return (f"{where} is above its recorded {recorded} in {allowlist_name}, which may only "
                "shrink")
    if value < recorded:
        return f"{where} is below its recorded {recorded}; lower the {allowlist_name} entry"
    return None


def _ratchet_violations(
    report: dict[str, object],
    allowlist: Mapping[str, Mapping[str, float]],
    setting: str,
    limit: float,
    allowlist_name: str,
) -> list[str]:
    """Return comparing lanes whose `setting` is above `limit` without a matching entry.

    Each entry must equal its lane's current value, so the list records the live loosening
    exactly: a larger value fails, a smaller one must lower its entry, and an entry whose lane no
    longer compares above the limit is stale.
    """

    violations: list[str] = []
    live: set[tuple[str, str]] = set()
    for path, lane, value in _compared_lane_settings(report, setting):
        if value is not None and value <= limit:
            continue
        live.add((path, lane))
        message = _ratchet_message(f"{path} ({lane}): {setting} {value}", value,
                                   allowlist.get(path, {}).get(lane), limit, allowlist_name)
        if message:
            violations.append(message)

    for path, lanes in sorted(allowlist.items()):
        violations += [
            f"{path} ({lane}): stale {allowlist_name} entry; the lane no longer compares with "
            f"{setting} above {limit}, so remove it"
            for lane in sorted(lanes) if (path, lane) not in live
        ]
    return sorted(violations)


def pixel_budget_ratchet_violations(
    report: dict[str, object], allowlist: Mapping[str, Mapping[str, int]] = PIXEL_BUDGET_ALLOWLIST
) -> list[str]:
    """Return comparing lanes whose mismatched-pixel budget breaks PIXEL_BUDGET_ALLOWLIST."""

    return _ratchet_violations(report, allowlist, "max_pixels", PIXEL_BUDGET_RATCHET_LIMIT,
                               "PIXEL_BUDGET_ALLOWLIST")


def threshold_ratchet_violations(
    report: dict[str, object], allowlist: Mapping[str, Mapping[str, float]] = THRESHOLD_ALLOWLIST
) -> list[str]:
    """Return comparing lanes whose per-pixel color threshold breaks THRESHOLD_ALLOWLIST."""

    return _ratchet_violations(report, allowlist, "threshold", THRESHOLD_RATCHET_LIMIT,
                               "THRESHOLD_ALLOWLIST")


# Every active Params::Skip row. A skipped case does not render, so it loses its regression,
# no-crash and sanitizer coverage. This list may only shrink; a new entry needs maintainer
# approval in the pull request that adds it.
SKIP_ALLOWLIST = frozenset({
    "paint-servers/radialGradient/fr=0.7.svg",
    "structure/svg/not-UTF-8-encoding.svg",
    "text/textPath/with-filter.svg",
    "text/tspan/with-filter.svg",
})

SKIP_RULE = (
    "a reference disagreement, harness mismatch or API-policy difference compares against a "
    "Donner-rendered golden with a recorded justification, not a skip; a skip is only for an "
    "unimplemented feature, `Not impl: <feature> (#<issue>)`, and needs maintainer approval to "
    "join SKIP_ALLOWLIST (donner/svg/renderer/tests/README_resvg_test_suite.md)"
)


def skip_ratchet_violations(
    report: dict[str, object], allowlist: Iterable[str] = SKIP_ALLOWLIST
) -> list[str]:
    """Return active skips missing from `allowlist`, and entries that are no longer skips."""

    allowed = set(allowlist)
    skipped = {
        str(case["path"])
        for case in report["cases"]  # type: ignore[union-attr]
        if case["registration"] == "active" and case["primary_state"] == "skip"
    }
    return [
        f"{path}: Params::Skip is not in SKIP_ALLOWLIST; {SKIP_RULE}"
        for path in sorted(skipped - allowed)
    ] + [
        f"{path}: stale SKIP_ALLOWLIST entry; the case is no longer an active skip, so remove it"
        for path in sorted(allowed - skipped)
    ]


GOLDEN_ROOT = Path("donner/svg/renderer/testdata/golden")
GOLDEN_README = GOLDEN_ROOT / "README.md"
GOLDEN_DIGEST_HEADING = "## Registered golden digests"
_SHA256 = re.compile(r"\b[0-9a-f]{64}\b")


def referenced_goldens(report: dict[str, object]) -> list[str]:
    """Return the repository path of every golden an active registration compares against."""

    paths: set[str] = set()
    for case in report["cases"]:  # type: ignore[union-attr]
        if case["registration"] == "active":
            config = _settings_from_params(str(case["raw_params_expression"]))
            paths.update(config[key] for key in REFERENCE_KINDS if config[key])
    return sorted(paths)


def _golden_table_rows(readme: str) -> tuple[list[str], list[str]]:
    """Return the digest section's table rows and every table row in the README."""

    lines = readme.splitlines()
    if lines.count(GOLDEN_DIGEST_HEADING) != 1:
        raise ParityReportError(f"{GOLDEN_README} needs one {GOLDEN_DIGEST_HEADING!r} section")
    start = lines.index(GOLDEN_DIGEST_HEADING) + 1
    end = next((index for index in range(start, len(lines)) if lines[index].startswith("## ")),
               len(lines))
    rows = [line for line in lines if line.lstrip().startswith("|")]
    return [line for line in lines[start:end] if line.lstrip().startswith("|")], rows


def _golden_digest_violation(
    golden: str, repo_root: Path, digest_rows: list[str], rows: list[str]
) -> str | None:
    path = Path(golden)
    if ".." in path.parts or "." in path.parts or path.is_absolute():
        return f"{golden}: golden path is not a normalized repository path"
    if GOLDEN_ROOT not in path.parents:
        return f"{golden}: golden is outside {GOLDEN_ROOT}"
    try:
        digest = hashlib.sha256((repo_root / path).read_bytes()).hexdigest()
    except OSError:
        return f"{golden}: referenced golden file is missing"
    token = f"`{path.relative_to(GOLDEN_ROOT).as_posix()}`"
    if not any(token in row and digest in _SHA256.findall(row) for row in digest_rows):
        return (f"{golden}: no row in the {GOLDEN_README} {GOLDEN_DIGEST_HEADING!r} table names "
                f"it with its SHA-256 {digest}")
    if any(token in row and digest not in _SHA256.findall(row) for row in rows):
        return f"{golden}: a {GOLDEN_README} row names it without its current SHA-256 {digest}"
    return None


def golden_documentation_violations(report: dict[str, object], repo_root: Path) -> list[str]:
    """Return referenced goldens that the golden README does not list with their SHA-256.

    Each referenced golden needs a row in the README's digest table that names its path relative
    to the golden directory in backticks, such as `geode/name.png` in backticks, and gives the
    committed file's SHA-256. Any other table row in the README that names the golden must carry
    the same digest, so a regenerated file cannot keep a stale entry anywhere.
    """

    try:
        readme = (repo_root / GOLDEN_README).read_text(encoding="utf-8")
    except OSError as error:
        raise ParityReportError(f"cannot read golden README: {error}") from error
    digest_rows, rows = _golden_table_rows(readme)
    violations = (_golden_digest_violation(golden, repo_root, digest_rows, rows)
                  for golden in referenced_goldens(report))
    return [violation for violation in violations if violation]


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


EXCEPTION_POLICIES = Path("tools/resvg_parity/exception_policies.json")
REFERENCE_KINDS = {"shared_golden", "geode_golden"}
NUMERIC_KINDS = {"threshold", "simple_text_threshold", "geode_threshold"}


def _scoped_exception_kinds(case: dict[str, object]) -> set[str]:
    """The row-specific exceptions that need a reviewed decision in the catalog."""
    if case["registration"] != "active":
        return {"unregistered"}
    if case["primary_state"] == "skip":
        return {"skip"}
    if case["primary_state"] == "render-only":
        return set()
    kinds = set(case["exception_types"]) & (NUMERIC_KINDS | REFERENCE_KINDS)
    if any(
        lane["settings"]["threshold"] != DEFAULT_THRESHOLD
        for lane in case["lanes"].values()
        if lane["configured_state"] == "compare"
    ):
        kinds.add("threshold")
    return kinds


DEFINED_MARKER_CASES = {
    "filters/feTile/complex-transform.svg": (
        "known-renderer-divergence", "https://www.w3.org/TR/filter-effects-1/#feTileElement",
    ),
    "filters/feTurbulence/stitchTiles=stitch.svg": (
        "known-renderer-divergence",
        "https://www.w3.org/TR/filter-effects-1/#element-attrdef-feturbulence-stitchtiles",
    ),
    **{
        f"filters/feConvolveMatrix/{stem}.svg": (
            "defined-input-no-reference", "https://www.w3.org/TR/filter-effects-1/#feConvolveMatrixElement",
        )
        for stem in ("bias=-0.5", "bias=0.5", "bias=9999", "edgeMode=wrap-with-matrix-larger-than-target")
    },
    **{
        f"filters/filter/in={stem}.svg": (
            "defined-input-no-reference", "https://www.w3.org/TR/filter-effects-1/#attr-valuedef-in-fillpaint",
        )
        for stem in ("FillPaint", "FillPaint-on-g-without-children", "FillPaint-with-gradient",
                     "FillPaint-with-pattern", "FillPaint-with-target-on-g", "StrokePaint")
    },
}


def _marker_policy(path: str) -> dict[str, object]:
    classification = "upstream-no-reference"
    evidence = [UB_MARKER_REFERENCE.as_posix(), "docs/design_docs/0021-resvg_feature_gaps.md"]
    provenance = "Upstream UB-marker image. The marker does not classify the SVG's normative behavior."
    if path in SPEC_UB_ALLOWLIST:
        classification, specification, provenance = SPEC_UB_ALLOWLIST[path]
        evidence = [REGISTRATION_SOURCE.as_posix(), specification]
    elif path in DEFINED_MARKER_CASES:
        classification, specification = DEFINED_MARKER_CASES[path]
        evidence.append(specification)
        if classification == "known-renderer-divergence":
            evidence.append("docs/spec_audit/filters_generators_lighting.md")
    return {
        "classification": classification,
        "disposition": "Retain no-crash execution while the case lacks a qualified pixel reference.",
        "evidence": evidence,
        "reference_provenance": provenance,
        "removal_criteria": (
            "Verify the fixture against the governing specification and qualify an independent "
            "reference before enabling its pixel comparison. Keep any required no-crash coverage."
        ),
    }


def _capability_policy(case: dict[str, object]) -> dict[str, object] | None:
    has_explicit_gate = bool({"text_full_gate", "backend_gate"} & set(case["exception_types"]))
    still_gated = any(lane["configured_state"] == "capability-gated"
                      for lane in case["lanes"].values())
    if not has_explicit_gate and not still_gated:
        return None
    if has_explicit_gate and not (case["reason"] or case["backend_requirement_reason"]):
        raise ParityReportError(f"{case['path']}: explicit capability gate needs a source reason")
    return {
        "classification": "backend-capability",
        "disposition": "Apply the renderer harness's declared capability requirements.",
        "evidence": [
            REGISTRATION_SOURCE.as_posix(),
            "donner/svg/renderer/tests/RendererTestBackendGeode.cc",
            "donner/svg/renderer/tests/RendererTestBackendTinySkia.cc",
        ],
        "owning_issue": "https://github.com/jwmcglynn/donner/issues/1166",
        "removal_criteria": (
            "Remove a gate after enabling the feature in that normal renderer configuration "
            "and validating the formerly gated comparisons without increasing their limits."
        ),
    }


def _named_policies(case: dict[str, object]) -> list[dict[str, object]]:
    policies = []
    render_only = case["render_only_kind"]
    if render_only == "undefined-behavior":
        policies.append(_marker_policy(case["path"]))
    elif render_only == "deprecated-in-svg2":
        policies.append({
            "classification": render_only,
            "disposition": "Retain no-crash coverage for this deliberately excluded legacy feature.",
            "evidence": ["docs/design_docs/0021-resvg_feature_gaps.md"],
            "removal_criteria": (
                "Replace the render-only policy after explicitly adopting the feature and qualifying "
                "its independent pixel reference, or replacing it with a supported SVG 2 fixture."
            ),
        })
    capability = _capability_policy(case)
    if capability:
        policies.append(capability)
    return policies


def _validate_policy_metadata(rule: dict[str, object], rule_id: str) -> None:
    required = ("classification", "disposition", "removal_criteria", "reference_provenance")
    if any(not isinstance(rule.get(key), str) or not rule[key].strip() for key in required):
        raise ParityReportError(f"{rule_id}: incomplete exception disposition")
    evidence = rule.get("evidence")
    if not isinstance(evidence, list) or not evidence or any(
        not isinstance(item, str) or not item.strip() for item in evidence
    ):
        raise ParityReportError(f"{rule_id}: exception policy lacks evidence")


def _index_policies(document: dict[str, object]) -> dict[str, dict[str, object]]:
    indexed = {}
    ids = set()
    for rule in document["rules"]:
        rule_id = rule["id"]
        if not rule_id or rule_id in ids:
            raise ParityReportError(f"missing or repeated exception rule id: {rule_id!r}")
        ids.add(rule_id)
        _validate_policy_metadata(rule, rule_id)
        for path in rule["paths"]:
            if path in indexed:
                raise ParityReportError(f"{path}: overlapping exception policies")
            indexed[path] = rule
    return indexed


def _validate_scoped_policy(case: dict[str, object], rule: dict[str, object], kinds: set[str]) -> None:
    path = case["path"]
    allowed = set(rule["kinds"])
    if kinds != allowed:
        raise ParityReportError(f"{path}: policy does not cover exception kinds {sorted(kinds)}")
    expected_refs = rule.get("references_by_fixture", {}).get(path, rule.get("references", {}))
    max_threshold, max_pixels = rule.get("bounds_by_fixture", {}).get(
        path, (rule.get("max_threshold"), rule.get("max_pixels"))
    )
    if kinds != {"skip"} and (
        type(max_threshold) not in {float, int} or not math.isfinite(max_threshold)
        or not 0 <= max_threshold <= 1 or type(max_pixels) is not int or max_pixels < 0
    ):
        raise ParityReportError(f"{path}: invalid comparison bounds in policy")
    config = _settings_from_params(case["raw_params_expression"])
    actual_refs = {key: config[key] for key in REFERENCE_KINDS if config[key] is not None}
    if actual_refs != expected_refs:
        raise ParityReportError(f"{path}: unreviewed CPU or Geode reference")
    for lane, entry in case["lanes"].items():
        if entry["configured_state"] != "compare":
            continue
        setting = entry["settings"]
        if _settings_unresolved(setting):
            raise ParityReportError(f"{path} ({lane}): unresolved comparison settings")
        if setting["threshold"] > max_threshold or setting["max_pixels"] > max_pixels:
            raise ParityReportError(f"{path} ({lane}): comparison exceeds the reviewed bounds")


def _check_dash_seam(case: dict[str, object]) -> None:
    path = case["path"]
    ordinary = (TESTS_ROOT / path).with_suffix(".png").as_posix()
    for lane, entry in case["lanes"].items():
        setting = entry["settings"]
        if (entry["configured_state"] != "compare" or setting["threshold"] != 0
                or setting["max_pixels"] != 0 or setting["reference"]["path"] != ordinary):
            raise ParityReportError(f"{path} ({lane}): preserve the exact dash seam comparison")


def _check_catalog_case(case: dict[str, object]) -> None:
    path = case["path"]
    if path == "painting/stroke-dasharray/n-0.svg":
        _check_dash_seam(case)
    if case["primary_state"] == "render-only" and not case["render_only_kind"]:
        raise ParityReportError(f"{path}: render-only case needs a named policy")
    if case["disabled_backends"]:
        raise ParityReportError(f"{path}: unexplained explicit backend disable")
    if case["primary_state"] != "compare" and not case["reason"]:
        raise ParityReportError(f"{path}: exceptional state needs a source reason")


def _scoped_policy(
    case: dict[str, object], rule: dict[str, object] | None
) -> dict[str, object] | None:
    path = case["path"]
    kinds = _scoped_exception_kinds(case)
    if not kinds:
        if rule:
            raise ParityReportError(f"{path}: remove retired exception policy {rule['id']}")
        return None
    if rule is None:
        raise ParityReportError(f"{path}: unexplained exceptions {sorted(kinds)}")
    _validate_scoped_policy(case, rule, kinds)
    fields = {"id", "classification", "disposition", "evidence", "owning_issue",
              "reference_provenance", "removal_criteria"}
    return {**{key: value for key, value in rule.items() if key in fields}, "kinds": sorted(kinds)}


def add_exception_policies(report: dict[str, object], document: dict[str, object]) -> None:
    indexed = _index_policies(document)
    for case in report["cases"]:
        _check_catalog_case(case)
        policies = _named_policies(case)
        scoped = _scoped_policy(case, indexed.pop(case["path"], None))
        if scoped:
            policies.append(scoped)
        if policies:
            case["policies"] = policies
    if indexed:
        raise ParityReportError("policies name absent fixtures: " + ", ".join(sorted(indexed)))


def generate_report(repo_root: Path) -> dict[str, object]:
    source_path = repo_root / REGISTRATION_SOURCE
    if not source_path.is_file():
        raise ParityReportError(f"registration source does not exist: {source_path}")
    try:
        source = source_path.read_text(encoding="utf-8")
    except OSError as error:
        raise ParityReportError(f"cannot read registration source: {error}") from error
    report = build_report(source, repo_root / TESTS_ROOT)
    policy_path = repo_root / EXCEPTION_POLICIES
    if policy_path.is_file():
        try:
            add_exception_policies(report, json.loads(policy_path.read_text(encoding="utf-8")))
        except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
            raise ParityReportError(f"invalid exception policy catalog: {error}") from error
    return report


RESULTS_CSV = Path("third_party/resvg-test-suite/results.csv")
HEAD_TO_HEAD_ENGINE = "resvg"
# Per-engine codes in the suite's results.csv: manual visual judgments recorded at the vendored
# snapshot, not an automated pixel diff.
RESULT_CODE_NAMES = {0: "unknown", 1: "passed", 2: "failed", 3: "crashed"}
# Exceptions that loosen a comparison. Gating a case to a feature tier or backend does not.
_LOOSENING_EXCEPTIONS = {
    "threshold", "simple_text_threshold", "geode_threshold", "shared_golden", "geode_golden",
}


_RESULT_CODES = {str(code): code for code in RESULT_CODE_NAMES}


def _engine_result_row(row: dict[str | None, object], engine: str) -> tuple[str, int]:
    """Return the path and result code of one results.csv row, rejecting malformed rows."""

    path = str(row["title"] or "").strip()
    if not path.endswith(".svg"):
        raise ParityReportError(f"results.csv row has no SVG path: {path!r}")
    if None in row:
        raise ParityReportError(f"results.csv row for {path} has extra columns")
    value = str(row[engine] or "").strip()
    if value not in _RESULT_CODES:
        raise ParityReportError(f"results.csv has invalid {engine} code {value!r} for {path}")
    return path, _RESULT_CODES[value]


def parse_engine_results(text: str, engine: str = HEAD_TO_HEAD_ENGINE) -> dict[str, int]:
    """Return one engine's result code per test path from the suite's results.csv."""

    reader = csv.DictReader(io.StringIO(text))
    if reader.fieldnames is None or "title" not in reader.fieldnames:
        raise ParityReportError("results.csv has no `title` column")
    if engine not in reader.fieldnames:
        raise ParityReportError(f"results.csv has no {engine!r} column")

    results: dict[str, int] = {}
    try:
        for row in reader:
            path, code = _engine_result_row(row, engine)
            if path in results:
                raise ParityReportError(f"duplicate results.csv row {path}")
            results[path] = code
    except csv.Error as error:
        raise ParityReportError(f"malformed results.csv: {error}") from error
    if not results:
        raise ParityReportError("results.csv contains no rows")
    return results


def _gap_state(case: dict[str, object]) -> str:
    if case["registration"] != "active":
        return "disabled"
    if case["primary_state"] == "compare" and not case["comparison_modes"]:
        return "no-backend"
    return str(case["render_only_kind"] or case["primary_state"])


# These reviewed corpus contracts award completion with a nonempty snapshot. Other fixtures
# in the same registration bucket can have defined output but no usable upstream reference.
RENDER_PASS_CONTRACTS = {
    "filters/feGaussianBlur/huge-stdDeviation.svg": "Upstream explicitly requires no crash.",
    "structure/svg/funcIRI-with-invalid-characters.svg":
        "Render the digit-leading ID fixture whose upstream PNG is the UB marker.",
}


def build_head_to_head(
    report: dict[str, object], engine_results: dict[str, int], engine: str = HEAD_TO_HEAD_ENGINE
) -> dict[str, object]:
    """Compare configured Donner outcomes with an engine's published results.

    The score assumes green corpus runs. It includes pixel comparisons and explicitly
    credited render-only contracts. Strict passes count only pixel comparisons without
    a numerical budget or a custom golden. The engine score uses its recorded passes.
    """

    cases = {str(case["path"]): case for case in report["cases"]}  # type: ignore[union-attr]
    missing = sorted(set(cases) - set(engine_results))
    extra = sorted(set(engine_results) - set(cases))
    if missing or extra:
        raise ParityReportError(
            "results.csv and the vendored corpus disagree: "
            f"{len(missing)} cases without a result {missing[:5]}, "
            f"{len(extra)} results without a case {extra[:5]}"
        )

    pixel_passes = {path for path, case in cases.items() if _gap_state(case) == "compare"}
    render_passes = {
        path for path in RENDER_PASS_CONTRACTS
        if path in cases and cases[path]["primary_state"] == "render-only"
        and _gap_state(cases[path]) == "undefined-behavior"
        and cases[path]["comparison_modes"]
    }
    donner_passes = pixel_passes | render_passes
    engine_passes = {path for path, code in engine_results.items() if code == 1}
    strict_passes = {
        path for path in pixel_passes
        if not set(cases[path]["exception_types"]) & _LOOSENING_EXCEPTIONS
    }
    donner_only = sorted(donner_passes - engine_passes)
    gap = sorted(engine_passes - donner_passes)

    def engine_result(path: str) -> str:
        return RESULT_CODE_NAMES[engine_results[path]]

    return {
        "schema_version": SCHEMA_VERSION,
        "source": {
            "engine": engine,
            "engine_results": RESULTS_CSV.as_posix(),
            "registration_source": REGISTRATION_SOURCE.as_posix(),
            "tests_root": TESTS_ROOT.as_posix(),
        },
        "summary": {
            "both_pass": len(donner_passes & engine_passes),
            "donner_only": len(donner_only),
            "donner_only_by_engine_result": dict(Counter(map(engine_result, donner_only))),
            "donner_pass": len(donner_passes),
            "donner_pixel_pass": len(pixel_passes),
            "donner_render_pass": len(render_passes),
            "donner_strict_pass": len(strict_passes),
            "engine_pass": len(engine_passes),
            "gap": len(gap),
            "gap_by_state": dict(Counter(_gap_state(cases[path]) for path in gap)),
            "margin": len(donner_passes) - len(engine_passes),
            "total_cases": len(cases),
        },
        "render_passes": [
            {"path": path, "contract": RENDER_PASS_CONTRACTS[path]} for path in sorted(render_passes)
        ],
        "donner_only": [{"engine_result": engine_result(path), "path": path}
                        for path in donner_only],
        "gap": [
            {"path": path, "reason": cases[path]["reason"], "state": _gap_state(cases[path])}
            for path in gap
        ],
    }


def generate_head_to_head(repo_root: Path) -> dict[str, object]:
    results_path = repo_root / RESULTS_CSV
    try:
        text = results_path.read_text(encoding="utf-8-sig")
    except (OSError, UnicodeDecodeError) as error:
        raise ParityReportError(f"cannot read engine results {results_path}: {error}") from error
    return build_head_to_head(generate_report(repo_root), parse_engine_results(text))


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


RENDERER_TEST_ROOT = Path("donner/svg/renderer/tests")


def _check_reference_defaults(header: str, registry: str) -> None:
    thresholds = re.findall(r"kDefaultThreshold\s*=\s*([0-9.]+)f\s*;", header)
    budgets = re.findall(r"kDefaultMismatchedPixels\s*=\s*(\d+)\s*;", header)
    if (thresholds != [str(DEFAULT_THRESHOLD)] or budgets != [str(DEFAULT_MAX_PIXELS)]
            or not re.search(r"bool includeAntiAliasing\s*=\s*false;", header)):
        raise ParityReportError("C++ comparison defaults differ from parity report")
    if not re.search(r"\btest\.params\.setCanvasSize\(500,\s*500\);", registry):
        raise ParityReportError("C++ corpus default canvas differs from parity report")
    if not re.search(
        r"ImageComparisonParams ResvgV049Reference\(\)\s*\{\s*return "
        r"ImageComparisonParams\(\)\.setCanvasSize\(300,\s*300\);\s*\}",
        registry,
    ):
        raise ParityReportError("resvg v0.49 reference canvas differs from parity report")


_FEATURE_NAMES = {
    "Text": "text",
    "TextFull": "text-full",
    "FilterEffects": "filter-effects",
    "AsciiSnapshot": "ascii-snapshot",
}


def _backend_function(source: str, name: str) -> str:
    start = source.index("{", source.index(f"bool {name}("))
    return source[start:_find_matching(source, start, "{", "}")]


def _check_geode_capabilities(source: str) -> None:
    body = _backend_function(source, "GeodeSupportsFeature")
    features = re.findall(r"case RendererBackendFeature::(\w+):\s*return (true|false);", body)
    if {name for name, _ in features} != set(_FEATURE_NAMES) or len(features) != len(_FEATURE_NAMES):
        raise ParityReportError("Geode capability implementation differs from parity report")
    enabled = {_FEATURE_NAMES[name] for name, value in features if value == "true"}
    if enabled != CORPUS_LANES["geode"][1]:
        raise ParityReportError("Geode capability values differ from parity report")


def _check_tiny_capabilities(source: str) -> None:
    body = _backend_function(source, "TinySkiaSupportsFeature")
    leaves = set(re.findall(r"case RendererBackendFeature::(\w+):", body))
    if leaves != set(_FEATURE_NAMES) or not re.search(
        r"case RendererBackendFeature::AsciiSnapshot:\s*return true;", body
    ):
        raise ParityReportError("TinySkia capability implementation differs from parity report")
    macros = {"DONNER_TEXT_ENABLED": "Text", "DONNER_TEXT_FULL": "TextFull",
              "DONNER_FILTERS_ENABLED": "FilterEffects"}
    for macro, feature in macros.items():
        gate = (
            rf"#ifdef\s+{macro}\s+case RendererBackendFeature::{feature}:\s*return true;"
            rf"\s+#else\s+case RendererBackendFeature::{feature}:\s*return false;\s+#endif"
        )
        if not re.search(gate, body):
            raise ParityReportError(f"TinySkia capability {feature} differs from parity report")


def _check_corpus_wrappers(source: str) -> None:
    build = re.sub(r"(?m)^\s*#.*$", "", source)
    start = build.index("named_variants = [", build.index('name = "resvg_test_suite"'))
    start = build.index("[", start)
    variants = build[start:_find_matching(build, start, "[", "]")]
    wrapper_modes = {}
    for block in re.findall(r"\{([^}]+)\}", variants):
        config = dict(re.findall(r'"(\w+)"\s*:\s*"([^"]+)"', block))
        wrapper_modes[config["name"]] = (config["backend"], config["text"], config["text_full"])
    expected = {
        "default_text": ("tiny_skia", "true", "false"),
        "max": ("tiny_skia", "true", "true"),
        "geode": ("geode", "true", "false"),
    }
    if wrapper_modes != expected:
        raise ParityReportError("Bazel corpus wrappers differ from parity report")


def check_renderer_contract(repo_root: Path) -> None:
    """Fail CI if the normal wrapper matrix or C++ defaults drift from the static report."""
    def read(file: str) -> str:
        return (repo_root / RENDERER_TEST_ROOT / file).read_text(encoding="utf-8")

    _check_reference_defaults(_mask_comments(read("ImageComparisonTestFixture.h")),
                              _mask_comments(read("resvg_test_suite.cc")))
    _check_geode_capabilities(_mask_comments(read("RendererTestBackendGeode.cc")))
    _check_tiny_capabilities(_mask_comments(read("RendererTestBackendTinySkia.cc")))
    _check_corpus_wrappers(read("BUILD.bazel"))


FEATURE_GAP_DOC = Path("docs/design_docs/0021-resvg_feature_gaps.md")
INVENTORY_START = "<!-- BEGIN RESVG INVENTORY -->"
INVENTORY_END = "<!-- END RESVG INVENTORY -->"


def inventory_markdown(report: dict[str, object]) -> str:
    """Render the compact static inventory checked into the feature-gap document."""
    summary = report["summary"]
    comparison_exceptions = sum(
        case["primary_state"] == "compare" and bool(_scoped_exception_kinds(case))
        for case in report["cases"]
    )
    lines = [
        INVENTORY_START,
        "",
        "| Registered SVG fixtures | Count |",
        "| --- | ---: |",
        f"| Total | {summary['total_cases']:,} |",
        f"| Pixel comparisons | {summary['compare_cases']:,} |",
        f"| Comparisons with a numeric setting or custom golden | {comparison_exceptions:,} |",
        f"| Render-only, undefined-behavior registration | {summary['render_only_undefined_behavior_cases']:,} |",
        f"| Render-only, legacy feature | {summary['render_only_deprecated_in_svg2_cases']:,} |",
        f"| Explicit skips | {summary['skip_cases']:,} |",
        f"| Fixtures in disabled categories | {summary['disabled_cases']:,} |",
        "",
        "| Configured lane | Pixel comparisons | Render-only | Capability-gated | Explicit skips |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for lane in CORPUS_LANES:
        state_counts = {
            state: sum(counts.values()) for state, counts in report["lane_summary"][lane].items()
        }
        values = [state_counts.get(state, 0) for state in (
            "compare", "render-only", "capability-gated", "skip"
        )]
        lines.append(f"| `{lane}` | " + " | ".join(f"{n:,}" for n in values) + " |")
    return "\n".join([*lines, "", INVENTORY_END])


def check_inventory_doc(report: dict[str, object], text: str) -> None:
    if text.count(INVENTORY_START) != 1 or text.count(INVENTORY_END) != 1:
        raise ParityReportError("feature-gap document needs one generated inventory block")
    if inventory_markdown(report) not in text:
        raise ParityReportError("feature-gap document inventory is stale; regenerate its block")


REPORT_INPUT_PATHS = (
    REGISTRATION_SOURCE.as_posix(),
    RESULTS_CSV.as_posix(),
    EXCEPTION_POLICIES.as_posix(),
    FEATURE_GAP_DOC.as_posix(),
    UB_MARKER_REFERENCE.as_posix(),
    f":(glob){TESTS_ROOT.as_posix()}/**/*.svg",
    f":(glob){TESTS_ROOT.as_posix()}/**/*.png",
    f"{RENDERER_TEST_ROOT.as_posix()}/BUILD.bazel",
    f"{RENDERER_TEST_ROOT.as_posix()}/ImageComparisonTestFixture.h",
    f"{RENDERER_TEST_ROOT.as_posix()}/RendererTestBackendGeode.cc",
    f"{RENDERER_TEST_ROOT.as_posix()}/RendererTestBackendTinySkia.cc",
    "docs/design_docs/0009-resvg_test_suite_bugs.md",
    "third_party/resvg-test-suite/BUILD.bazel",
    "donner/svg/renderer/testdata/golden/README.md",
    ":(glob)donner/svg/renderer/testdata/golden/**/*.png",
    ":(glob)donner/svg/renderer/testdata/golden/**/*.svg",
    ":(glob)tools/resvg_parity/**/*.py",
)


def _checkout_revision(repo_root: Path) -> tuple[str | None, bool, bool]:
    if not (repo_root / ".git").exists():
        return None, False, False
    try:
        def git(*args: str) -> str:
            return subprocess.check_output(
                ["git", "-C", str(repo_root), *args], text=True, stderr=subprocess.DEVNULL,
            ).strip()

        head = git("rev-parse", "HEAD")
        tracked_dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
        # Do not apply the ignore rules here. The corpus scanner still sees ignored fixtures.
        untracked_inputs = bool(git("ls-files", "--others", "-z", "--", *REPORT_INPUT_PATHS))
        return head, tracked_dirty, untracked_inputs
    except (OSError, subprocess.CalledProcessError) as error:
        raise ParityReportError("cannot identify the report checkout revision") from error


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=_default_repo_root(),
        help="Donner repository root (default: inferred from this script)",
    )
    parser.add_argument("--output", type=Path, help="write JSON to this file instead of stdout")
    parser.add_argument(
        "--head-to-head",
        action="store_true",
        help=f"compare Donner's passes with {HEAD_TO_HEAD_ENGINE}'s in {RESULTS_CSV} instead of "
        "writing the per-case ledger",
    )
    parser.add_argument(
        "--revision", help="caller assertion for a source archive or dirty tree; clean checkouts use HEAD",
    )
    doc = parser.add_mutually_exclusive_group()
    doc.add_argument("--check-doc", action="store_true",
                     help="check that the feature-gap document inventory matches the source")
    doc.add_argument("--inventory-markdown", action="store_true",
                     help="print the generated feature-gap inventory block")
    parser.add_argument(
        "--execution", action="append", default=[], metavar="REVISION:LANE=PATH",
        help="import caller-attested GoogleTest statuses for one normal corpus lane",
    )
    return parser


def _validate_cli(args: argparse.Namespace) -> None:
    if args.revision is not None and not re.fullmatch(r"[0-9a-f]{40}", args.revision):
        raise ParityReportError("--revision requires a full lowercase Git commit SHA")
    if args.head_to_head and (args.revision or args.execution or args.check_doc or args.inventory_markdown):
        raise ParityReportError("inventory and execution options require the per-case report")
    if args.inventory_markdown and args.output:
        raise ParityReportError("--inventory-markdown writes to stdout")


def _import_execution_argument(report: dict[str, object], receipt: str) -> None:
    revision_lane, separator, path = receipt.partition("=")
    revision, colon, lane = revision_lane.partition(":")
    if not (separator and colon and path and lane and revision):
        raise ParityReportError("--execution expects REVISION:LANE=PATH")
    add_execution(report, lane, revision, Path(path))


def _report_from_cli(args: argparse.Namespace, repo_root: Path) -> dict[str, object]:
    if args.head_to_head:
        return generate_head_to_head(repo_root)
    report = generate_report(repo_root)
    checkout_revision, tracked_dirty, untracked_inputs = _checkout_revision(repo_root)
    if checkout_revision and args.revision and args.revision != checkout_revision:
        raise ParityReportError("--revision differs from the checkout HEAD")
    report["source"].update({
        "checkout_head": checkout_revision,
        "tracked_tree_dirty": tracked_dirty,
        "untracked_report_inputs": untracked_inputs,
        "revision": checkout_revision if not tracked_dirty and not untracked_inputs else None,
        "caller_revision": args.revision,
    })
    for receipt in args.execution:
        _import_execution_argument(report, receipt)
    return report


def _check_feature_gap_doc(repo_root: Path, report: dict[str, object]) -> None:
    try:
        text = (repo_root / FEATURE_GAP_DOC).read_text(encoding="utf-8")
    except OSError as error:
        raise ParityReportError(f"cannot read feature-gap document: {error}") from error
    check_inventory_doc(report, text)


def main(argv: Sequence[str] | None = None, stdout: TextIO = sys.stdout) -> int:
    args = _argument_parser().parse_args(argv)
    try:
        _validate_cli(args)
        repo_root = args.repo_root.resolve()
        report = _report_from_cli(args, repo_root)
        if args.check_doc:
            _check_feature_gap_doc(repo_root, report)
        if args.inventory_markdown:
            stdout.write(inventory_markdown(report) + "\n")
        elif not args.check_doc or args.output:
            _write_json(report, args.output, stdout)
    except ParityReportError as error:
        print(f"parity_report: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
