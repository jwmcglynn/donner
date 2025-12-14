"""
Unit tests for test_output_parser module.
"""

import pytest
from test_output_parser import (
    extract_test_prefix,
    parse_test_output,
    parse_skip_file,
    group_by_category,
    identify_missing_features,
    get_next_priority_feature,
    TestResult,
)


def test_extract_test_prefix():
    """Test extracting category prefix from test names."""
    assert extract_test_prefix("e-text-002.svg") == "e-text"
    assert extract_test_prefix("a-transform-001.svg") == "a-transform"
    assert extract_test_prefix("custom-test.svg") is None
    assert extract_test_prefix("invalid") is None


def test_parse_test_output():
    """Test parsing bazel test output."""
    test_output = """
[ RUN      ] ResvgTest/e_text_002
[  COMPARE ] FAIL: 1234 pixels differ
[  FAILED  ] ResvgTest/e_text_002
[ RUN      ] ResvgTest/e_text_003
[       OK ] ResvgTest/e_text_003
    """

    results, summary = parse_test_output(test_output)

    # Check results
    assert len(results) == 2

    # First test should be failed
    assert results[0].test_name == "e-text-002.svg"
    assert results[0].status == "FAILED"
    assert results[0].pixel_diff == 1234
    assert results[0].category_prefix == "e-text"

    # Second test should be passed
    assert results[1].test_name == "e-text-003.svg"
    assert results[1].status == "PASSED"
    assert results[1].category_prefix == "e-text"

    # Check summary
    assert summary.total_tests == 2
    assert summary.passed == 1
    assert summary.failed == 1
    assert summary.skipped == 0
    assert summary.completion_rate == 0.5


def test_parse_skip_file():
    """Test parsing skip file entries."""
    skip_content = """
    {"e-text-002.svg", Params::Skip()},  // Not impl: dx attribute
    {"e-text-003.svg", Params::Skip()},  // Not impl: `letter-spacing`
    {"e-text-004.svg", Params::Skip()},  // Not impl: <tspan>
    """

    skip_map = parse_skip_file(skip_content)

    assert len(skip_map) == 3
    assert skip_map["e-text-002.svg"] == "Not impl: dx attribute"
    assert skip_map["e-text-003.svg"] == "Not impl: `letter-spacing`"
    assert skip_map["e-text-004.svg"] == "Not impl: <tspan>"


def test_group_by_category():
    """Test grouping test results by category."""
    results = [
        TestResult("e-text-001.svg", "PASSED", None, "e-text"),
        TestResult("e-text-002.svg", "FAILED", 100, "e-text"),
        TestResult("a-transform-001.svg", "PASSED", None, "a-transform"),
    ]

    grouped = group_by_category(results)

    assert len(grouped) == 2
    assert "e-text" in grouped
    assert "a-transform" in grouped

    # Check e-text category
    e_text = grouped["e-text"]
    assert e_text.total_tests == 2
    assert e_text.passing == 1
    assert e_text.failing == 1
    assert e_text.completion_rate == 0.5

    # Check a-transform category
    a_transform = grouped["a-transform"]
    assert a_transform.total_tests == 1
    assert a_transform.passing == 1
    assert a_transform.completion_rate == 1.0


def test_identify_missing_features():
    """Test identifying missing features from skip map."""
    skip_map = {
        "e-text-001.svg": "Not impl: dx attribute",
        "e-text-002.svg": "Not impl: dx attribute",
        "e-text-003.svg": "Not impl: `letter-spacing`",
        "a-transform-001.svg": "Not impl: rotate attribute",
    }

    # Test without category filter
    features = identify_missing_features(skip_map)

    assert len(features) == 3
    assert "dx attribute" in features
    assert len(features["dx attribute"]) == 2
    assert "letter-spacing" in features
    assert len(features["letter-spacing"]) == 1

    # Test with category filter
    features = identify_missing_features(skip_map, category="e-text")

    assert len(features) == 2
    assert "dx attribute" in features
    assert "letter-spacing" in features
    assert "rotate attribute" not in features  # Different category


def test_get_next_priority_feature():
    """Test identifying next priority feature."""
    feature_map = {
        "dx attribute": ["e-text-001.svg", "e-text-002.svg", "e-text-003.svg"],
        "letter-spacing": ["e-text-004.svg"],
        "rotate": ["e-text-005.svg", "e-text-006.svg"],
    }

    feature, count = get_next_priority_feature(feature_map)

    # Should return the feature affecting most tests
    assert feature == "dx attribute"
    assert count == 3


def test_get_next_priority_feature_empty():
    """Test get_next_priority_feature with empty map."""
    result = get_next_priority_feature({})
    assert result is None


def test_parse_test_output_no_failures():
    """Test parsing output with no test failures."""
    test_output = """
[ RUN      ] ResvgTest/e_text_001
[       OK ] ResvgTest/e_text_001
    """

    results, summary = parse_test_output(test_output)

    assert len(results) == 1
    assert summary.passed == 1
    assert summary.failed == 0
    assert summary.completion_rate == 1.0


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
