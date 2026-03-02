# Resvg Test Suite Instructions {#ReSvgTestSuite}

`//donner/svg/renderer/tests:resvg_test_suite` uses https://github.com/RazrFalcon/resvg-test-suite
to validate Donner's rendering end-to-end. The test suite now ships its SVG and PNG files inside a
`tests/` directory (with assets under `resources/` and fonts under `fonts/`), and the Bazel target
bundles those directories so they can be used as runfiles.

To validate against this suite continuously, https://github.com/jwmcglynn/pixelmatch-cpp17 is used
to perceptually difference the images and wrap it in a gtest. Execution is single-threaded but it's
fast enough to be run in CI, and sufficiently fast to be run as part of inner loop development.

To run the suite:

```
bazel run //donner/svg/renderer/tests:resvg_test_suite
```

Or in debug mode:

```
bazel run -c dbg //donner/svg/renderer/tests:resvg_test_suite
```

Since this is a gtest, it will also be run as part of any bazel test targeting this directory:

```
bazel test //...
```

The parameter-driven test scans a curated list of directories under `tests/` and instantiates a
test for every `.svg` file it finds. Test names are derived from the relative path inside
`tests/` and sanitized so that repeated file names in different directories remain unique. Every
test also uses a 500x500 canvas size by default to mirror the original suite expectations.

The harness also supports per-test configuration to allow more lenient matching or to skip tests
that exercise unsupported features. Each override is keyed by the relative path under `tests/`
and can set thresholds, mark tests as skipped, or point at alternate goldens. For example, the
resvg layout now contains `painting/stroke-dasharray/em-units.svg` (a case Donner does not yet
support), which is mapped to `ImageComparisonParams::Skip()` in
`resvg_test_suite.cc`. To lower the acceptable pixel count for a noisy test instead, use
`ImageComparisonParams::WithThreshold` with the desired values.

Extending the suite means updating the directory lists and overrides in
`donner/svg/renderer/tests/resvg_test_suite.cc`. A typical override block looks like:

```cpp
const TestDirectoryConfig paintServerConfig{
    .relativePaths = {
        "paint-servers/pattern",
    },
    .overrides = {
        {"paint-servers/pattern/tiny-pattern-upscaled.svg",
         ImageComparisonParams::WithThreshold(0.2f)},  // Anti-aliasing artifacts
    },
    .defaultParams = ImageComparisonParams::WithThreshold(0.02f),
};
```

With the new layout, a test name is generated from the sanitized relative path. For the above
example, the test would be named:

`Resvg/ImageComparisonTestFixture.ResvgTest/paint_servers_pattern_tiny_pattern_upscaled_svg`

To run a test with only one test:

```sh
bazel run -c dbg //donner/svg/renderer/tests:resvg_test_suite -- --gtest_filter="*a_transform_001"
```

If a test is skipped, it's still useful to manually run it without code changes. With gtest, the
quickest way to enable that is to automatically prefix the test names with `DISABLED_`. Prefixing
this has special meaning in gtest, where the test is automatically marked disabled, but it can
still be run manually from the command line.

To run a test that has been disabled, invoke it the same way:

```sh
bazel run -c dbg //donner/svg/renderer/tests:resvg_test_suite -- --gtest_filter="*a_transform_008" --gtest_also_run_disabled_tests
```

With suffix-matching, the same test identifier can be used.

## Triaging Test Failures

When tests fail, follow this systematic approach to triage and document them:

### 1. Run the Failing Tests

First, identify which tests are failing:

```sh
bazel run //donner/svg/renderer/tests:resvg_test_suite -c dbg -- '--gtest_filter=*e_text_*'
```

Look for tests marked as `FAIL` and note the pixel difference count. Tests pass if pixel differences are under the threshold (default 100 pixels).

### 2. Examine the Test Output

When a test fails, the framework provides detailed diagnostic information:

#### Test Failure Header

```
[  COMPARE ] .../svg/e-text-023.svg: FAIL (8234 pixels differ, with 100 max)
```

- **FAIL**: Test failed (vs PASS for success)
- **8234 pixels differ**: Number of pixels that don't match between actual and expected
- **with 100 max**: Threshold for passing (tests pass if pixel diff ≤ 100)

#### Verbose Rendering Output

When a test fails, it re-renders with verbose logging showing:

```
Document world from canvas transform: matrix(2.5 0 0 2.5 0 0)
Instantiating SVG id=svg1 #1
Instantiating Text id=text1 #5
Rendering Text id=text1 #5 transform=matrix(2.5 0 0 2.5 0 0)
```

This shows:
- **Canvas transform**: The scaling/transform applied to the entire SVG
- **Instantiating**: Elements being created from the SVG DOM
- **Rendering**: Elements being drawn to the canvas with their transforms

#### SVG Source Display

The complete SVG source is printed inline:

```
SVG Content for e-text-023.svg:
---
<svg id="svg1" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg"
     font-family="Noto Sans" font-size="48">
    <title>letter-spacing</title>
    <text id="text1" x="30" y="100" letter-spacing="10">Text</text>
</svg>
---
```

Look for:
- **`<title>`**: Describes what the test validates
- **Element attributes**: Features being tested (e.g., multiple x/y values)
- **Complexity**: Number of elements and their properties

#### Output File Paths

Three files are generated and their paths printed:

```
Actual rendering: /var/folders/.../e-text-023.png
Expected: /path/to/resvg-test-suite/png/e-text-023.png
Diff: /var/folders/.../diff_e-text-023.png
```

**Actual rendering** (Donner's output):
- PNG generated by the Donner renderer
- Shows what Donner currently produces
- Located in temporary directory (`/var/folders/...` on macOS, `/tmp/...` on Linux)

**Expected** (Golden reference):
- PNG from the resvg test suite
- Reference image showing correct rendering
- Located in the bazel runfiles directory
- Generated by resvg, the reference SVG renderer

**Diff** (Visual comparison):
- Highlights differences between actual and expected
- Red/orange pixels show where images differ
- Yellow/colored outlines indicate positional differences
- Helps quickly identify the nature of the failure

#### Skia Picture (.skp) File

For failed tests, a Skia Picture file is also generated:

```
Load this .skp into https://debugger.skia.org/
=> /var/folders/.../e-text-023.png.skp
```

This file can be uploaded to the Skia debugger for detailed inspection of drawing commands.

### Interpreting Output Files

**What to look for**:

- **Diff image**:
  - Solid red areas = completely different pixels
  - Colored outlines = positional/alignment differences
  - Minimal differences = may just need threshold adjustment
  - Large differences = missing feature or wrong implementation

- **Actual vs Expected**:
  - Compare side-by-side to understand the failure
  - Missing elements = not implemented
  - Wrong position = baseline/positioning issue
  - Wrong style = font/styling issue
  - Different rendering = may be font engine differences (acceptable)

### 3. Analyze the Failure

Based on the output, determine what's causing the failure:

**Check the SVG source** (printed in test output):
- Look at the `<title>` to understand test intent
- Identify which SVG features are being tested
- Note complex attributes or patterns

**Compare images**:
- Open the diff image to see where differences are
- Compare actual vs expected side-by-side
- Assess the magnitude of differences (pixel count)

**Review verbose output**:
- Check if elements are being instantiated
- Verify transforms are being applied
- Look for errors or warnings in the render log

### 4. Categorize the Failure

Common failure categories:
- **Not implemented**: Feature doesn't exist yet (e.g., `<tspan>`, `writing-mode`)
- **UB (Undefined Behavior)**: Edge case or non-standard behavior
- **Anti-aliasing artifacts**: Small rendering differences requiring higher threshold
- **Font rendering differences**: Expected when using platform fonts (CoreText vs FreeType)

### 5. Document in resvg_test_suite.cc

Add the failing test to the appropriate `INSTANTIATE_TEST_SUITE_P` block with a skip comment:

```cpp
INSTANTIATE_TEST_SUITE_P(
    Text, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-text-",
        {
            {"e-text-006.svg", Params::Skip()},  // Not impl: `dx` attribute
            {"e-text-023.svg", Params::Skip()},  // Not impl: `letter-spacing`
            {"e-text-027.svg", Params::Skip()},  // Not impl: Color emoji font (Noto Color Emoji)
            {"e-text-042.svg", Params::Skip()},  // Not impl: <textPath>
        })),
    TestNameFromFilename);
```

**Comment format**:
- `Not impl: <feature>` - Feature not yet implemented
- `UB: <reason>` - Undefined behavior or edge case
- `Bug: <description>` - Known bug
- `Larger threshold due to <reason>` - Use `Params::WithThreshold(0.05f)` for anti-aliasing

### 6. Group Related Failures

When triaging multiple tests, group them by the missing feature:

```cpp
// Text positioning features
{"e-text-006.svg", Params::Skip()},  // Not impl: `dx` attribute
{"e-text-007.svg", Params::Skip()},  // Not impl: `dx` attribute
{"e-text-010.svg", Params::Skip()},  // Not impl: `dy` attribute

// Text styling features
{"e-text-023.svg", Params::Skip()},  // Not impl: `letter-spacing`
{"e-text-024.svg", Params::Skip()},  // Not impl: `word-spacing`
{"e-text-028.svg", Params::Skip()},  // Not impl: `font-weight`
```

### 7. Verify Skip Configuration

After adding skips, verify tests run correctly:

```sh
bazel run //donner/svg/renderer/tests:resvg_test_suite -c dbg -- '--gtest_filter=*e_text_*'
```

You should see:
- Skipped tests don't run
- Passing tests still pass
- Clear count of passed/skipped tests

### Example Triage Workflow

1. Run tests
    ```
    bazel run //donner/svg/renderer/tests:resvg_test_suite -c dbg -- '--gtest_filter=*e_text_*'
    ```

2. Examine the SVG of the failing test (printed as output)

3. Open the diff image to see where differences are

4. Identify the cause of the failure. Either fix the root cause in Donner, modify the test parameters in resvg_test_suite.cc, or mark the test skipped to defer resolving the issue while keeping the suite operational.

### Tips

- **Visual inspection**: Always view the diff images to understand the nature of failures
- **Pixel count matters**: Small pixel differences (< 100) might just need threshold adjustment
- **Font differences**: CoreText (macOS) vs FreeType/HarfBuzz (Linux) produce different rendering - this is expected
- **Categorize systematically**: Group tests by missing feature for easier tracking
- **Keep comments concise**: Use the established format from existing tests


## MCP Servers

The `resvg-test-triage` MCP server provides automated test analysis. When available, use it to:

**Batch analyze test failures:**
```python
# After running tests, pass output to MCP server
result = await mcp.call_tool("batch_triage_tests", {
    "test_output": test_output_string
})

# Server returns:
# - Categorized failures by feature
# - Suggested skip comments
# - Grouping recommendations
```

**Analyze individual tests:**
```python
result = await mcp.call_tool("analyze_test_failure", {
    "test_name": "e-text-023.svg",
    "svg_content": svg_source,
    "pixel_diff": 8234
})

# Returns feature detection, category, and skip suggestion
```

**Get implementation guidance (NEW):**
```python
# Find which files to modify for a missing feature
result = await mcp.call_tool("suggest_implementation_approach", {
    "test_name": "e-text-031.svg",
    "features": ["writing_mode"],
    "category": "text_layout",
    "codebase_files": []  # Optionally provide files from glob/grep
})

# Returns:
# - Ranked list of files to modify
# - Search keywords for finding similar features
# - Implementation hints specific to the feature
```

**Find related tests for batch implementation (NEW):**
```python
# Discover all tests failing for the same feature
result = await mcp.call_tool("find_related_tests", {
    "feature": "writing-mode",
    "skip_file_content": resvg_test_suite_cc_content
})

# Returns:
# - List of all tests with this feature (e.g., e-text-031, e-text-033)
# - Impact assessment and priority (low/medium/high)
# - Batch implementation opportunity!
```

**Track feature progress (NEW):**
```python
# Generate progress report for a test category
result = await mcp.call_tool("generate_feature_report", {
    "category": "e-text",
    "test_output": bazel_test_output,
    "skip_file_content": resvg_test_suite_cc_content
})

# Returns:
# - Pass/fail/skip counts
# - Completion rate percentage
# - Next priority feature by test impact
# - List of all missing features
```

**Analyze visual differences (NEW):**
```python
# Programmatically analyze diff images
result = await mcp.call_tool("analyze_visual_diff", {
    "diff_image_path": "/tmp/diff_e-text-031.png",
    "actual_image_path": "/tmp/e-text-031.png",
    "expected_image_path": "/path/to/resvg-test-suite/png/e-text-031.png"
})

# Returns:
# - Difference type: positioning/missing_element/styling/anti_aliasing
# - Visual analysis metrics (pixel counts, regions, offsets)
# - Likely cause with confidence score
```

**Setup:**
1. Install: `pip install -e tools/mcp-servers/resvg-test-triage`
2. Configure in MCP settings:
   - **Claude Code**: See `tools/mcp-servers/resvg-test-triage/mcp-config-example.json`
   - **VSCode**: Add to `.vscode/mcp.json` (see README for format)
3. Use tools during test triage

**Benefits:**
- Consistent categorization across all tests
- Auto-detection of SVG features being tested
- Batch processing of 50+ test failures
- Properly formatted skip comments
- Vision model analysis with actual, expected, and diff images
- **Implementation guidance** - suggests files to modify
- **Batch opportunities** - find all tests for same feature
- **Progress tracking** - monitor feature completion
- **Visual analysis** - categorize diff types automatically

See [resvg-test-triage README](../../../tools/mcp-servers/resvg-test-triage/README.md) for full documentation.
