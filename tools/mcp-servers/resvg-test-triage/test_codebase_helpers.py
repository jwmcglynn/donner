"""
Unit tests for codebase_helpers module.
"""

import pytest
from codebase_helpers import (
    get_file_patterns_for_category,
    get_search_keywords_for_feature,
    rank_file_suggestions,
    get_implementation_hints,
)


def test_get_file_patterns_for_category():
    """Test that category patterns are returned correctly."""
    # Test known categories
    patterns = get_file_patterns_for_category("text_styling")
    assert len(patterns) > 0
    assert any("ComputedTextStyleComponent" in p for p in patterns)

    patterns = get_file_patterns_for_category("text_positioning")
    assert len(patterns) > 0
    assert any("SVGTextElement" in p for p in patterns)

    # Test unknown category
    patterns = get_file_patterns_for_category("unknown_category")
    assert patterns == []


def test_get_search_keywords_for_feature():
    """Test keyword generation for features."""
    keywords = get_search_keywords_for_feature("letter_spacing")
    assert "letter-spacing" in keywords
    assert "LetterSpacing" in keywords
    assert "letterspacing" in keywords

    keywords = get_search_keywords_for_feature("dx_attribute")
    assert "dx" in keywords
    assert "Dx" in keywords
    assert '"dx"' in keywords  # Attribute format


def test_rank_file_suggestions():
    """Test file ranking algorithm."""
    files = [
        "donner/svg/components/text/ComputedTextStyleComponent.h",
        "donner/svg/SVGTextElement.h",
        "donner/svg/tests/TextTest.cc",
        "donner/svg/properties/PresentationAttribute.h",
    ]

    # Rank for letter_spacing feature
    ranked = rank_file_suggestions(files, "letter_spacing")

    # Should return list of tuples (file, confidence)
    assert len(ranked) == len(files)
    assert all(isinstance(item, tuple) for item in ranked)
    assert all(len(item) == 2 for item in ranked)

    # Test files should have lower confidence
    test_file_rank = next(i for i, (f, _) in enumerate(ranked) if "Test" in f)
    assert test_file_rank > 0  # Not the top result

    # Component files should have higher confidence
    component_file_rank = next(i for i, (f, _) in enumerate(ranked) if "Component" in f)
    assert component_file_rank < len(files) - 1  # Not the worst result


def test_get_implementation_hints():
    """Test implementation hints generation."""
    # Test text_styling category
    hints = get_implementation_hints("letter_spacing", "text_styling")
    assert len(hints) > 0
    assert any("CSS property" in hint for hint in hints)

    # Test text_positioning category
    hints = get_implementation_hints("dx_attribute", "text_positioning")
    assert len(hints) > 0
    assert any("offset" in hint.lower() for hint in hints)

    # Test feature-specific hints
    hints = get_implementation_hints("letter_spacing", "text_styling")
    assert any("letter-spacing" in hint for hint in hints)


def test_rank_file_suggestions_confidence_scores():
    """Test that confidence scores are in valid range [0, 1]."""
    files = [
        "donner/svg/components/text/ComputedTextStyleComponent.h",
        "random/unrelated/file.cc",
    ]

    ranked = rank_file_suggestions(files, "letter_spacing")

    for file, confidence in ranked:
        assert 0.0 <= confidence <= 1.0, f"Confidence {confidence} out of range for {file}"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
