#!/usr/bin/env python3
"""
MCP Server for automated resvg test triage.

This server provides AI-assisted tools for analyzing and categorizing
resvg test suite failures, making test triage faster and more consistent.
"""

import base64
import json
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from mcp.server import Server
from mcp.types import Tool, TextContent, ImageContent
from PIL import Image
import numpy as np

from codebase_helpers import (
    get_file_patterns_for_category,
    get_search_keywords_for_feature,
    rank_file_suggestions,
    get_implementation_hints,
    SimilarFeature,
)
from test_output_parser import (
    parse_test_output,
    parse_skip_file,
    group_by_category,
    identify_missing_features,
    get_next_priority_feature,
)

server = Server("resvg-test-triage")


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
    category: str  # "not_implemented", "threshold_needed", "font_difference", "bug"
    suggested_skip: str
    related_tests: list[str]
    severity: str  # "minor", "moderate", "major"


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


def detect_svg_features(svg_content: str) -> list[SVGFeature]:
    """
    Parse SVG content and detect advanced features being tested.

    Args:
        svg_content: The SVG source code

    Returns:
        List of detected SVG features
    """
    features = []

    try:
        # Parse SVG
        root = ET.fromstring(svg_content)

        # Scan all text elements
        for elem in root.iter():
            tag = elem.tag.split('}')[-1]  # Remove namespace

            # Check for tspan/textPath elements
            if tag == "tspan":
                features.append(SVG_FEATURES["tspan"])
            elif tag == "textPath":
                features.append(SVG_FEATURES["textPath"])

            # Check text element attributes
            if tag == "text" or tag == "tspan":
                # Check for multiple x/y values
                if "x" in elem.attrib and " " in elem.attrib["x"]:
                    features.append(SVG_FEATURES["multiple_x_values"])
                if "y" in elem.attrib and " " in elem.attrib["y"]:
                    features.append(SVG_FEATURES["multiple_y_values"])

                # Check for dx/dy
                if "dx" in elem.attrib:
                    features.append(SVG_FEATURES["dx_attribute"])
                if "dy" in elem.attrib:
                    features.append(SVG_FEATURES["dy_attribute"])

                # Check for rotate
                if "rotate" in elem.attrib:
                    features.append(SVG_FEATURES["rotate_attribute"])

                # Check for textLength
                if "textLength" in elem.attrib:
                    features.append(SVG_FEATURES["textLength"])

                # Check for text-anchor
                if "text-anchor" in elem.attrib:
                    value = elem.attrib["text-anchor"]
                    if value in ["end", "middle"]:  # start is default
                        features.append(SVG_FEATURES["text-anchor"])

        # Check CSS properties in style attribute or <style> elements
        svg_str = svg_content.lower()
        if "letter-spacing" in svg_str:
            features.append(SVG_FEATURES["letter-spacing"])
        if "word-spacing" in svg_str:
            features.append(SVG_FEATURES["word-spacing"])
        if "text-decoration" in svg_str:
            features.append(SVG_FEATURES["text-decoration"])
        if "font-weight" in svg_str:
            features.append(SVG_FEATURES["font-weight"])
        if "font-style" in svg_str and "font-style:" in svg_str:  # Avoid false positives
            features.append(SVG_FEATURES["font-style"])
        if "font-variant" in svg_str:
            features.append(SVG_FEATURES["font-variant"])
        if "writing-mode" in svg_str:
            features.append(SVG_FEATURES["writing-mode"])
        if "baseline-shift" in svg_str:
            features.append(SVG_FEATURES["baseline-shift"])
        if "alignment-baseline" in svg_str:
            features.append(SVG_FEATURES["alignment-baseline"])
        if "dominant-baseline" in svg_str:
            features.append(SVG_FEATURES["dominant-baseline"])

        # Check for color emoji fonts (special case)
        if "noto color emoji" in svg_str or "noto-color-emoji" in svg_str:
            features.append(SVGFeature(
                "color_emoji",
                "text",
                "Color emoji font (Noto Color Emoji)"
            ))

    except ET.ParseError as e:
        # If parsing fails, try simple text search
        pass

    # Remove duplicates
    seen = set()
    unique_features = []
    for feature in features:
        if feature.name not in seen:
            seen.add(feature.name)
            unique_features.append(feature)

    return unique_features


# ============================================================================
# Image Handling
# ============================================================================

def read_image_as_base64(image_path: str) -> str | None:
    """
    Read an image file and encode it as base64.

    Args:
        image_path: Path to the image file

    Returns:
        Base64-encoded image data, or None if file doesn't exist
    """
    try:
        path = Path(image_path)
        if not path.exists():
            return None

        with open(path, 'rb') as f:
            image_data = f.read()
            return base64.b64encode(image_data).decode('utf-8')
    except Exception:
        return None


# ============================================================================
# Failure Analysis
# ============================================================================

def categorize_failure(pixel_diff: int, features: list[SVGFeature]) -> tuple[str, str]:
    """
    Categorize a test failure based on pixel diff and features.

    Returns:
        (category, severity) tuple
    """
    # Determine severity
    if pixel_diff < 500:
        severity = "minor"
    elif pixel_diff < 5000:
        severity = "moderate"
    else:
        severity = "major"

    # Determine category
    if pixel_diff <= 100:
        category = "threshold_needed"
    elif features:
        # Has advanced features that aren't implemented
        category = "not_implemented"
    elif pixel_diff < 1000:
        # Small diff, no special features - likely font rendering difference
        category = "font_difference"
    else:
        # Large diff, no obvious missing features - likely a bug
        category = "bug"

    return category, severity


def suggest_skip_comment(test_name: str, features: list[SVGFeature], category: str) -> str:
    """
    Generate a skip comment for resvg_test_suite.cc.

    Args:
        test_name: Test file name (e.g., "e-text-002.svg")
        features: Detected SVG features
        category: Failure category

    Returns:
        Formatted skip entry
    """
    if category == "not_implemented" and features:
        # Use the most specific feature
        feature = features[0]

        # Generate appropriate comment based on feature
        if feature.category == "element":
            comment = f"Not impl: <{feature.name}>"
        elif feature.category == "positioning":
            # Format feature name nicely
            feature_name = feature.name.replace("_", " ").replace(" attribute", "")
            if "multiple" in feature_name:
                comment = f"Not impl: {feature_name.capitalize()}"
            else:
                comment = f"Not impl: `{feature_name}` attribute"
        elif feature.category == "styling":
            attr_name = feature.name.replace("_", "-")
            comment = f"Not impl: `{attr_name}`"
        elif feature.category == "text":
            if "emoji" in feature.name:
                comment = "Not impl: Color emoji font (Noto Color Emoji)"
            elif "mode" in feature.name:
                comment = f"Not impl: `{feature.name.replace('_', '-')}`"
            else:
                comment = f"Not impl: `{feature.name.replace('_', '-')}`"
        else:
            comment = f"Not impl: {feature.description}"
    elif category == "threshold_needed":
        comment = "Larger threshold due to anti-aliasing artifacts"
    elif category == "font_difference":
        comment = "Expected font rendering difference (CoreText vs FreeType)"
    else:
        comment = "Bug: Rendering issue"

    return f'{{\"{test_name}\", Params::Skip()}},  // {comment}'


def find_related_tests(features: list[SVGFeature], all_tests: list[str]) -> list[str]:
    """
    Find tests that likely use similar features.

    Args:
        features: Features detected in current test
        all_tests: List of all test names

    Returns:
        List of related test names
    """
    if not features:
        return []

    # Simple heuristic: tests with same numeric prefix often test related features
    # e.g., e-text-002, e-text-003, e-text-004 all test multiple x/y values
    related = []

    # Extract base pattern (e.g., "e-text-00" from "e-text-002.svg")
    for test in all_tests:
        # Match pattern like e-text-NNN.svg
        match = re.match(r'(e-\w+-)(\d{3})', test)
        if match:
            prefix = match.group(1)
            num = int(match.group(2))
            # Tests within +/-5 often related
            related.append(f"{prefix}{num:03d}.svg")

    return related[:5]  # Limit to 5 related tests


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
                "categorizing the failure type, and suggesting appropriate skip comments. "
                "Can optionally include actual, expected, and diff images for vision model analysis."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "test_name": {
                        "type": "string",
                        "description": "Test file name (e.g., 'e-text-002.svg')"
                    },
                    "svg_content": {
                        "type": "string",
                        "description": "Complete SVG source code"
                    },
                    "pixel_diff": {
                        "type": "number",
                        "description": "Number of pixels that differ from expected"
                    },
                    "actual_image_path": {
                        "type": "string",
                        "description": "Path to actual rendering PNG (Donner's output)"
                    },
                    "expected_image_path": {
                        "type": "string",
                        "description": "Path to expected rendering PNG (golden reference)"
                    },
                    "diff_image_path": {
                        "type": "string",
                        "description": "Path to diff image PNG (visual comparison)"
                    }
                },
                "required": ["test_name", "svg_content", "pixel_diff"]
            }
        ),
        Tool(
            name="batch_triage_tests",
            description=(
                "Batch analyze multiple test failures from test output, "
                "grouping them by feature category and generating skip entries."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "test_output": {
                        "type": "string",
                        "description": "Complete test output from bazel run"
                    }
                },
                "required": ["test_output"]
            }
        ),
        Tool(
            name="suggest_skip_comment",
            description=(
                "Generate a properly formatted skip comment for a test, "
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
                        "description": "Test file name (e.g., 'e-text-023.svg')"
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
                "Generate comprehensive progress report for a feature category, "
                "showing pass/fail/skip counts and next priority features."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "category": {
                        "type": "string",
                        "description": "Category prefix (e.g., 'e-text', 'a-transform')"
                    },
                    "test_output": {
                        "type": "string",
                        "description": "Recent test run output from bazel test"
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
                "Programmatically analyze diff images to categorize failure types "
                "(positioning errors, missing elements, color differences, anti-aliasing)."
            ),
            inputSchema={
                "type": "object",
                "properties": {
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


@server.call_tool()
async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
    """Handle tool calls."""

    if name == "detect_svg_features":
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

    elif name == "analyze_test_failure":
        test_name = arguments["test_name"]
        svg_content = arguments["svg_content"]
        pixel_diff = int(arguments["pixel_diff"])

        # Optional image paths
        actual_path = arguments.get("actual_image_path")
        expected_path = arguments.get("expected_image_path")
        diff_path = arguments.get("diff_image_path")

        # Detect features
        features = detect_svg_features(svg_content)

        # Categorize failure
        category, severity = categorize_failure(pixel_diff, features)

        # Generate skip comment
        skip_comment = suggest_skip_comment(test_name, features, category)

        result = {
            "test_name": test_name,
            "pixel_diff": pixel_diff,
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
                "recommendation": (
                    "Skip - feature not implemented" if category == "not_implemented"
                    else "Adjust threshold" if category == "threshold_needed"
                    else "Skip - expected font difference" if category == "font_difference"
                    else "Investigate - possible bug"
                )
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

    elif name == "batch_triage_tests":
        test_output = arguments["test_output"]

        # Parse test output to extract failures
        failures = []
        current_test = None
        current_svg = None
        current_diff = None

        for line in test_output.split('\n'):
            # Match test name: Text/ImageComparisonTestFixture.ResvgTest/e_text_002
            if "ResvgTest/" in line and "[ RUN" in line:
                match = re.search(r'ResvgTest/(\w+)', line)
                if match:
                    current_test = match.group(1).replace('_', '-') + '.svg'

            # Match COMPARE line with pixel diff
            elif "[  COMPARE ]" in line and "FAIL" in line:
                match = re.search(r'(\d+) pixels differ', line)
                if match and current_test:
                    current_diff = int(match.group(1))

            # Match SVG content start
            elif "SVG Content for" in line:
                current_svg = []
            elif current_svg is not None:
                if line.strip() == "---" and len(current_svg) > 0:
                    # End of SVG content
                    svg_content = '\n'.join(current_svg)
                    if current_test and current_diff:
                        features = detect_svg_features(svg_content)
                        category, severity = categorize_failure(current_diff, features)
                        skip_comment = suggest_skip_comment(current_test, features, category)

                        failures.append({
                            "test": current_test,
                            "pixel_diff": current_diff,
                            "features": [f.name for f in features],
                            "category": category,
                            "severity": severity,
                            "suggested_skip": skip_comment
                        })

                    current_svg = None
                    current_test = None
                    current_diff = None
                elif line.strip() != "---":
                    current_svg.append(line)

        # Group by feature
        grouped = {}
        for failure in failures:
            if failure["features"]:
                key = failure["features"][0]
            else:
                key = "other"

            if key not in grouped:
                grouped[key] = []
            grouped[key].append(failure["test"])

        result = {
            "total_failures": len(failures),
            "failures": failures,
            "grouped_by_feature": grouped,
            "summary": {
                "not_implemented": sum(1 for f in failures if f["category"] == "not_implemented"),
                "threshold_needed": sum(1 for f in failures if f["category"] == "threshold_needed"),
                "font_difference": sum(1 for f in failures if f["category"] == "font_difference"),
                "bug": sum(1 for f in failures if f["category"] == "bug")
            }
        }

        return [TextContent(
            type="text",
            text=json.dumps(result, indent=2)
        )]

    elif name == "suggest_skip_comment":
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
            "test_name": test_name,
            "category": category
        }

        return [TextContent(
            type="text",
            text=json.dumps(result, indent=2)
        )]

    elif name == "suggest_implementation_approach":
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
            "implementation_hints": hints
        }

        return [TextContent(
            type="text",
            text=json.dumps(result, indent=2)
        )]

    elif name == "find_related_tests":
        feature = arguments["feature"]
        skip_file_content = arguments["skip_file_content"]

        # Parse skip file
        skip_map = parse_skip_file(skip_file_content)

        # Find tests with this feature
        related_tests = []
        for test_name, reason in skip_map.items():
            if feature.lower() in reason.lower():
                related_tests.append(test_name)

        # Identify all missing features and group
        feature_map = identify_missing_features(skip_map)

        # Group related tests by category
        grouped_by_category = {}
        for test in related_tests:
            # Extract category prefix
            match = re.match(r'^([a-z]+-[a-z]+)-', test)
            if match:
                cat = match.group(1)
            else:
                cat = "other"

            if cat not in grouped_by_category:
                grouped_by_category[cat] = []
            grouped_by_category[cat].append(test)

        # Determine impact and priority
        impact_count = len(related_tests)
        if impact_count == 0:
            priority = "none"
            impact = "0 tests affected"
        elif impact_count == 1:
            priority = "low"
            impact = "1 test affected"
        elif impact_count <= 5:
            priority = "medium"
            impact = f"{impact_count} tests affected"
        else:
            priority = "high"
            impact = f"{impact_count} tests affected"

        result = {
            "feature": feature,
            "related_tests": related_tests,
            "impact": impact,
            "priority": priority,
            "grouped_by_category": grouped_by_category,
            "all_missing_features": {k: len(v) for k, v in feature_map.items()}
        }

        return [TextContent(
            type="text",
            text=json.dumps(result, indent=2)
        )]

    elif name == "generate_feature_report":
        category = arguments["category"]
        test_output = arguments["test_output"]
        skip_file_content = arguments.get("skip_file_content", "")

        # Parse test output
        results, summary = parse_test_output(test_output)

        # Group by category
        category_reports = group_by_category(results)

        # Get report for requested category
        if category in category_reports:
            cat_report = category_reports[category]
            total = cat_report.total_tests
            passing = cat_report.passing
            skipped = cat_report.skipped
            completion_rate = f"{int(cat_report.completion_rate * 100)}%"

            # Get implemented vs missing features
            implemented = []
            missing = []

            if skip_file_content:
                skip_map = parse_skip_file(skip_file_content)
                feature_map = identify_missing_features(skip_map, category)
                missing = list(feature_map.keys())
                next_priority_info = get_next_priority_feature(feature_map)
                if next_priority_info:
                    next_priority = f"{next_priority_info[0]} (affects {next_priority_info[1]} tests)"
                else:
                    next_priority = "None - all features implemented!"
            else:
                next_priority = "Unknown - no skip file provided"

            result = {
                "category": category,
                "total_tests": total,
                "passing": passing,
                "skipped": skipped,
                "completion_rate": completion_rate,
                "implemented_features": implemented,
                "missing_features": missing[:10],  # Top 10
                "next_priority": next_priority,
                "test_details": [
                    {
                        "name": t.test_name,
                        "status": t.status,
                        "pixel_diff": t.pixel_diff
                    }
                    for t in cat_report.test_results[:20]  # First 20 tests
                ]
            }
        else:
            result = {
                "category": category,
                "error": f"No tests found for category '{category}'",
                "available_categories": list(category_reports.keys())
            }

        return [TextContent(
            type="text",
            text=json.dumps(result, indent=2)
        )]

    elif name == "analyze_visual_diff":
        diff_image_path = arguments["diff_image_path"]
        actual_image_path = arguments["actual_image_path"]
        expected_image_path = arguments["expected_image_path"]

        try:
            # Load images
            diff_img = Image.open(diff_image_path)
            actual_img = Image.open(actual_image_path)
            expected_img = Image.open(expected_image_path)

            # Convert to numpy arrays for analysis
            diff_array = np.array(diff_img)

            # Analyze diff image
            # Find non-zero pixels (differences)
            if len(diff_array.shape) == 3:
                # RGB or RGBA image
                diff_mask = np.any(diff_array[:, :, :3] > 0, axis=2)
            else:
                # Grayscale
                diff_mask = diff_array > 0

            diff_pixels = np.sum(diff_mask)
            total_pixels = diff_mask.size

            # Find bounding boxes of diff regions
            if diff_pixels > 0:
                rows = np.any(diff_mask, axis=1)
                cols = np.any(diff_mask, axis=0)
                rmin, rmax = np.where(rows)[0][[0, -1]]
                cmin, cmax = np.where(cols)[0][[0, -1]]

                largest_region_pixels = int((rmax - rmin + 1) * (cmax - cmin + 1))

                # Check for uniform offset (all diffs in one region)
                is_uniform_offset = diff_pixels > 100 and (largest_region_pixels / diff_pixels) > 0.8
            else:
                largest_region_pixels = 0
                is_uniform_offset = False

            # Determine difference type
            if diff_pixels < 100:
                diff_type = "anti_aliasing"
                likely_cause = "Minor anti-aliasing differences"
                confidence = "high"
            elif is_uniform_offset:
                diff_type = "positioning"
                likely_cause = "Baseline positioning offset or element placement issue"
                confidence = "high"
            elif diff_pixels > total_pixels * 0.3:
                diff_type = "missing_element"
                likely_cause = "Large missing or incorrect element"
                confidence = "medium"
            else:
                diff_type = "styling"
                likely_cause = "Color, stroke, or fill differences"
                confidence = "medium"

            # Count diff regions (connected components - simplified)
            diff_region_count = 1 if diff_pixels > 0 else 0

            result = {
                "difference_type": diff_type,
                "visual_analysis": {
                    "diff_pixels": int(diff_pixels),
                    "total_pixels": int(total_pixels),
                    "diff_percentage": f"{(diff_pixels / total_pixels * 100):.2f}%",
                    "diff_regions": diff_region_count,
                    "largest_region_pixels": largest_region_pixels,
                    "is_uniform_offset": bool(is_uniform_offset),
                },
                "likely_cause": likely_cause,
                "confidence": confidence
            }

        except Exception as e:
            result = {
                "error": f"Failed to analyze images: {str(e)}",
                "diff_image_path": diff_image_path,
                "actual_image_path": actual_image_path,
                "expected_image_path": expected_image_path
            }

        return [TextContent(
            type="text",
            text=json.dumps(result, indent=2)
        )]

    else:
        raise ValueError(f"Unknown tool: {name}")


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
