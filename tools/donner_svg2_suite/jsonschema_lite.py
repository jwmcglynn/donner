#!/usr/bin/env python3
"""A small, dependency-free JSON Schema validator.

The Donner SVG2 test suite ships canonical JSON Schema documents for its
corpus, profile, result, and requirement records (see ``schemas/``). Those
documents are the single source of truth for structural validity. Rather than
pull in a third-party validator, this module interprets the subset of JSON
Schema 2020-12 keywords the suite's own schemas use.

The subset is intentionally narrow. Adding a keyword to a schema without adding
support here is a bug, so :func:`validate` raises ``UnsupportedKeyword`` when it
encounters a keyword it does not understand instead of silently ignoring it.
That fail-closed behaviour keeps a schema from appearing to constrain data it
does not actually constrain.
"""

from __future__ import annotations

import re
from typing import Any


SUPPORTED_KEYWORDS = frozenset(
    {
        "$schema",
        "$id",
        "$ref",
        "$defs",
        "title",
        "description",
        "type",
        "const",
        "enum",
        "pattern",
        "minLength",
        "maxLength",
        "minimum",
        "exclusiveMinimum",
        "maximum",
        "required",
        "properties",
        "additionalProperties",
        "items",
        "minItems",
        "maxItems",
        "allOf",
        "anyOf",
        "oneOf",
        "if",
        "then",
        "else",
    }
)

# JSON Schema type name -> Python predicate. ``integer`` accepts ``bool`` in
# JSON's data model would be wrong, so booleans are excluded from numeric types.
_TYPE_PREDICATES = {
    "object": lambda value: isinstance(value, dict),
    "array": lambda value: isinstance(value, list),
    "string": lambda value: isinstance(value, str),
    "boolean": lambda value: isinstance(value, bool),
    "null": lambda value: value is None,
    "number": lambda value: isinstance(value, (int, float)) and not isinstance(value, bool),
    "integer": lambda value: isinstance(value, int) and not isinstance(value, bool),
}


class UnsupportedKeyword(Exception):
    """Raised when a schema uses a keyword this validator does not implement."""


class SchemaError(Exception):
    """Raised when a schema is itself malformed (for example a bad ``$ref``)."""


def _pointer(path: list[str]) -> str:
    return "/" + "/".join(path) if path else "<root>"


def _resolve_ref(ref: str, root: dict[str, Any]) -> dict[str, Any]:
    if not ref.startswith("#/"):
        raise SchemaError(f"only local '#/...' refs are supported, got {ref!r}")
    node: Any = root
    for token in ref[2:].split("/"):
        token = token.replace("~1", "/").replace("~0", "~")
        if not isinstance(node, dict) or token not in node:
            raise SchemaError(f"unresolvable ref {ref!r}")
        node = node[token]
    if not isinstance(node, dict):
        raise SchemaError(f"ref {ref!r} does not point at a schema object")
    return node


def _check_keywords(schema: dict[str, Any]) -> None:
    unknown = set(schema) - SUPPORTED_KEYWORDS
    if unknown:
        raise UnsupportedKeyword(
            "unsupported schema keyword(s): " + ", ".join(sorted(unknown))
        )


def _validate(
    value: Any,
    schema: dict[str, Any],
    root: dict[str, Any],
    path: list[str],
    errors: list[str],
) -> None:
    _check_keywords(schema)

    if "$ref" in schema:
        _validate(value, _resolve_ref(schema["$ref"], root), root, path, errors)
        # A ref-only subschema is common; other sibling keywords are still
        # applied below per the 2020-12 spec, so do not return early.

    if "type" in schema:
        expected = schema["type"]
        names = expected if isinstance(expected, list) else [expected]
        for name in names:
            if name not in _TYPE_PREDICATES:
                raise SchemaError(f"unknown type {name!r} at {_pointer(path)}")
        if not any(_TYPE_PREDICATES[name](value) for name in names):
            errors.append(f"{_pointer(path)}: expected type {expected}, got {_typename(value)}")
            return

    if "const" in schema and value != schema["const"]:
        errors.append(f"{_pointer(path)}: expected const {schema['const']!r}")

    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{_pointer(path)}: {value!r} is not one of {schema['enum']}")

    if isinstance(value, str):
        _validate_string(value, schema, path, errors)
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        _validate_number(value, schema, path, errors)
    if isinstance(value, list):
        _validate_array(value, schema, root, path, errors)
    if isinstance(value, dict):
        _validate_object(value, schema, root, path, errors)

    _validate_combinators(value, schema, root, path, errors)


def _validate_string(value: str, schema: dict[str, Any], path: list[str], errors: list[str]) -> None:
    if "minLength" in schema and len(value) < schema["minLength"]:
        errors.append(f"{_pointer(path)}: string shorter than minLength {schema['minLength']}")
    if "maxLength" in schema and len(value) > schema["maxLength"]:
        errors.append(f"{_pointer(path)}: string longer than maxLength {schema['maxLength']}")
    if "pattern" in schema and re.search(schema["pattern"], value) is None:
        errors.append(f"{_pointer(path)}: {value!r} does not match pattern {schema['pattern']!r}")


def _validate_number(value: Any, schema: dict[str, Any], path: list[str], errors: list[str]) -> None:
    if "minimum" in schema and value < schema["minimum"]:
        errors.append(f"{_pointer(path)}: {value} is below minimum {schema['minimum']}")
    if "exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"]:
        errors.append(f"{_pointer(path)}: {value} is not above exclusiveMinimum {schema['exclusiveMinimum']}")
    if "maximum" in schema and value > schema["maximum"]:
        errors.append(f"{_pointer(path)}: {value} is above maximum {schema['maximum']}")


def _validate_array(
    value: list[Any], schema: dict[str, Any], root: dict[str, Any], path: list[str], errors: list[str]
) -> None:
    if "minItems" in schema and len(value) < schema["minItems"]:
        errors.append(f"{_pointer(path)}: array has fewer than minItems {schema['minItems']}")
    if "maxItems" in schema and len(value) > schema["maxItems"]:
        errors.append(f"{_pointer(path)}: array has more than maxItems {schema['maxItems']}")
    if "items" in schema:
        for index, item in enumerate(value):
            _validate(item, schema["items"], root, path + [str(index)], errors)


def _validate_object(
    value: dict[str, Any], schema: dict[str, Any], root: dict[str, Any], path: list[str], errors: list[str]
) -> None:
    for name in schema.get("required", []):
        if name not in value:
            errors.append(f"{_pointer(path)}: missing required property {name!r}")

    properties = schema.get("properties", {})
    for key, item in value.items():
        if key in properties:
            _validate(item, properties[key], root, path + [key], errors)

    if "additionalProperties" in schema:
        additional = schema["additionalProperties"]
        extras = [key for key in value if key not in properties]
        if additional is False:
            for key in extras:
                errors.append(f"{_pointer(path)}: additional property {key!r} is not allowed")
        elif isinstance(additional, dict):
            for key in extras:
                _validate(value[key], additional, root, path + [key], errors)


def _validate_combinators(
    value: Any, schema: dict[str, Any], root: dict[str, Any], path: list[str], errors: list[str]
) -> None:
    for subschema in schema.get("allOf", []):
        _validate(value, subschema, root, path, errors)

    if "anyOf" in schema:
        if not any(_matches(value, sub, root) for sub in schema["anyOf"]):
            errors.append(f"{_pointer(path)}: does not match any of the anyOf subschemas")

    if "oneOf" in schema:
        matched = sum(1 for sub in schema["oneOf"] if _matches(value, sub, root))
        if matched != 1:
            errors.append(f"{_pointer(path)}: matched {matched} oneOf subschemas, expected exactly 1")

    if "if" in schema:
        branch = "then" if _matches(value, schema["if"], root) else "else"
        if branch in schema:
            _validate(value, schema[branch], root, path, errors)


def _matches(value: Any, schema: dict[str, Any], root: dict[str, Any]) -> bool:
    local: list[str] = []
    _validate(value, schema, root, [], local)
    return not local


def _typename(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, str):
        return "string"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    return type(value).__name__


def validate(value: Any, schema: dict[str, Any]) -> list[str]:
    """Validate ``value`` against ``schema`` and return a list of error strings.

    An empty list means the value is valid. ``UnsupportedKeyword`` and
    ``SchemaError`` propagate because they indicate a defect in the schema
    itself, not in the data under test.
    """

    errors: list[str] = []
    _validate(value, schema, schema, [], errors)
    return errors
