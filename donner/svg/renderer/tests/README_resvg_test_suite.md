# Resvg Test Suite Instructions {#ReSvgTestSuite}

`//donner/svg/renderer/tests:resvg_test_suite` uses https://github.com/RazrFalcon/resvg-test-suite
to validate Donner's rendering end-to-end. The test suite provides `.svg` files that can be
rendered with the static subset of SVG (and some SVG2), and resvg's golden images to compare
against.

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

To run as part of gtest, a parameter-driven test is automatically created by grabbing scanning the
directory, wildcard-matching the filenames, and then generating a unique test for each filename
being tested.

Some tests require more lenient matching, or must be skipped entirely due to incomplete Donner
functionality. To do this per-test params may be specified. Combined together, a test
registration appears as:

```cpp
INSTANTIATE_TEST_SUITE_P(
    Transform, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-transform",
        {
            {"a-transform-007.svg",
            Params::WithThreshold(0.05f)},  // Larger threshold due to anti-aliasing artifacts.
        })),
    TestNameFromFilename);
```

The Resvg test suite files all start with a letter, either "a-" for attribute, or "e-" for element,
followed by a dash-delimited name, and then a zero-prefixed number. So "a-transform" will match
all tests like "a-transform-007.svg" or "a-transform-origin-001.svg" (hypothetically).

The test name is generated based on the test suite name, "Transform" above and the sanitized
filename. For the above example, an example test would be named:

`Transform/ImageComparisonTestFixture.ResvgTest/a_transform_001`

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

See [resvg-test-triage README](tools/mcp-servers/resvg-test-triage/README.md) for full documentation.
