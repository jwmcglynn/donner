# MCP Test Triage Server

## Overview

The `resvg-test-triage` MCP server provides 8 AI-assisted tools for automated test analysis and feature implementation guidance. It accelerates test triage by automatically detecting SVG features, categorizing failures, suggesting implementation approaches, and tracking progress across the test suite.

**Location:** `tools/mcp-servers/resvg-test-triage/`

## Architecture

The server is built on three core modules:

1. **server.py** - MCP server with 8 tool implementations
2. **codebase_helpers.py** - File search patterns, ranking, and implementation hints
3. **test_output_parser.py** - Test output parsing, skip file analysis, and feature identification

### Dependencies

- `mcp>=0.9.0` - MCP server framework
- `Pillow>=10.0.0` - Image processing for visual diff analysis
- `numpy>=1.24.0` - Numerical analysis for image processing
- `pytest>=7.0.0` (dev) - Unit testing

## Tool Reference

### Core Triage Tools

#### 1. `analyze_test_failure`

Analyzes a single test failure with comprehensive diagnostics.

**Purpose:** Detect SVG features, categorize failure type, and generate skip comments.

**Input:**
```json
{
  "test_name": "e-text-023.svg",
  "svg_content": "<svg>...</svg>",
  "pixel_diff": 8234,
  "actual_image_path": "/tmp/actual.png",      // Optional
  "expected_image_path": "/tmp/expected.png",  // Optional
  "diff_image_path": "/tmp/diff.png"           // Optional
}
```

**Output:**
```json
{
  "test_name": "e-text-023.svg",
  "pixel_diff": 8234,
  "features": [
    {
      "name": "letter_spacing",
      "category": "styling",
      "description": "letter-spacing attribute"
    }
  ],
  "category": "not_implemented",
  "severity": "major",
  "suggested_skip": "{\"e-text-023.svg\", Params::Skip()},  // Not impl: `letter-spacing`",
  "analysis": {
    "feature_count": 1,
    "primary_feature": "letter-spacing attribute",
    "recommendation": "Skip - feature not implemented"
  }
}
```

**Implementation:** `server.py:531-610`
**Feature Detection:** `server.py:157-254` (`detect_svg_features`)
**Categorization Logic:** `server.py:287-315` (`categorize_failure`)

#### 2. `batch_triage_tests`

Process multiple test failures from bazel output, grouping by feature.

**Purpose:** Automate triage of 50+ test failures at once.

**Input:**
```json
{
  "test_output": "... complete bazel test output ..."
}
```

**Output:**
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

**Implementation:** `server.py:612-688`

#### 3. `detect_svg_features`

Parse SVG content to identify advanced features being tested.

**Purpose:** Understand what a test validates without running it.

**Input:**
```json
{
  "svg_content": "<svg><text letter-spacing='10'>...</text></svg>"
}
```

**Output:**
```json
{
  "features": [
    {
      "name": "letter_spacing",
      "category": "styling",
      "description": "letter-spacing attribute"
    }
  ]
}
```

**Detected Features:**
- **Positioning:** multiple x/y values, dx/dy attributes, rotate, textLength
- **Elements:** `<tspan>`, `<textPath>`
- **Styling:** text-anchor, letter-spacing, word-spacing, text-decoration, font-*
- **Layout:** writing-mode, baseline-shift, alignment-baseline, dominant-baseline

**Implementation:** `server.py:511-529`

#### 4. `suggest_skip_comment`

Generate properly formatted skip comments for `resvg_test_suite.cc`.

**Purpose:** Maintain consistent skip comment format across the codebase.

**Input:**
```json
{
  "test_name": "e-text-023.svg",
  "features": ["letter_spacing"],
  "category": "not_implemented"
}
```

**Output:**
```json
{
  "skip_comment": "{\"e-text-023.svg\", Params::Skip()},  // Not impl: `letter-spacing`",
  "test_name": "e-text-023.svg",
  "category": "not_implemented"
}
```

**Comment Conventions:**
- `Not impl: <feature>` - Feature not implemented
- `Not impl: <element>` - SVG element not supported
- `UB: <reason>` - Undefined behavior
- `Bug: <description>` - Known bug
- `Larger threshold due to <reason>` - Anti-aliasing artifacts

**Implementation:** `server.py:690-712` + `server.py:318-363` (format logic)

### Implementation Guidance Tools

#### 5. `suggest_implementation_approach`

Suggests which files to modify and provides implementation hints.

**Purpose:** Accelerate feature implementation by pointing to relevant code.

**Input:**
```json
{
  "test_name": "e-text-031.svg",
  "features": ["writing_mode"],
  "category": "text_layout",
  "codebase_files": []  // Optional: pre-filtered files from glob/grep
}
```

**Output:**
```json
{
  "test_name": "e-text-031.svg",
  "category": "text_layout",
  "primary_feature": "writing_mode",
  "likely_files": [
    "donner/svg/components/text/ComputedTextStyleComponent.h",
    "donner/svg/components/text/TextFlowComponent.h"
  ],
  "file_patterns": [
    "**/SVGTextElement.h",
    "**/text/*.cc",
    "**/ComputedTextStyleComponent.h"
  ],
  "search_keywords": [
    "writing-mode",
    "WritingMode",
    "writingmode"
  ],
  "similar_features": [
    {
      "name": "text-direction",
      "files": ["donner/svg/components/text/..."]
    }
  ],
  "implementation_hints": [
    "Layout properties often affect how text is measured and positioned.",
    "May require changes to text rendering pipeline.",
    "Check ComputedTextStyleComponent for text layout state."
  ]
}
```

**File Ranking Algorithm:**
- Category match: +0.3 confidence
- Exact feature name match: +0.5 confidence
- Component files: +0.2 confidence
- Header files: +0.1 confidence
- Test files: -0.3 confidence penalty

**Implementation:** `server.py:714-764` + `codebase_helpers.py`

#### 6. `find_related_tests`

Find all tests failing for the same feature.

**Purpose:** Identify batch implementation opportunities (fix one feature, pass multiple tests).

**Input:**
```json
{
  "feature": "writing-mode",
  "skip_file_content": "... content of resvg_test_suite.cc ..."
}
```

**Output:**
```json
{
  "feature": "writing-mode",
  "related_tests": ["e-text-031.svg", "e-text-033.svg"],
  "impact": "2 tests affected",
  "priority": "medium",
  "grouped_by_category": {
    "e-text": ["e-text-031.svg", "e-text-033.svg"]
  },
  "all_missing_features": {
    "writing-mode": 2,
    "dx attribute": 5,
    "textPath": 3
  }
}
```

**Priority Calculation:**
- 0 tests: none
- 1 test: low
- 2-5 tests: medium
- 6+ tests: high

**Implementation:** `server.py:766-823` + `test_output_parser.py:identify_missing_features()`

#### 7. `generate_feature_report`

Generate progress reports by test category.

**Purpose:** Track feature implementation progress and identify next priorities.

**Input:**
```json
{
  "category": "e-text",
  "test_output": "... bazel test output ...",
  "skip_file_content": "... resvg_test_suite.cc ..."  // Optional
}
```

**Output:**
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
    "letter-spacing",
    "..."
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

**Category Prefixes:**
- `e-text` - Text element tests
- `a-transform` - Transform attribute tests
- `e-path` - Path element tests
- etc.

**Implementation:** `server.py:825-888` + `test_output_parser.py:group_by_category()`

#### 8. `analyze_visual_diff`

Programmatically analyze diff images to categorize failure types.

**Purpose:** Understand visual failures without manual image inspection.

**Input:**
```json
{
  "diff_image_path": "/tmp/diff.png",
  "actual_image_path": "/tmp/actual.png",
  "expected_image_path": "/tmp/expected.png"
}
```

**Output:**
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
- `anti_aliasing` - <100 pixels, minor rendering artifacts
- `positioning` - Uniform offset detected, element placement issue
- `missing_element` - >30% of image, large missing or incorrect element
- `styling` - Color, stroke, or fill differences

**Image Analysis Algorithm:**
1. Load images with PIL/Pillow
2. Convert to NumPy arrays
3. Create diff mask (non-zero pixels)
4. Calculate bounding boxes of diff regions
5. Detect uniform offsets (concentrated in single region)
6. Categorize based on pixel count and distribution

**Implementation:** `server.py:890-977`

## Usage Examples

### Example 1: Triage a Single Failing Test

```python
# 1. Analyze the failure
result = await mcp.call_tool("analyze_test_failure", {
    "test_name": "e-text-031.svg",
    "svg_content": svg_content,
    "pixel_diff": 8234
})
# Features detected: [writing_mode], category: not_implemented

# 2. Find related tests (batch opportunity!)
related = await mcp.call_tool("find_related_tests", {
    "feature": "writing-mode",
    "skip_file_content": skip_file_content
})
# Found 2 tests: e-text-031, e-text-033

# 3. Get implementation guidance
guidance = await mcp.call_tool("suggest_implementation_approach", {
    "test_name": "e-text-031.svg",
    "features": ["writing_mode"],
    "category": "text_layout"
})
# Suggests: ComputedTextStyleComponent.h, TextFlowComponent.h
```

### Example 2: Batch Triage After Test Run

```python
# Run tests
test_output = subprocess.run(
    ["bazel", "test", "//donner/svg/renderer/tests:resvg_test_suite"],
    capture_output=True
).stdout

# Batch analyze
result = await mcp.call_tool("batch_triage_tests", {
    "test_output": test_output.decode()
})

# Result: 32 failures, grouped by 8 features
# Auto-generated skip comments for all
```

### Example 3: Track Progress

```python
# Generate progress report
report = await mcp.call_tool("generate_feature_report", {
    "category": "e-text",
    "test_output": test_output,
    "skip_file_content": skip_file_content
})

# Output:
# - 15 total tests
# - 3 passing (20%)
# - 11 missing features
# - Next priority: dx attribute (3 tests)
```

## Testing

**Unit Tests:**
- `test_codebase_helpers.py` - 98 lines, 6 test cases
- `test_test_output_parser.py` - 150 lines, 11 test cases

**Run Tests:**
```bash
cd tools/mcp-servers/resvg-test-triage
pip install -e ".[dev]"
pytest -v
```

**Validation:**
- Tested against real test failures (e-text-031: writing-mode)
- File ranking validated with actual codebase files
- Skip file parsing tested with resvg_test_suite.cc

## Installation & Setup

**Install:**
```bash
cd tools/mcp-servers/resvg-test-triage
pip install -e .
```

**Configure for Claude Code:**
```json
{
  "mcpServers": {
    "resvg-test-triage": {
      "command": "python3",
      "args": ["/path/to/donner/tools/mcp-servers/resvg-test-triage/server.py"]
    }
  }
}
```

**Configure for VSCode:**
See `.vscode/mcp.json` or `mcp-config-example.json` for format.

## Performance Characteristics

- **analyze_test_failure:** <100ms for typical SVG (500 lines)
- **batch_triage_tests:** ~2s for 50 test failures
- **detect_svg_features:** <50ms per SVG
- **suggest_implementation_approach:** <10ms (no file I/O if files provided)
- **find_related_tests:** <100ms for resvg_test_suite.cc (500 skip entries)
- **generate_feature_report:** <500ms for full test output (1000 tests)
- **analyze_visual_diff:** ~200ms per image trio (PNG, 500x500)

## Security Considerations

- All file paths provided by user (no arbitrary file access)
- Image processing limited to PNG files in temp directories
- No network calls or external data collection
- All analysis performed locally
- Image size limited to 10MB to prevent DoS
- Regex patterns sanitized in test output parsing
- Safe XML parsing (defusedxml not needed - no external input)

## Future Enhancements

**Potential Additions:**
1. ML/CV models for better visual diff categorization (70% → 90%+ accuracy)
2. Cache analysis results for performance
3. Integration with CI/CD for automated reporting
4. Support for other test suites beyond resvg
5. Real-time progress dashboard

**Design Decisions:**
- ✅ Simple image processing (no ML) - Fast, deterministic, good enough
- ✅ No caching - Keeps implementation simple, performance acceptable
- ✅ Local-only analysis - Privacy-preserving, no dependencies

## See Also

- [MCP Server README](../../../tools/mcp-servers/resvg-test-triage/README.md) - User documentation
- [AGENTS.md](../../../AGENTS.md) - Workflow examples for AI agents
- [README_resvg_test_suite.md](../../../donner/svg/renderer/tests/README_resvg_test_suite.md) - Test triage workflow
