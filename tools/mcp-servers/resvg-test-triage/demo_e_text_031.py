#!/usr/bin/env python3
"""
Demo: Testing MCP tools on e-text-031.svg triage
"""

import json
from test_output_parser import parse_skip_file, identify_missing_features, get_next_priority_feature
from codebase_helpers import (
    get_file_patterns_for_category,
    get_search_keywords_for_feature,
    get_implementation_hints,
)

# Sample skip file content from resvg_test_suite.cc
skip_file_content = """
{"e-text-025.svg", Params::Skip()},  // Not impl: `text-decoration`
{"e-text-026.svg", Params::Skip()},  // Not impl: `text-decoration`
{"e-text-027.svg", Params::Skip()},  // Not impl: Color emoji font (Noto Color Emoji)
{"e-text-028.svg", Params::Skip()},  // Not impl: `font-weight`
{"e-text-029.svg", Params::Skip()},  // Not impl: `font-style`
{"e-text-030.svg", Params::Skip()},  // Not impl: `font-variant`
{"e-text-031.svg", Params::Skip()},  // Not impl: Vertical text / writing-mode
{"e-text-033.svg", Params::Skip()},  // Not impl: Vertical text / writing-mode
{"e-text-034.svg", Params::Skip()},  // Not impl: `baseline-shift`
{"e-text-035.svg", Params::Skip()},  // Not impl: `baseline-shift`
{"e-text-036.svg", Params::Skip()},  // Not impl: `alignment-baseline`
{"e-text-038.svg", Params::Skip()},  // Not impl: `dominant-baseline`
{"e-text-040.svg", Params::Skip()},  // Not impl: <tspan>
{"e-text-041.svg", Params::Skip()},  // Not impl: <tspan>
{"e-text-042.svg", Params::Skip()},  // Not impl: <textPath>
{"e-text-043.svg", Params::Skip()},  // Not impl: <textPath>
"""

print("=" * 80)
print("MCP Test Triage Demo: Analyzing e-text-031.svg")
print("=" * 80)
print()

# Tool 1: find_related_tests for "writing-mode"
print("🔍 Tool 1: find_related_tests")
print("-" * 80)

skip_map = parse_skip_file(skip_file_content)
related_tests = []
for test_name, reason in skip_map.items():
    if "writing-mode" in reason.lower():
        related_tests.append(test_name)

result = {
    "feature": "writing-mode",
    "related_tests": related_tests,
    "impact": f"{len(related_tests)} tests affected",
    "priority": "medium" if len(related_tests) <= 5 else "high"
}

print(json.dumps(result, indent=2))
print()

# Tool 2: Identify all missing features
print("📊 Tool 2: identify_missing_features (e-text category)")
print("-" * 80)

feature_map = identify_missing_features(skip_map, category="e-text")
next_priority = get_next_priority_feature(feature_map)

print(f"Missing features in e-text tests:")
for feature, tests in sorted(feature_map.items(), key=lambda x: len(x[1]), reverse=True):
    print(f"  - {feature}: {len(tests)} test(s)")

print()
if next_priority:
    print(f"🎯 Next Priority: {next_priority[0]} (affects {next_priority[1]} tests)")
print()

# Tool 3: suggest_implementation_approach for writing-mode
print("💡 Tool 3: suggest_implementation_approach")
print("-" * 80)

patterns = get_file_patterns_for_category("text_layout")
keywords = get_search_keywords_for_feature("writing_mode")
hints = get_implementation_hints("writing_mode", "text_layout")

guidance = {
    "test_name": "e-text-031.svg",
    "category": "text_layout",
    "primary_feature": "writing_mode",
    "file_patterns": patterns,
    "search_keywords": keywords,
    "implementation_hints": hints
}

print(json.dumps(guidance, indent=2))
print()

print("=" * 80)
print("Summary:")
print("=" * 80)
print(f"✓ Found {len(related_tests)} tests affected by writing-mode")
print(f"✓ Identified {len(feature_map)} missing features in e-text")
print(f"✓ Provided implementation guidance for writing-mode")
print()
print("Next steps:")
print("1. Search for files matching the patterns above")
print("2. Look for similar features in the codebase")
print("3. Implement writing-mode support for both tests")
