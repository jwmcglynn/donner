"""
Unit tests for test_output_parser module.
"""

import pytest
from test_output_parser import (
    extract_category_from_path,
    parse_test_output,
    parse_skip_file,
    group_by_category,
    identify_missing_features,
    get_next_priority_feature,
    TestResult,
)


def test_extract_category_from_path():
    """Extract the directory portion from an SVG runfile path."""
    assert (
        extract_category_from_path(
            "/abs/path/resvg-test-suite/tests/painting/fill/rgb-int-int-int.svg"
        )
        == "painting/fill"
    )
    assert (
        extract_category_from_path(
            "/runfiles/+non_bcr_deps+resvg-test-suite/tests/text/textPath/simple-case.svg"
        )
        == "text/textPath"
    )
    assert extract_category_from_path("tests/filters/feGaussianBlur/complex-transform.svg") == "filters/feGaussianBlur"
    assert extract_category_from_path("some/unrelated/path.svg") is None
    assert extract_category_from_path("") is None
    # A path under tests/ but with no subdirectory before the file is not a
    # category (there is no valid leaf dir).
    assert extract_category_from_path("tests/orphan.svg") is None


def test_parse_test_output_failing_and_passing():
    """Parse a minimal mixed output covering PASS and FAIL."""
    test_output = """
[ RUN      ] PaintingFill/ImageComparisonTestFixture.ResvgTest/rgb_int_int_int
[  COMPARE ] /root/tests/painting/fill/rgb-int-int-int.svg [TinySkia]: 1234 pixels differ
[  FAILED  ] PaintingFill/ImageComparisonTestFixture.ResvgTest/rgb_int_int_int
[ RUN      ] PaintingFill/ImageComparisonTestFixture.ResvgTest/named_color
[  COMPARE ] /root/tests/painting/fill/named-color.svg [TinySkia]: 0 pixels
[       OK ] PaintingFill/ImageComparisonTestFixture.ResvgTest/named_color
"""
    results, summary = parse_test_output(test_output)

    assert len(results) == 2

    by_name = {r.test_name: r for r in results}
    assert "rgb-int-int-int.svg" in by_name
    assert by_name["rgb-int-int-int.svg"].status == "FAILED"
    assert by_name["rgb-int-int-int.svg"].pixel_diff == 1234
    assert by_name["rgb-int-int-int.svg"].category == "painting/fill"

    assert by_name["named-color.svg"].status == "PASSED"
    assert by_name["named-color.svg"].category == "painting/fill"

    assert summary.total_tests == 2
    assert summary.passed == 1
    assert summary.failed == 1
    assert summary.completion_rate == 0.5


def test_parse_test_output_where_getparam_summary_line():
    """FAILED summary lines carry the path via `where GetParam() = <path>`."""
    test_output = """
[  FAILED  ] TextTextPath/ImageComparisonTestFixture.ResvgTest/simple_case, where GetParam() = /runfiles/resvg-test-suite/tests/text/textPath/simple-case.svg
"""
    results, _ = parse_test_output(test_output)
    assert len(results) == 1
    assert results[0].test_name == "simple-case.svg"
    assert results[0].status == "FAILED"
    assert results[0].category == "text/textPath"


def test_parse_test_output_runtime_skipped_without_compare_line():
    """Runtime-skipped tests (GTEST_SKIP() before [ COMPARE ]) must still
    produce a record. The fixture's [ COMPARE ] line never fires because
    skipReasonIfUnsupported() returned a reason, so current_svg_path is
    None when [ SKIPPED ] arrives. Recover via the test ID instead of
    silently dropping the record."""
    test_output = """
[ RUN      ] TextLetterSpacing/ImageComparisonTestFixture.ResvgTest/on_Arabic
.../ImageComparisonTestFixture.cc:599: Skipped
TinySkia backend does not support Arabic text shaping
[  SKIPPED ] TextLetterSpacing/ImageComparisonTestFixture.ResvgTest/on_Arabic (2 ms)
"""
    results, _ = parse_test_output(test_output)
    assert len(results) == 1
    assert results[0].status == "SKIPPED"
    # No SVG path was emitted, so test_name falls back to the test ID.
    assert "on_Arabic" in results[0].test_name
    assert results[0].category is None


def test_parse_test_output_skipped_with_where_getparam():
    """If a test fixture is updated to print [  COMPARE  ] before the skip
    check, or if GoogleTest's summary block ever starts including
    `where GetParam() = …` for skipped parameterized tests, the parser
    should pick up the path the same way it does for FAILED."""
    test_output = """
[  SKIPPED ] PaintingFill/ImageComparisonTestFixture.ResvgTest/example, where GetParam() = /runfiles/resvg-test-suite/tests/painting/fill/example.svg
"""
    results, _ = parse_test_output(test_output)
    assert len(results) == 1
    assert results[0].status == "SKIPPED"
    assert results[0].test_name == "example.svg"
    assert results[0].category == "painting/fill"


def test_parse_skip_file_new_form():
    """New Params::Skip("reason") syntax is the primary format."""
    skip_content = '''
    {"rgb-int-int-int.svg", Params::Skip("UB: rgb(int int int)")},
    {"simple-case.svg", Params::Skip("Not impl: textPath")},
    {"emoji.svg", Params::Skip("Not impl: Color emoji font")},
    '''
    skip_map = parse_skip_file(skip_content)
    assert skip_map["rgb-int-int-int.svg"] == "UB: rgb(int int int)"
    assert skip_map["simple-case.svg"] == "Not impl: textPath"
    assert skip_map["emoji.svg"] == "Not impl: Color emoji font"


def test_parse_skip_file_legacy_form():
    """Legacy trailing-comment form is still recognized as a fallback."""
    skip_content = """
    {"legacy-test.svg", Params::Skip()},  // Not impl: dx attribute
    """
    skip_map = parse_skip_file(skip_content)
    assert skip_map["legacy-test.svg"] == "Not impl: dx attribute"


def test_parse_skip_file_new_form_wins_over_legacy():
    """If both forms are present for the same test, new form takes priority."""
    skip_content = '''
    {"foo.svg", Params::Skip("new reason")},
    {"foo.svg", Params::Skip()},  // legacy reason
    '''
    skip_map = parse_skip_file(skip_content)
    assert skip_map["foo.svg"] == "new reason"


def test_group_by_category():
    """Group by the new directory-based category."""
    results = [
        TestResult("rgb.svg", "PASSED", None, "painting/fill"),
        TestResult("icc.svg", "FAILED", 100, "painting/fill"),
        TestResult("simple-case.svg", "PASSED", None, "text/textPath"),
    ]

    grouped = group_by_category(results)

    assert set(grouped) == {"painting/fill", "text/textPath"}

    fill = grouped["painting/fill"]
    assert fill.total_tests == 2
    assert fill.passing == 1
    assert fill.failing == 1
    assert fill.completion_rate == 0.5

    tp = grouped["text/textPath"]
    assert tp.total_tests == 1
    assert tp.passing == 1
    assert tp.completion_rate == 1.0


def test_identify_missing_features():
    """Extract missing feature names from skip reasons."""
    skip_map = {
        "a.svg": "Not impl: dx attribute",
        "b.svg": "Not impl: dx attribute",
        "c.svg": "Not impl: `letter-spacing`",
        "d.svg": "Not impl: <textPath>",
        "e.svg": "M1 upgrade: needs triage",  # no feature extracted
    }
    features = identify_missing_features(skip_map)

    assert set(features) >= {"dx attribute", "letter-spacing", "textPath"}
    assert set(features["dx attribute"]) == {"a.svg", "b.svg"}
    assert features["letter-spacing"] == ["c.svg"]
    assert features["textPath"] == ["d.svg"]
    # M1-style reasons should not contribute a feature entry.
    assert "needs triage" not in features


def test_get_next_priority_feature():
    feature_map = {
        "dx attribute": ["a.svg", "b.svg", "c.svg"],
        "letter-spacing": ["d.svg"],
        "rotate": ["e.svg", "f.svg"],
    }
    result = get_next_priority_feature(feature_map)
    assert result == ("dx attribute", 3)


def test_get_next_priority_feature_empty():
    assert get_next_priority_feature({}) is None


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
