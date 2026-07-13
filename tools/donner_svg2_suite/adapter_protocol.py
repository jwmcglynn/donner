#!/usr/bin/env python3
"""The SVG2 suite render-adapter request/response contract.

The reference runner invokes an adapter as an executable plus an argument array
(never a shell string) and exchanges typed JSON documents:

    svg-render-adapter render --request request.json --response response.json

This module defines the request the runner writes, the response an adapter must
return, and strict parsing that treats a missing, malformed, or out-of-contract
response as an error rather than a silent pass. Runner-level statuses are held
separately in :mod:`runner`; the adapter reports only its own outcome
(``ok`` / ``unsupported`` / ``error``).
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field


# Statuses an adapter may report for its own work. The runner maps these to the
# richer per-test result statuses.
ADAPTER_STATUSES = frozenset({"ok", "unsupported", "error"})

RENDER_FORMAT = "rgba8"


class AdapterProtocolError(Exception):
    """Raised when an adapter response violates the contract."""


@dataclass(frozen=True)
class RenderRequest:
    """Everything an adapter needs to render one case inside its sandbox."""

    operation: str
    test_id: str
    input: str
    output: str
    resource_root: str
    font_root: str
    processing_mode: str
    width: int
    height: int
    capabilities: list[str] = field(default_factory=list)

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2, sort_keys=True)


def render_request(
    *,
    test_id: str,
    input_path: str,
    output_path: str,
    resource_root: str,
    font_root: str,
    processing_mode: str,
    width: int,
    height: int,
    capabilities: list[str],
) -> RenderRequest:
    return RenderRequest(
        operation="render",
        test_id=test_id,
        input=input_path,
        output=output_path,
        resource_root=resource_root,
        font_root=font_root,
        processing_mode=processing_mode,
        width=width,
        height=height,
        capabilities=list(capabilities),
    )


@dataclass(frozen=True)
class AdapterResponse:
    status: str
    width: int | None = None
    height: int | None = None
    format: str | None = None
    diagnostics: str = ""
    duration_ms: float | None = None


def parse_response(text: str) -> AdapterResponse:
    """Parse and validate an adapter response document.

    Raises :class:`AdapterProtocolError` on malformed JSON, a missing or unknown
    status, or a render success that omits its declared dimensions/format.
    """

    try:
        payload = json.loads(text)
    except json.JSONDecodeError as error:
        raise AdapterProtocolError(f"response is not valid JSON: {error}") from error
    if not isinstance(payload, dict):
        raise AdapterProtocolError("response must be a JSON object")

    status = payload.get("status")
    if status not in ADAPTER_STATUSES:
        raise AdapterProtocolError(f"invalid or missing adapter status: {status!r}")

    response = AdapterResponse(
        status=status,
        width=payload.get("width"),
        height=payload.get("height"),
        format=payload.get("format"),
        diagnostics=payload.get("diagnostics", ""),
        duration_ms=payload.get("duration_ms"),
    )

    if status == "ok":
        if not isinstance(response.width, int) or not isinstance(response.height, int):
            raise AdapterProtocolError("render success must report integer width and height")
        if response.format != RENDER_FORMAT:
            raise AdapterProtocolError(f"render success must report format {RENDER_FORMAT!r}")

    return response


def write_response(path: str, status: str, **fields: object) -> None:
    """Helper for adapter implementations to emit a response document."""

    document: dict[str, object] = {"status": status}
    document.update(fields)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2, sort_keys=True)
