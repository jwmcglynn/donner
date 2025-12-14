#!/usr/bin/env python3
"""
Complete MCP Triage Demo: e-text-031.svg with actual file ranking
"""

import json
from test_output_parser import parse_skip_file, identify_missing_features
from codebase_helpers import rank_file_suggestions, get_implementation_hints

print("=" * 80)
print("Complete MCP Triage Workflow: e-text-031.svg (writing-mode)")
print("=" * 80)
print()

# Step 1: Find files in codebase
print("Step 1: Codebase files found (via glob)")
print("-" * 80)

codebase_files = [
    "donner/svg/SVGTextElement.h",
    "donner/svg/SVGTextPositioningElement.h",
    "donner/svg/components/text/TextFlowComponent.h",
    "donner/svg/components/text/TextComponent.h",
    "donner/svg/components/text/ComputedTextComponent.h",
    "donner/svg/components/text/ComputedTextStyleComponent.h",
    "donner/svg/components/text/TextPositioningComponent.h",
    "donner/svg/components/text/TextRootComponent.h"
]

for f in codebase_files:
    print(f"  - {f}")
print()

# Step 2: Rank files by relevance
print("Step 2: suggest_implementation_approach (with file ranking)")
print("-" * 80)

ranked_files = rank_file_suggestions(codebase_files, "writing_mode")
likely_files = [f[0] for f in ranked_files[:5]]

hints = get_implementation_hints("writing_mode", "text_layout")

result = {
    "test_name": "e-text-031.svg",
    "category": "text_layout",
    "primary_feature": "writing_mode",
    "likely_files": likely_files,
    "file_rankings": [
        {"file": f, "confidence": f"{c:.2f}"} for f, c in ranked_files[:5]
    ],
    "search_keywords": [
        "writing-mode",
        "WritingMode",
        "writingmode"
    ],
    "similar_features": [
        {
            "name": "text-direction",
            "category": "text_layout"
        }
    ],
    "implementation_hints": hints
}

print(json.dumps(result, indent=2))
print()

# Step 3: Related tests
print("Step 3: find_related_tests (batch implementation)")
print("-" * 80)

skip_content = """
{"e-text-031.svg", Params::Skip()},  // Not impl: Vertical text / writing-mode
{"e-text-033.svg", Params::Skip()},  // Not impl: Vertical text / writing-mode
"""

skip_map = parse_skip_file(skip_content)
related = [t for t, r in skip_map.items() if "writing-mode" in r.lower()]

print(json.dumps({
    "feature": "writing-mode",
    "related_tests": related,
    "impact": f"{len(related)} tests affected",
    "priority": "medium",
    "batch_implementation": "Implement writing-mode to fix both tests at once"
}, indent=2))
print()

# Summary
print("=" * 80)
print("Implementation Guidance Summary")
print("=" * 80)
print()
print("📁 Top files to modify:")
for i, (file, conf) in enumerate(ranked_files[:3], 1):
    print(f"  {i}. {file} (confidence: {conf:.2f})")
print()
print("🔍 Search for these keywords:")
print("  - writing-mode, WritingMode")
print("  - vertical text, text direction")
print()
print("💡 Implementation hints:")
for i, hint in enumerate(hints[:3], 1):
    print(f"  {i}. {hint}")
print()
print("✅ Batch opportunity:")
print(f"  Implementing writing-mode will fix {len(related)} tests:")
for test in related:
    print(f"    - {test}")
