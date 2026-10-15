#!/usr/bin/env python3
"""
MCP Server for automated resvg test triage.

This server provides AI-assisted tools for analyzing and categorizing
resvg test suite failures, making test triage faster and more consistent.
"""

import base64
import json
import os
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from typing import Any

from mcp.server import Server
from mcp.types import Tool, TextContent, ImageContent
from triage_evidence import (
    bounded_text,
    comparator_count,
    failure_assessment,
    read_png,
    visual_evidence,
)

from codebase_helpers import (
    get_file_patterns_for_category,
    get_search_keywords_for_feature,
    rank_file_suggestions,
    get_implementation_hints,
)
from test_output_parser import (
    parse_test_output,
    parse_skip_file,
    group_by_category,
    identify_missing_features,
    get_next_priority_feature,
    extract_category_from_path,
)

server = Server("resvg-test-triage")
# Startup configuration is trusted; tool arguments cannot widen this boundary.
ARTIFACT_ROOT = os.environ.get("DONNER_TRIAGE_ARTIFACT_ROOT")


# ============================================================================
# Data Models
# ============================================================================

@dataclass
class SVGFeature:
    """Represents an SVG feature detected in a test."""
    name: str
    category: str  # "text", "styling", "positioning", "element"
    description: str


@dataclass
class TestFailure:
    """Represents a single test failure with analysis."""
    test_name: str
    pixel_diff: int
    features: list[SVGFeature]
    category: str  # "unresolved" until source/spec investigation establishes a cause
    suggested_skip: str
    related_tests: list[str]
    severity: str  # "unknown" until impact is established


# ============================================================================
# SVG Feature Detection
# ============================================================================

SVG_FEATURES = {
    # Text positioning
    "multiple_x_values": SVGFeature(
        "multiple_x_values",
        "positioning",
        "Multiple x values for per-glyph positioning"
    ),
    "multiple_y_values": SVGFeature(
        "multiple_y_values",
        "positioning",
        "Multiple y values for per-glyph positioning"
    ),
    "dx_attribute": SVGFeature(
        "dx_attribute",
        "positioning",
        "dx attribute for relative positioning"
    ),
    "dy_attribute": SVGFeature(
        "dy_attribute",
        "positioning",
        "dy attribute for relative positioning"
    ),
    "rotate_attribute": SVGFeature(
        "rotate_attribute",
        "positioning",
        "rotate attribute for glyph rotation"
    ),
    "textLength": SVGFeature(
        "textLength",
        "text",
        "textLength attribute for text stretching"
    ),

    # Text elements
    "tspan": SVGFeature(
        "tspan",
        "element",
        "<tspan> element for inline text spans"
    ),
    "textPath": SVGFeature(
        "textPath",
        "element",
        "<textPath> element for text on paths"
    ),

    # Text styling
    "text-anchor": SVGFeature(
        "text-anchor",
        "styling",
        "text-anchor attribute for alignment"
    ),
    "letter-spacing": SVGFeature(
        "letter-spacing",
        "styling",
        "letter-spacing attribute"
    ),
    "word-spacing": SVGFeature(
        "word-spacing",
        "styling",
        "word-spacing attribute"
    ),
    "text-decoration": SVGFeature(
        "text-decoration",
        "styling",
        "text-decoration attribute"
    ),
    "font-weight": SVGFeature(
        "font-weight",
        "styling",
        "font-weight attribute"
    ),
    "font-style": SVGFeature(
        "font-style",
        "styling",
        "font-style attribute"
    ),
    "font-variant": SVGFeature(
        "font-variant",
        "styling",
        "font-variant attribute"
    ),

    # Text layout
    "writing-mode": SVGFeature(
        "writing-mode",
        "text",
        "writing-mode attribute for vertical text"
    ),
    "baseline-shift": SVGFeature(
        "baseline-shift",
        "text",
        "baseline-shift attribute"
    ),
    "alignment-baseline": SVGFeature(
        "alignment-baseline",
        "text",
        "alignment-baseline attribute"
    ),
    "dominant-baseline": SVGFeature(
        "dominant-baseline",
        "text",
        "dominant-baseline attribute"
    ),
}


def _text_attribute_features(attributes: dict[str, str]) -> list[SVGFeature]:
    features = []
    for attribute in ("x", "y"):
        if " " in attributes.get(attribute, ""):
            features.append(SVG_FEATURES[f"multiple_{attribute}_values"])
    names = {"dx": "dx_attribute", "dy": "dy_attribute", "rotate": "rotate_attribute",
             "textLength": "textLength"}
    features.extend(SVG_FEATURES[name] for attribute, name in names.items() if attribute in attributes)
    if attributes.get("text-anchor") in ("end", "middle"):
        features.append(SVG_FEATURES["text-anchor"])
    return features


def _element_features(root: ET.Element) -> list[SVGFeature]:
    features = []
    for element in root.iter():
        tag = element.tag.split("}")[-1]
        if tag in ("tspan", "textPath"):
            features.append(SVG_FEATURES[tag])
        if tag in ("text", "tspan"):
            features.extend(_text_attribute_features(element.attrib))
    return features


def _style_features(svg_content: str) -> list[SVGFeature]:
    source = svg_content.lower()
    properties = ("letter-spacing", "word-spacing", "text-decoration", "font-weight", "font-style",
                  "font-variant", "writing-mode", "baseline-shift", "alignment-baseline",
                  "dominant-baseline")
    features = [SVG_FEATURES[name] for name in properties
                if ("font-style:" if name == "font-style" else name) in source]
    if "noto color emoji" in source or "noto-color-emoji" in source:
        features.append(SVGFeature("color_emoji", "text", "Color emoji font (Noto Color Emoji)"))
    return features


def detect_svg_features(svg_content: str) -> list[SVGFeature]:
    """Return observed SVG features without inferring implementation support."""
    bounded_text(svg_content)
    if "<!DOCTYPE" in svg_content.upper() or "<!ENTITY" in svg_content.upper():
        raise ValueError("DTD and entity declarations are not accepted for feature inspection")
    try:
        root = ET.fromstring(svg_content)
    except ET.ParseError as error:
        raise ValueError("Malformed SVG; feature detection is incomplete") from error
    features = _element_features(root) + _style_features(svg_content)
    return list({feature.name: feature for feature in features}.values())


# ============================================================================
# Image Handling
# ============================================================================

def read_image_as_base64(image_path: str) -> str:
    """Read a root-confined bounded PNG; invalid or unauthorized paths fail closed."""
    return base64.b64encode(read_png(image_path, ARTIFACT_ROOT).data).decode("ascii")


def categorize_failure(pixel_diff: int | None, features: list[SVGFeature]) -> tuple[str, str]:
    """Retain compatibility while refusing to infer causes from feature presence/counts."""
    del features
    return failure_assessment(pixel_diff)


def suggest_skip_comment(test_name: str, features: list[SVGFeature], category: str) -> str:
    """Prepare an explicitly unverified proposal; this tool never applies skips."""
    del features
    reason = f"Proposed ({category}): requires causal evidence and human review"
    return f'{{{json.dumps(test_name)}, Params::Skip({json.dumps(reason)})}},'


def find_related_tests(features: list[SVGFeature], all_tests: list[str]) -> list[str]:
    """
    Find tests that likely exercise the same feature.

    In the new directory-based layout there's no useful filename-prefix
    heuristic — a test like ``text/textPath/simple-case.svg`` and
    ``text/textPath/nested.svg`` are related because they share a
    category directory, not because they share a numeric prefix.

    This helper is kept for callers that have a pre-scoped list of
    category peers (e.g., from ``getTestsInCategory("text/textPath")``)
    and just want to sample the first few.

    Args:
        features: Features detected in current test (unused; retained
            for API compatibility).
        all_tests: List of test names within the same category.

    Returns:
        First N peers from ``all_tests``.
    """
    del features  # retained for compatibility, not used here
    return sorted(all_tests)[:5]


# ============================================================================
# MCP Server Tools
# ============================================================================

@server.list_tools()
async def list_tools() -> list[Tool]:
    """List available tools."""
    return [
        Tool(
            name="analyze_test_failure",
            description=(
                "Analyze a single resvg test failure by detecting SVG features, "
                "retaining unresolved causes and runner-supplied comparison metrics. "
                "Can optionally include actual, expected, and diff images for vision model analysis."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "test_name": {
                        "type": "string",
                        "description": "Bare SVG filename (e.g., 'simple-case.svg')"
                    },
                    "svg_content": {
                        "type": "string",
                        "description": "Complete SVG source code"
                    },
                    "pixel_diff": {
                        "type": ["integer", "null"],
                        "minimum": 0,
                        "description": (
                            "Mismatch count from the comparator, never display-diff RGB; "
                            "omit or use null when comparison did not produce a count"
                        )
                    },
                    "actual_image_path": {
                        "type": "string",
                        "description": "Path to actual rendering PNG (Donner's output)"
                    },
                    "expected_image_path": {
                        "type": "string",
                        "description": (
                            "Path to expected rendering PNG (golden reference). "
                            "In the new layout this sits next to the .svg in "
                            "resvg-test-suite/tests/<category>/."
                        )
                    },
                    "diff_image_path": {
                        "type": "string",
                        "description": "Path to diff image PNG (visual comparison)"
                    }
                },
                "required": ["test_name", "svg_content"]
            }
        ),
        Tool(
            name="batch_triage_tests",
            description=(
                "Summarize failures observed in caller-supplied log text, "
                "retaining quiet failures without authenticating metrics or runner provenance."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "test_output": {
                        "type": "string",
                        "description": "Caller-supplied test output; log syntax does not authenticate its origin"
                    },
                    "expected_iterations": {
                        "type": "integer", "minimum": 1, "maximum": 1000000,
                        "description": "Optional caller-known finite GoogleTest repetition count"
                    }
                },
                "required": ["test_output"]
            }
        ),
        Tool(
            name="suggest_skip_comment",
            description=(
                "Draft an unverified skip proposal requiring manual review; never apply it. "
                "following the established conventions in resvg_test_suite.cc."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "test_name": {
                        "type": "string",
                        "description": "Test file name"
                    },
                    "features": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "List of detected feature names"
                    },
                    "category": {
                        "type": "string",
                        "enum": ["not_implemented", "threshold_needed", "font_difference", "bug"],
                        "description": "Failure category"
                    }
                },
                "required": ["test_name", "category"]
            }
        ),
        Tool(
            name="detect_svg_features",
            description=(
                "Parse SVG content and detect which advanced features are being tested. "
                "Useful for understanding what a test validates."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "svg_content": {
                        "type": "string",
                        "description": "SVG source code to analyze"
                    }
                },
                "required": ["svg_content"]
            }
        ),
        Tool(
            name="suggest_implementation_approach",
            description=(
                "Given a failing test, suggest which files to modify and provide "
                "implementation guidance based on similar existing features."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "test_name": {
                        "type": "string",
                        "description": "Bare SVG filename (e.g., 'letter-spacing-with-arabic.svg')"
                    },
                    "features": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "List of detected feature names"
                    },
                    "category": {
                        "type": "string",
                        "description": "Feature category (e.g., 'text_styling', 'text_positioning')"
                    },
                    "codebase_files": {
                        "type": "array",
                        "items": {"type": "string"},
                        "description": "Optional: List of relevant files found in codebase"
                    }
                },
                "required": ["test_name", "features", "category"]
            }
        ),
        Tool(
            name="find_related_tests",
            description=(
                "Find all tests failing for the same feature or reason, "
                "grouped by category for easier batch implementation."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "feature": {
                        "type": "string",
                        "description": "Feature name to find related tests for"
                    },
                    "skip_file_content": {
                        "type": "string",
                        "description": "Content of resvg_test_suite.cc with skip entries"
                    }
                },
                "required": ["feature", "skip_file_content"]
            }
        ),
        Tool(
            name="generate_feature_report",
            description=(
                "Report unverified pass/fail/skip observations from supplied logs and "
                "declared skip reasons; this does not certify progress or provenance."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "category": {
                        "type": "string",
                        "description": (
                            "Category directory relative to resvg-test-suite/tests/ "
                            "(e.g. 'text/textPath', 'painting/fill', "
                            "'filters/feGaussianBlur')."
                        )
                    },
                    "test_output": {
                        "type": "string",
                        "description": "Caller-supplied test output; log syntax does not authenticate its origin"
                    },
                    "expected_iterations": {
                        "type": "integer", "minimum": 1, "maximum": 1000000,
                        "description": "Optional caller-known finite GoogleTest repetition count"
                    },
                    "skip_file_content": {
                        "type": "string",
                        "description": "Optional: Content of resvg_test_suite.cc for missing feature analysis"
                    }
                },
                "required": ["category", "test_output"]
            }
        ),
        Tool(
            name="analyze_visual_diff",
            description=(
                "Inspect bounded PNG metadata and optional comparator-supplied mismatch count. "
                "Root cause stays unresolved; display-diff colors are not a mismatch mask."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "pixel_diff": {
                        "type": "integer",
                        "minimum": 0,
                        "description": "Optional authoritative comparator mismatch count"
                    },
                    "diff_image_path": {
                        "type": "string",
                        "description": "Path to diff image PNG"
                    },
                    "actual_image_path": {
                        "type": "string",
                        "description": "Path to actual rendering PNG"
                    },
                    "expected_image_path": {
                        "type": "string",
                        "description": "Path to expected rendering PNG"
                    }
                },
                "required": ["diff_image_path", "actual_image_path", "expected_image_path"]
            }
        )
    ]


def _json_response(result: dict) -> list[TextContent]:
    return [TextContent(type="text", text=json.dumps(result, indent=2))]


def _raw_log_response(result: dict) -> list[TextContent]:
    """Protocol-shaped text does not identify who produced a diagnostic or metric."""
    return _json_response({**result, "evidence_source": "unverified_log_text",
                           "provenance_verified": False})


def _handle_detect_svg_features(arguments: dict[str, Any]):
    svg_content = arguments["svg_content"]
    features = detect_svg_features(svg_content)

    result = {
        "features": [
            {
                "name": f.name,
                "category": f.category,
                "description": f.description
            }
            for f in features
        ]
    }

    return [TextContent(
        type="text",
        text=json.dumps(result, indent=2)
    )]


def _handle_analyze_test_failure(arguments: dict[str, Any]):
    test_name = arguments["test_name"]
    svg_content = arguments["svg_content"]
    pixel_diff = comparator_count(arguments.get("pixel_diff"))

    # Optional image paths
    actual_path = arguments.get("actual_image_path")
    expected_path = arguments.get("expected_image_path")
    diff_path = arguments.get("diff_image_path")

    # Detect features
    features = detect_svg_features(svg_content)

    # Categorize failure
    category, severity = categorize_failure(pixel_diff, features)

    # Generate skip comment
    skip_comment = None

    result = {
        "test_name": test_name,
        "pixel_diff": pixel_diff,
        "metric_source": "caller_supplied_comparator" if pixel_diff is not None else None,
        "hypotheses": [],
        "features": [
            {
                "name": f.name,
                "category": f.category,
                "description": f.description
            }
            for f in features
        ],
        "category": category,
        "severity": severity,
        "suggested_skip": skip_comment,
        "analysis": {
            "feature_count": len(features),
            "primary_feature": features[0].description if features else "None detected",
            "recommendation": "Reproduce; inspect SVG, specification, renderer source and images."
        }
    }

    # Build response with optional images
    response = [TextContent(
        type="text",
        text=json.dumps(result, indent=2)
    )]

    # Add images if paths provided
    if actual_path:
        actual_data = read_image_as_base64(actual_path)
        if actual_data:
            response.append(ImageContent(
                type="image",
                data=actual_data,
                mimeType="image/png"
            ))

    if expected_path:
        expected_data = read_image_as_base64(expected_path)
        if expected_data:
            response.append(ImageContent(
                type="image",
                data=expected_data,
                mimeType="image/png"
            ))

    if diff_path:
        diff_data = read_image_as_base64(diff_path)
        if diff_data:
            response.append(ImageContent(
                type="image",
                data=diff_data,
                mimeType="image/png"
            ))

    return response


def _handle_batch_triage_tests(arguments: dict[str, Any]):
    results, summary = parse_test_output(
        arguments["test_output"], expected_iterations=arguments.get("expected_iterations"))
    failures = [
        {
            "test": item.test_name,
            "test_id": item.test_id,
            "iteration": item.iteration,
            "status": item.status,
            "path": item.svg_path,
            "category_dir": item.category,
            "pixel_diff": item.pixel_diff,
            "metric_source": "unverified_log_text" if item.pixel_diff is not None else None,
            "provenance_verified": False,
            "features": [],
            "category": "unresolved",
            "severity": "unknown",
            "suggested_skip": None,
            "raw_output": item.raw_output,
            "hypotheses": [],
        }
        for item in results if item.status in ("FAILED", "INCOMPLETE")
    ]
    result = {
        "total_failures": len(failures),
        "incomplete_tests": summary.incomplete,
        "run_complete": summary.run_complete,
        "completed_iterations": summary.completed_iterations,
        "expected_iterations": summary.expected_iterations,
        "failures": failures,
        "grouped_by_feature": {},
        "summary": {"unresolved": len(failures)},
    }
    return _raw_log_response(result)


def _handle_suggest_skip_comment(arguments: dict[str, Any]):
    test_name = arguments["test_name"]
    category = arguments["category"]
    feature_names = arguments.get("features", [])

    # Convert feature names to SVGFeature objects
    features = []
    for fname in feature_names:
        if fname in SVG_FEATURES:
            features.append(SVG_FEATURES[fname])

    skip_comment = suggest_skip_comment(test_name, features, category)

    result = {
        "skip_comment": skip_comment,
        "proposal_only": True,
        "requires_human_review": True,
        "applied": False,
        "test_name": test_name,
        "category": category
    }

    return [TextContent(
        type="text",
        text=json.dumps(result, indent=2)
    )]


def _handle_suggest_implementation_approach(arguments: dict[str, Any]):
    test_name = arguments["test_name"]
    feature_names = arguments["features"]
    category = arguments["category"]
    codebase_files = arguments.get("codebase_files", [])

    # Get file patterns for this category
    patterns = get_file_patterns_for_category(category)

    # Get search keywords for the primary feature
    primary_feature = feature_names[0] if feature_names else ""
    keywords = get_search_keywords_for_feature(primary_feature)

    # Rank provided files or suggest patterns
    if codebase_files:
        ranked_files = rank_file_suggestions(codebase_files, primary_feature)
        likely_files = [f[0] for f in ranked_files[:5]]  # Top 5
    else:
        likely_files = []

    # Get implementation hints
    hints = get_implementation_hints(primary_feature, category)

    # Find similar features (simplified - just return examples)
    similar_features = []
    if category == "text_styling":
        similar_features.append({
            "name": "font-size",
            "files": ["donner/svg/components/text/ComputedTextStyleComponent.h"]
        })
    elif category == "text_positioning":
        similar_features.append({
            "name": "x_attribute",
            "files": ["donner/svg/SVGTextElement.h"]
        })

    result = {
        "test_name": test_name,
        "category": category,
        "primary_feature": primary_feature,
        "likely_files": likely_files,
        "file_patterns": patterns,
        "search_keywords": keywords,
        "similar_features": similar_features,
        "implementation_hints": hints,
        "evidence_status": "unverified_search_leads",
        "hypotheses": hints
    }

    return [TextContent(
        type="text",
        text=json.dumps(result, indent=2)
    )]


def _impact_priority(count: int) -> str:
    if count == 0:
        return "none"
    if count == 1:
        return "low"
    if count <= 5:
        return "medium"
    return "high"


def _handle_find_related_tests(arguments: dict[str, Any]):
    feature = arguments["feature"]
    skip_map = parse_skip_file(arguments["skip_file_content"])
    related = [name for name, reason in skip_map.items() if feature.lower() in reason.lower()]
    feature_map = identify_missing_features(skip_map)
    count = len(related)
    return _json_response({
        "feature": feature,
        "related_tests": related,
        "impact": f"{count} test affected" if count == 1 else f"{count} tests affected",
        "priority": _impact_priority(count),
        "grouped_by_category": {"unknown": related} if related else {},
        "all_missing_features": {name: len(tests) for name, tests in feature_map.items()},
        "evidence_status": "declared_skip_reasons_only",
    })


def _declared_feature_priority(skip_file_content: str, category: str) -> tuple[list[str], str]:
    if not skip_file_content:
        return [], "Unknown - no skip file provided"
    feature_map = identify_missing_features(parse_skip_file(skip_file_content), category)
    priority = get_next_priority_feature(feature_map)
    if priority:
        feature, count = priority
        return list(feature_map), f"{feature} (affects {count} tests)"
    return list(feature_map), "Unknown - no missing features declared in supplied skip text"


def _handle_generate_feature_report(arguments: dict[str, Any]):
    category = arguments["category"]
    results, summary = parse_test_output(
        arguments["test_output"], expected_iterations=arguments.get("expected_iterations"))
    reports = group_by_category(results)
    if category not in reports:
        return _raw_log_response({
            "category": category,
            "error": f"No tests found for category '{category}'",
            "available_categories": list(reports),
        })
    report = reports[category]
    missing, priority = _declared_feature_priority(arguments.get("skip_file_content", ""), category)
    return _raw_log_response({
        "category": category,
        "total_tests": report.total_tests,
        "passing": report.passing,
        "failing": report.failing,
        "skipped": report.skipped,
        "evidence_status": "run_counts_and_declared_skip_reasons_only",
        "completion_rate": f"{int(report.completion_rate * 100)}%",
        "run_complete": summary.run_complete,
        "completed_iterations": summary.completed_iterations,
        "expected_iterations": summary.expected_iterations,
        "incomplete_tests": report.incomplete,
        "implemented_features": [],
        "missing_features": missing[:10],
        "next_priority": priority,
        "test_details": [
            {"name": item.test_name, "status": item.status, "pixel_diff": item.pixel_diff,
             "iteration": item.iteration,
             "metric_source": "unverified_log_text" if item.pixel_diff is not None else None,
             "provenance_verified": False}
            for item in report.test_results[:20]
        ],
    })


def _handle_analyze_visual_diff(arguments: dict[str, Any]):
    diff_image_path = arguments["diff_image_path"]
    actual_image_path = arguments["actual_image_path"]
    expected_image_path = arguments["expected_image_path"]

    images = {
        "diff": read_png(diff_image_path, ARTIFACT_ROOT),
        "actual": read_png(actual_image_path, ARTIFACT_ROOT),
        "expected": read_png(expected_image_path, ARTIFACT_ROOT),
    }
    result = visual_evidence(images, arguments.get("pixel_diff"))

    return [TextContent(
        type="text",
        text=json.dumps(result, indent=2)
    )]



_TOOL_HANDLERS = {
    "detect_svg_features": _handle_detect_svg_features,
    "analyze_test_failure": _handle_analyze_test_failure,
    "batch_triage_tests": _handle_batch_triage_tests,
    "suggest_skip_comment": _handle_suggest_skip_comment,
    "suggest_implementation_approach": _handle_suggest_implementation_approach,
    "find_related_tests": _handle_find_related_tests,
    "generate_feature_report": _handle_generate_feature_report,
    "analyze_visual_diff": _handle_analyze_visual_diff,
}


@server.call_tool()
async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent | ImageContent]:
    """Dispatch read-only handlers after bounding the complete request."""
    bounded_text(json.dumps(arguments))
    handler = _TOOL_HANDLERS.get(name)
    if handler is None:
        raise ValueError(f"Unknown tool: {name}")
    return handler(arguments)


# ============================================================================
# Main
# ============================================================================

async def main():
    """Run the MCP server."""
    from mcp.server.stdio import stdio_server

    async with stdio_server() as (read_stream, write_stream):
        await server.run(
            read_stream,
            write_stream,
            server.create_initialization_options()
        )


if __name__ == "__main__":
    import asyncio
    asyncio.run(main())
