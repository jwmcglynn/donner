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
