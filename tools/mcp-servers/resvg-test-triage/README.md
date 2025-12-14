# Resvg Test Triage MCP Server

An MCP (Model Context Protocol) server that automates the analysis and categorization of resvg test suite failures for the Donner SVG renderer.

## Features

- **SVG Feature Detection**: Automatically parse SVG files to identify which advanced features are being tested
- **Failure Categorization**: Classify failures as "not implemented", "threshold needed", "font difference", or "bug"
- **Skip Comment Generation**: Auto-generate properly formatted skip comments following project conventions
- **Batch Processing**: Analyze multiple test failures at once and group them by feature category
- **Vision Model Integration**: Send actual, expected, and diff images to vision models for visual analysis of rendering differences
- **Implementation Guidance**: Suggest which files to modify and provide hints for implementing missing features
- **Related Test Discovery**: Find all tests affected by the same missing feature for batch implementation
- **Feature Progress Reports**: Generate comprehensive reports showing test completion rates by category
- **Visual Diff Analysis**: Programmatically analyze diff images to categorize failure types (positioning, missing elements, styling, anti-aliasing)

## Installation

```bash
cd tools/mcp-servers/resvg-test-triage
pip install -e .
```

## Configuration

### Claude Code

Add to your MCP settings:

```json
{
  "mcpServers": {
    "resvg-test-triage": {
      "command": "python3",
      "args": ["/path/to/donner/donner/tools/mcp-servers/resvg-test-triage/server.py"],
      "env": {}
    }
  }
}
```

See [mcp-config-example.json](mcp-config-example.json) for the full configuration.

### VSCode

Add to your VSCode MCP settings (`.vscode/mcp.json` or global settings). It is added by default to the donner `.vscode/mcp.json`:

```json
{
  "servers": {
    "resvg-test-triage": {
      "type": "stdio",
      "command": "python3",
      "args": [
        "${workspaceFolder}/donner/tools/mcp-servers/resvg-test-triage/server.py"
      ]
    },
  },
  "inputs": []
}
```

## Available Tools

### `analyze_test_failure`

Analyze a single test failure with complete diagnostics. Optionally include rendered images for vision model analysis.

**Parameters:**
- `test_name` (string, required): Test file name (e.g., "e-text-023.svg")
- `svg_content` (string, required): Complete SVG source code
- `pixel_diff` (number, required): Number of pixels differing from expected
- `actual_image_path` (string, optional): Path to actual rendering PNG (Donner's output)
- `expected_image_path` (string, optional): Path to expected rendering PNG (golden reference)
- `diff_image_path` (string, optional): Path to diff image PNG (visual comparison)

**Returns:**
JSON analysis followed by PNG images (if paths provided):
```json
{
  "test_name": "e-text-023.svg",
  "pixel_diff": 8234,
  "features": [
    {
      "name": "letter_spacing",
      "category": "text_styling",
      "description": "Multiple x values for per-glyph positioning"
    }
  ],
  "category": "not_implemented",
  "severity": "major",
  "suggested_skip": "{\"e-text-002.svg\", Params::Skip()},  // Not impl: Multiple x values",
  "analysis": {
    "feature_count": 1,
    "primary_feature": "letter-spacing attribute",
    "recommendation": "Skip - feature not implemented"
  }
}
```

Plus up to 3 PNG images (actual, expected, diff) when paths are provided. Vision models can analyze these images to better understand the rendering differences.

### `batch_triage_tests`

Process multiple test failures from test output.

**Parameters:**
- `test_output` (string): Complete output from `bazel run //donner/svg/renderer/tests:resvg_test_suite`

**Returns:**
```json
{
  "total_failures": 32,
  "failures": [
    {
      "test": "e-text-002.svg",
      "pixel_diff": 13766,
      "features": ["multiple_x_values"],
      "category": "not_implemented",
      "severity": "major",
      "suggested_skip": "..."
    }
  ],
  "grouped_by_feature": {
    "multiple_x_values": ["e-text-002.svg", "e-text-003.svg"],
    "dx_attribute": ["e-text-006.svg", "e-text-007.svg"]
  },
  "summary": {
    "not_implemented": 28,
    "threshold_needed": 2,
    "font_difference": 1,
    "bug": 1
  }
}
```

### `detect_svg_features`

Parse SVG and identify features being tested.

**Parameters:**
- `svg_content` (string): SVG source code

**Returns:**
```json
{
  "features": [
    {
      "name": "multiple_x_values",
      "category": "positioning",
      "description": "Multiple x values for per-glyph positioning"
    }
  ]
}
```

### `suggest_skip_comment`

Generate a formatted skip comment.

**Parameters:**
- `test_name` (string): Test file name
- `features` (array): List of feature names
- `category` (string): One of "not_implemented", "threshold_needed", "font_difference", "bug"

**Returns:**
```json
{
  "skip_comment": "{\"e-text-023.svg\", Params::Skip()},  // Not impl: letter-spacing",
  "test_name": "e-text-023.svg",
  "category": "not_implemented"
}
```

### `suggest_implementation_approach`

Suggest which files to modify and provide implementation guidance for a failing test.

**Parameters:**
- `test_name` (string, required): Test file name (e.g., "e-text-023.svg")
- `features` (array, required): List of detected feature names
- `category` (string, required): Feature category (e.g., "text_styling", "text_positioning")
- `codebase_files` (array, optional): List of relevant files found in codebase

**Returns:**
```json
{
  "test_name": "e-text-023.svg",
  "category": "text_styling",
  "primary_feature": "letter_spacing",
  "likely_files": [
    "donner/svg/components/text/ComputedTextStyleComponent.h",
    "donner/svg/properties/PresentationAttribute.h"
  ],
  "file_patterns": [
    "**/ComputedTextStyleComponent.h",
    "**/SVGTextElement.h"
  ],
  "search_keywords": [
    "letter-spacing",
    "LetterSpacing",
    "letterspacing"
  ],
  "similar_features": [
    {
      "name": "font-size",
      "files": ["donner/svg/components/text/ComputedTextStyleComponent.h"]
    }
  ],
  "implementation_hints": [
    "Text styling properties are typically CSS properties that need to be parsed and applied.",
    "Check PresentationAttribute.h for existing property definitions.",
    "letter-spacing is a CSS property that adds space between characters."
  ]
}
```

### `find_related_tests`

Find all tests failing for the same feature or reason.

**Parameters:**
- `feature` (string, required): Feature name to find related tests for
- `skip_file_content` (string, required): Content of resvg_test_suite.cc with skip entries

**Returns:**
```json
{
  "feature": "letter-spacing",
  "related_tests": ["e-text-023.svg"],
  "impact": "1 test affected",
  "priority": "low",
  "grouped_by_category": {
    "e-text": ["e-text-023.svg", "e-text-024.svg"]
  },
  "all_missing_features": {
    "letter-spacing": 2,
    "dx attribute": 5,
    "textPath": 3
  }
}
```

### `generate_feature_report`

Generate comprehensive progress report for a feature category.

**Parameters:**
- `category` (string, required): Category prefix (e.g., "e-text", "a-transform")
- `test_output` (string, required): Recent test run output from bazel test
- `skip_file_content` (string, optional): Content of resvg_test_suite.cc for missing feature analysis

**Returns:**
```json
{
  "category": "e-text",
  "total_tests": 15,
  "passing": 3,
  "skipped": 12,
  "completion_rate": "20%",
  "implemented_features": [],
  "missing_features": [
    "dx attribute",
    "dy attribute",
    "letter-spacing"
  ],
  "next_priority": "dx attribute (affects 3 tests)",
  "test_details": [
    {
      "name": "e-text-001.svg",
      "status": "PASSED",
      "pixel_diff": null
    }
  ]
}
```

### `analyze_visual_diff`

Programmatically analyze diff images to categorize failure types.

**Parameters:**
- `diff_image_path` (string, required): Path to diff image PNG
- `actual_image_path` (string, required): Path to actual rendering PNG
- `expected_image_path` (string, required): Path to expected rendering PNG

**Returns:**
```json
{
  "difference_type": "positioning",
  "visual_analysis": {
    "diff_pixels": 3421,
    "total_pixels": 65536,
    "diff_percentage": "5.22%",
    "diff_regions": 1,
    "largest_region_pixels": 3421,
    "is_uniform_offset": true
  },
  "likely_cause": "Baseline positioning offset or element placement issue",
  "confidence": "high"
}
```

**Difference Types:**
- `anti_aliasing`: Minor differences (<100 pixels), likely rendering artifacts
- `positioning`: Uniform offset detected, element placement issue
- `missing_element`: Large missing or incorrect element (>30% of image)
- `styling`: Color, stroke, or fill differences

## Example Workflows

### Basic Analysis (Text Only)

```python
# 1. Run tests and capture output
test_output = """
[  COMPARE ] .../e-text-023.svg: FAIL (8234 pixels differ, with 100 max)
SVG Content for e-text-023.svg:
---
<svg><text x="30" y="100" letter-spacing="10">Text</text></svg>
---
"""

# 2. Call MCP server to batch analyze
result = await mcp.call_tool("batch_triage_tests", {
    "test_output": test_output
})

# 3. Server returns categorized failures with skip comments
# 4. Agent updates resvg_test_suite.cc with generated skips
```

### Vision Model Analysis (With Images)

```python
# 1. Analyze a single test with images for vision model
result = await mcp.call_tool("analyze_test_failure", {
    "test_name": "e-text-023.svg",
    "svg_content": svg_source,
    "pixel_diff": 8234,
    "actual_image_path": "/var/folders/.../e-text-023.png",
    "expected_image_path": "/path/to/resvg-test-suite/png/e-text-023.png",
    "diff_image_path": "/var/folders/.../diff_e-text-023.png"
})

# 2. Vision model receives:
#    - JSON analysis with feature detection and categorization
#    - Actual rendering (what Donner produced)
#    - Expected rendering (golden reference)
#    - Diff image (visual comparison)
#
# 3. Vision model can now:
#    - See exactly what went wrong visually
#    - Identify positioning, styling, or rendering issues
#    - Provide more accurate categorization
#    - Suggest specific fixes based on visual differences
```

### Implementation Guidance Workflow

```python
# 1. Detect what feature is needed
features = await mcp.call_tool("detect_svg_features", {
    "svg_content": svg_source
})

# 2. Get implementation guidance
guidance = await mcp.call_tool("suggest_implementation_approach", {
    "test_name": "e-text-023.svg",
    "features": ["letter_spacing"],
    "category": "text_styling",
    "codebase_files": []  # Agent can populate with glob/grep results
})

# 3. Agent uses guidance to:
#    - Search for files matching patterns
#    - Look at similar feature implementations
#    - Follow implementation hints
#    - Make targeted code changes
```

### Feature Progress Tracking

```python
# 1. Generate progress report for a category
report = await mcp.call_tool("generate_feature_report", {
    "category": "e-text",
    "test_output": bazel_test_output,
    "skip_file_content": resvg_test_suite_cc_content
})

# 2. Identify next priority
# report["next_priority"] = "dx attribute (affects 3 tests)"

# 3. Find all related tests
related = await mcp.call_tool("find_related_tests", {
    "feature": "dx attribute",
    "skip_file_content": resvg_test_suite_cc_content
})

# 4. Batch implement the feature for all 3 tests at once
```

### Visual Diff Analysis

```python
# 1. Analyze diff image programmatically
analysis = await mcp.call_tool("analyze_visual_diff", {
    "diff_image_path": "/tmp/diff.png",
    "actual_image_path": "/tmp/actual.png",
    "expected_image_path": "/tmp/expected.png"
})

# 2. Use analysis to categorize failure
if analysis["difference_type"] == "positioning":
    # Focus on position-related code
    # analysis["likely_cause"] = "Baseline positioning offset"
elif analysis["difference_type"] == "missing_element":
    # Feature likely not implemented
    pass
elif analysis["difference_type"] == "anti_aliasing":
    # Just needs threshold adjustment
    pass
```

## Detected Features

The server can detect these SVG features:

**Positioning:**
- Multiple x/y values
- dx/dy attributes
- rotate attribute
- textLength

**Elements:**
- `<tspan>`
- `<textPath>`

**Styling:**
- text-anchor
- letter-spacing
- word-spacing
- text-decoration
- font-weight
- font-style
- font-variant

**Text Layout:**
- writing-mode
- baseline-shift
- alignment-baseline
- dominant-baseline

## Comment Format Conventions

The server follows established conventions:

- `Not impl: <feature>` - Feature not yet implemented
- `Not impl: <element>` - SVG element not supported
- `UB: <reason>` - Undefined behavior
- `Bug: <description>` - Known bug
- `Larger threshold due to <reason>` - Anti-aliasing artifacts

## Integration with Test Documentation

See [README_resvg_test_suite.md](../../../donner/svg/renderer/tests/README_resvg_test_suite.md) for detailed test triage workflow.
