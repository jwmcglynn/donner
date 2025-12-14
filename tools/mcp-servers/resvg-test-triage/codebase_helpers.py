"""
Helper utilities for searching and analyzing the Donner codebase.

This module provides tools for finding files, searching for similar features,
and understanding the codebase structure to support implementation guidance.
"""

from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass
class FilePattern:
    """Represents a file pattern mapping for feature categories."""
    category: str
    patterns: list[str]
    description: str


# Known file patterns for different feature categories
CATEGORY_FILE_PATTERNS = {
    "text_styling": FilePattern(
        category="text_styling",
        patterns=[
            "**/ComputedTextStyleComponent.h",
            "**/SVGTextElement.h",
            "**/svg/properties/PresentationAttribute.h",
        ],
        description="Text styling properties (font-weight, letter-spacing, etc.)"
    ),
    "text_positioning": FilePattern(
        category="text_positioning",
        patterns=[
            "**/SVGTextElement.h",
            "**/SVGTextPositioningElement.h",
            "**/text/*.cc",
        ],
        description="Text positioning attributes (x, y, dx, dy, rotate)"
    ),
    "text_elements": FilePattern(
        category="text_elements",
        patterns=[
            "**/SVGTSpanElement.h",
            "**/SVGTextPathElement.h",
            "**/svg/components/text/*.h",
        ],
        description="Text element types (tspan, textPath)"
    ),
    "text_layout": FilePattern(
        category="text_layout",
        patterns=[
            "**/SVGTextElement.h",
            "**/text/*.cc",
            "**/ComputedTextStyleComponent.h",
        ],
        description="Text layout properties (writing-mode, baseline-shift)"
    ),
}


def get_file_patterns_for_category(category: str) -> list[str]:
    """
    Get glob patterns for files likely related to a feature category.

    Args:
        category: Feature category (e.g., "text_styling", "text_positioning")

    Returns:
        List of glob patterns to search
    """
    if category in CATEGORY_FILE_PATTERNS:
        return CATEGORY_FILE_PATTERNS[category].patterns
    return []


def get_search_keywords_for_feature(feature_name: str) -> list[str]:
    """
    Generate search keywords for finding similar feature implementations.

    Args:
        feature_name: Feature to search for (e.g., "letter_spacing", "dx_attribute")

    Returns:
        List of keywords to search in codebase
    """
    keywords = []

    # Convert underscore to various formats
    # letter_spacing -> letter-spacing, letterSpacing, LetterSpacing
    base_name = feature_name.replace("_attribute", "").replace("_", "-")
    keywords.append(base_name)

    # CamelCase version
    camel_case = ''.join(word.capitalize() for word in feature_name.replace("_attribute", "").split('_'))
    keywords.append(camel_case)

    # lowercase no separator
    keywords.append(feature_name.replace("_", "").lower())

    # Attribute name format for XML/SVG
    if "attribute" in feature_name:
        keywords.append(f'"{base_name}"')

    return keywords


@dataclass
class SimilarFeature:
    """Represents a similar feature found in the codebase."""
    name: str
    files: list[str]
    confidence: float  # 0.0 to 1.0


def rank_file_suggestions(files: list[str], feature_name: str) -> list[tuple[str, float]]:
    """
    Rank file suggestions by relevance to the feature.

    Args:
        files: List of file paths to rank
        feature_name: Feature being implemented

    Returns:
        List of (file_path, confidence_score) tuples, sorted by confidence
    """
    ranked = []

    for file_path in files:
        confidence = 0.0
        file_lower = file_path.lower()
        feature_lower = feature_name.lower()

        # Boost for exact feature name match
        if feature_lower.replace("_", "") in file_lower.replace("_", ""):
            confidence += 0.5

        # Boost for category match
        for category, pattern_info in CATEGORY_FILE_PATTERNS.items():
            if any(keyword in file_lower for keyword in category.split("_")):
                confidence += 0.3
                break

        # Boost for component files (likely implementation files)
        if "component" in file_lower:
            confidence += 0.2

        # Boost for header files (interface definitions)
        if file_path.endswith(".h"):
            confidence += 0.1

        # Penalty for test files
        if "test" in file_lower:
            confidence -= 0.3

        ranked.append((file_path, min(1.0, max(0.0, confidence))))

    # Sort by confidence (highest first)
    ranked.sort(key=lambda x: x[1], reverse=True)
    return ranked


def get_implementation_hints(feature_name: str, category: str) -> list[str]:
    """
    Generate implementation hints for a feature.

    Args:
        feature_name: Feature being implemented
        category: Feature category

    Returns:
        List of helpful hints for implementation
    """
    hints = []

    # General hints based on category
    if category == "text_styling":
        hints.append("Text styling properties are typically CSS properties that need to be parsed and applied.")
        hints.append("Check PresentationAttribute.h for existing property definitions.")
        hints.append("Look for similar properties like font-size or font-family as examples.")

    elif category == "text_positioning":
        hints.append("Positioning attributes often involve parsing space-separated number lists.")
        hints.append("Check how existing attributes like 'x' and 'y' are handled in SVGTextElement.")
        hints.append("May need to update text layout code to apply positioning values.")

    elif category == "text_elements":
        hints.append("New elements require a class definition (e.g., SVGTSpanElement).")
        hints.append("Elements need to be registered in the SVG element factory.")
        hints.append("Check existing text elements for implementation patterns.")

    elif category == "text_layout":
        hints.append("Layout properties often affect how text is measured and positioned.")
        hints.append("May require changes to text rendering pipeline.")
        hints.append("Check ComputedTextStyleComponent for text layout state.")

    # Feature-specific hints
    feature_hints = {
        "letter_spacing": [
            "letter-spacing is a CSS property that adds space between characters.",
            "Look at how font-size is implemented as a reference."
        ],
        "dx_attribute": [
            "dx provides relative positioning offsets for each character.",
            "Similar to x attribute but applies offsets instead of absolute positions."
        ],
        "dy_attribute": [
            "dy provides vertical offsets for each character.",
            "Works in conjunction with dx for full positional control."
        ],
        "textPath": [
            "textPath requires path parsing and text-on-path layout.",
            "Complex feature that may need path position calculation utilities."
        ],
    }

    feature_key = feature_name.replace("_attribute", "").replace("_", "_")
    if feature_key in feature_hints:
        hints.extend(feature_hints[feature_key])

    return hints
