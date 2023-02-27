#include <gmock/gmock.h>

#include "src/svg/renderer/tests/image_comparison_test_fixture.h"

using testing::ValuesIn;

namespace donner::svg {

using Params = ImageComparisonParams;

namespace {

static const std::filesystem::path kSvgDir = "external/resvg-test-suite/svg/";
static const std::filesystem::path kGoldenDir = "external/resvg-test-suite/png/";

std::vector<ImageComparisonTestcase> getTestsWithPrefix(
    const char* prefix, std::map<std::string, ImageComparisonParams> overrides = {}) {
  // Copy into a vector and sort the tests.
  std::vector<ImageComparisonTestcase> testPlan;
  for (const auto& entry : std::filesystem::directory_iterator(kSvgDir)) {
    const std::string& filename = entry.path().filename().string();
    if (filename.find(prefix) == 0) {
      ImageComparisonTestcase test;
      test.svgFilename = entry.path();

      // Set special-case params.
      if (auto it = overrides.find(filename); it != overrides.end()) {
        test.params = it->second;
      }

      testPlan.emplace_back(std::move(test));
    }
  }

  std::sort(testPlan.begin(), testPlan.end());
  return testPlan;
}

}  // namespace

TEST_P(ImageComparisonTestFixture, ResvgTest) {
  const ImageComparisonTestcase& testcase = GetParam();

  const std::filesystem::path goldenFilename =
      kGoldenDir / testcase.svgFilename.filename().replace_extension(".png");

  SVGDocument document = loadSVG(testcase.svgFilename.string().c_str());
  renderAndCompare(document, testcase.svgFilename, goldenFilename.string().c_str());
}

// TODO(text): a-alignment-baseline
// TODO(text): a-baseline-shift
// TODO: a-clip

INSTANTIATE_TEST_SUITE_P(Color, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("a-color",  //
                                                     {
                                                         {"a-color-interpolation-filters-001.svg",
                                                          Params::Skip()},  // Not impl: Filters
                                                     })),
                         TestNameFromFilename);

// TODO: a-direction

INSTANTIATE_TEST_SUITE_P(
    Display, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("a-display",  //
                                {
                                    {"a-display-004.svg", Params::Skip()},  // Not impl: <clipPath>
                                    {"a-display-005.svg", Params::Skip()},  // Not impl: <tspan>
                                    {"a-display-006.svg", Params::Skip()},  // Not impl: <tref>
                                    {"a-display-008.svg", Params::Skip()},  // Not impl: <clipPath>
                                    {"a-display-009.svg", Params::Skip()},  // Not impl: <tspan>
                                })),
    TestNameFromFilename);

// TODO: a-dominant-baseline
// TODO: a-enable-background

INSTANTIATE_TEST_SUITE_P(
    Fill, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-fill",  //
        {
            {"a-fill-010.svg", Params::Skip()},          // UB: rgb(int int int)
            {"a-fill-015.svg", Params::Skip()},          // UB: ICC color
            {"a-fill-027.svg", Params::Skip()},          // Not impl: Fallback with icc-color
            {"a-fill-031.svg", Params::Skip()},          // Not impl: <text>
            {"a-fill-032.svg", Params::Skip()},          // Not impl: <text>
            {"a-fill-033.svg", Params::Skip()},          // Not impl: <pattern>, <text>
            {"a-fill-opacity-004.svg", Params::Skip()},  // Not impl: `fill-opacity` affects pattern
            {"a-fill-opacity-006.svg", Params::Skip()},  // Not impl: <text>
        })),
    TestNameFromFilename);

// TODO(filter): a-filter
// TODO(filter): a-flood
// TODO(font): a-font
// TODO(font): a-glyph-orientation
// TODO(filter?): a-isolation
// TODO(text): a-kerning
// TODO(text): a-lengthAdjust
// TODO(text): a-letter-spacing
// TODO(marker): a-marker
// TODO(filter): a-mark
// TODO(filter): a-mix-blend-mode

INSTANTIATE_TEST_SUITE_P(
    Opacity, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-opacity",
        {
            {"a-opacity-002.svg", Params::Skip()},  // Not impl: <clipPath>
            {"a-opacity-005.svg",
             Params::Skip()},  // Changed in css-color-4 to allow percentage in <alpha-value>, see
                               // https://www.w3.org/TR/css-color/#transparency
        })),
    TestNameFromFilename);

// TODO(text): a-overflow

INSTANTIATE_TEST_SUITE_P(Shape, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("a-shape",
                                                     {
                                                         {"a-shape-rendering-005.svg",
                                                          Params::Skip()},  // Not impl: <text>
                                                         {"a-shape-rendering-008.svg",
                                                          Params::Skip()},  // Not impl: <marker>
                                                     })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StopAttributes, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("a-stop")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Stroke, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-stroke",
        {
            {"a-stroke-007.svg", Params::Skip()},            // Not impl: <text>
            {"a-stroke-008.svg", Params::Skip()},            // Not impl: <text>
            {"a-stroke-009.svg", Params::Skip()},            // Not impl: <pattern>, <text>
            {"a-stroke-012.svg", Params::Skip()},            // Not impl: <pattern>
            {"a-stroke-013.svg", Params::Skip()},            // Not impl: <pattern>, "gradientUnits"
            {"a-stroke-dasharray-005.svg", Params::Skip()},  // Not impl: "font-size"? "em" units
                                                             // (font-size="20" not impl)
            {"a-stroke-dasharray-007.svg", Params::Skip()},  // UB (negative values)
            {"a-stroke-dasharray-009.svg", Params::Skip()},  // UB (negative sum)
            {"a-stroke-dasharray-013.svg",
             Params::WithThreshold(0.13f)},  // Larger threshold due to anti-aliasing artifacts.
            {"a-stroke-dashoffset-004.svg", Params::Skip()},  // Not impl: dashoffset "em" units
            {"a-stroke-linejoin-004.svg",
             Params::Skip()},  // UB (SVG 2), no UA supports `miter-clip`
            {"a-stroke-linejoin-005.svg", Params::Skip()},  // UB (SVG 2), no UA supports `arcs`
            {"a-stroke-opacity-004.svg", Params::Skip()},   // Not impl: <pattern>
            {"a-stroke-opacity-006.svg", Params::Skip()},   // Not impl: <text>
            {"a-stroke-width-004.svg", Params::Skip()},     // UB: Nothing should be rendered
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Style, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-style",
        {
            {"a-style-003.svg",
             Params::Skip()},  // <svg version="1.1"> disables geometry attributes in style
        })),
    TestNameFromFilename);

// TODO: a-systemLanguage
// TODO(text): a-text

INSTANTIATE_TEST_SUITE_P(
    Transform, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-transform",
        {
            {"a-transform-007.svg",
             Params::WithThreshold(0.05f)},  // Larger threshold due to anti-aliasing artifacts.
        })),
    TestNameFromFilename);

// TODO(text): a-unicode

INSTANTIATE_TEST_SUITE_P(Visibility, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix(
                             "a-visibility",  //
                             {
                                 {"a-visibility-003.svg", Params::Skip()},  // Not impl: <tspan>
                                 {"a-visibility-004.svg", Params::Skip()},  // Not impl: <tspan>
                                 {"a-visibility-005.svg", Params::Skip()},  // Not impl: <clipPath>
                                 {"a-visibility-006.svg", Params::Skip()},  // Not impl: <clipPath>
                                 {"a-visibility-007.svg", Params::Skip()},  // Not impl: <clipPath>
                             })),
                         TestNameFromFilename);

// TODO(text): a-word-spacing
// TODO(text): a-writing-mode

// TODO: e-a-

INSTANTIATE_TEST_SUITE_P(Circle, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-circle")), TestNameFromFilename);

// TODO(clip): e-clipPath

INSTANTIATE_TEST_SUITE_P(Defs, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-defs",
                                                     {
                                                         {"e-defs-007.svg",
                                                          Params::Skip()},  // Not impl: <text>
                                                     })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Ellipse, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-ellipse")), TestNameFromFilename);

// TODO(filter): e-fe
// TODO(filter): e-filter

INSTANTIATE_TEST_SUITE_P(G, ImageComparisonTestFixture, ValuesIn(getTestsWithPrefix("e-g")),
                         TestNameFromFilename);

// TODO: e-image

INSTANTIATE_TEST_SUITE_P(
    Line, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-line-",
        {
            {"e-line-001.svg",
             Params::WithThreshold(0.02f)},  // Larger threshold due to anti-aliasing artifacts with
                                             // overlapping lines.
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    LinearGradient, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-linearGradient",
                                {
                                    {"e-linearGradient-037.svg",
                                     Params::Skip()},  // UB: Invalid `gradientTransform`
                                })),
    TestNameFromFilename);

// TODO: e-marker
// TODO: e-mask

INSTANTIATE_TEST_SUITE_P(Path, ImageComparisonTestFixture, ValuesIn(getTestsWithPrefix("e-path")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Pattern, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-pattern",
        {
            {"e-pattern-003.svg", Params::Skip()},  // UB: overflow=visible
            {"e-pattern-007.svg",
             Params::Skip()},  // Not impl: patternContentUnits=objectBoundingBox
            {"e-pattern-008.svg",
             Params::Skip()},  // Not impl: patternContentUnits=objectBoundingBox
            {"e-pattern-009.svg", Params::Skip()},  // Not impl: viewBox
            {"e-pattern-010.svg", Params::Skip()},  // Not impl: viewBox
            {"e-pattern-011.svg", Params::Skip()},  // Not impl: preserveAspectRatio
            {"e-pattern-014.svg", Params::Skip()},  // Not impl: Full href attributes
            {"e-pattern-016.svg", Params::Skip()},  // Not impl: Full href attributes
            {"e-pattern-018.svg", Params::Skip()},  // Not impl: <text>
            {"e-pattern-019.svg",
             Params::Skip()},  // Not impl: patternContentUnits, objectBoundingBox
            {"e-pattern-020.svg", Params::Skip()},  // Not impl: objectBoundingBox
            {"e-pattern-021.svg", Params::Skip()},  // Bug? Recursive on child
            {"e-pattern-022.svg", Params::Skip()},  // Bug? Self-recursive
            {"e-pattern-023.svg", Params::Skip()},  // Bug? Self-recursive on child
            {"e-pattern-024.svg", Params::Skip()},  // Not impl: objectBoundingBox
            {"e-pattern-025.svg", Params::Skip()},  // Not impl: objectBoundingBox
            {"e-pattern-026.svg", Params::Skip()},  // Not impl: userSpaceOnUse
            {"e-pattern-027.svg", Params::Skip()},  // Bug? Invalid patternUnits
            {"e-pattern-028.svg", Params::Skip()},  // UB: Invalid patternTransform
            {"e-pattern-029.svg", Params::Skip()},  // Not impl: viewBox
            {"e-pattern-030.svg", Params::Skip()},  // Not impl: userSpaceOnUse
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Polygon, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-polygon")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Polyline, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-polyline")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    RadialGradient, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-radialGradient",
        {
            {"e-radialGradient-031.svg",
             Params::Skip()},  // Test suite bug? In SVG2 this was changed to draw conical
                               // gradient instead of correcting focal point.
            {"e-radialGradient-032.svg", Params::Skip()},  // UB: Negative `r`
            {"e-radialGradient-039.svg", Params::Skip()},  // UB: Invalid `gradientUnits`
            {"e-radialGradient-040.svg", Params::Skip()},  // UB: Invalid `gradientTransform`
            {"e-radialGradient-043.svg", Params::Skip()},  // UB: fr=0.5 (SVG 2)
            {"e-radialGradient-044.svg",
             Params::Skip()},  // Test suite bug? fr > default value of r (0.5) should not
                               //  render.
            {"e-radialGradient-045.svg", Params::Skip()},  // UB: fr=-1 (SVG 2)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Rect, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-rect",
                                {
                                    {"e-rect-022.svg", Params::Skip()},  // Not impl: "em" units
                                    {"e-rect-023.svg", Params::Skip()},  // Not impl: "ex" units
                                    {"e-rect-029.svg", Params::Skip()},  // Not impl: "rem" units
                                    {"e-rect-031.svg", Params::Skip()},  // Not impl: "ch" units
                                    {"e-rect-034.svg", Params::Skip()},  // Bug? vw/vh
                                    {"e-rect-036.svg", Params::Skip()},  // Bug? vmin/vmax
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StopElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-stop",
                                {
                                    {"e-stop-011.svg",
                                     Params::Skip()},  // Bug? Strange edge case, stop-color
                                                       // inherited from <linearGradient>.
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StyleElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-style",
        {
            {"e-style-004.svg", Params::Skip()},  // Not impl: Attribute matchers
            {"e-style-012.svg", Params::Skip()},  // Not impl: <svg version="1.1">
            {"e-style-014.svg", Params::Skip()},  // Not impl: CSS @import
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    SvgElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-svg",
        {
            {"e-svg-002.svg", Params::Skip()},                // Bug? xmlns validation
            {"e-svg-003.svg", Params::Skip()},                // Bug? mixed namespaces
            {"e-svg-004.svg", Params::Skip()},                // Bug/Not impl? XML Entity references
            {"e-svg-005.svg", Params::Skip()},                // Bug/Not impl? XML Entity references
            {"e-svg-007.svg", Params::Skip()},                // Bug/Not impl? Non-UTF8 encoding
            {"e-svg-008.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"e-svg-009.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"e-svg-010.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"e-svg-011.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"e-svg-012.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"e-svg-013.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"e-svg-014.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"e-svg-015.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"e-svg-016.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"e-svg-017.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"e-svg-018.svg", Params::Skip()},                // UB: Invalid id attribute
            {"e-svg-019.svg", Params::Skip()},                // UB: Invalid id attribute
            {"e-svg-020.svg", Params::Skip()},                // UB: FuncIRI parsing
            {"e-svg-021.svg", Params::Skip()},                // UB: FuncIRI with invalid chars
            {"e-svg-028.svg", Params::Skip()},                // Not impl: overflow
            {"e-svg-029.svg", Params::Skip()},                // Not impl: overflow
            {"e-svg-031.svg", Params::Skip()},                // Bug/Not impl? XML Entity references
            {"e-svg-032.svg", Params::Skip()},                // Bug/Not impl? XML Entity references
            {"e-svg-033.svg", Params::Skip()},                // Bug? Rect inside unknown element
            {"e-svg-036.svg", Params::Skip()},  // Not impl: Computed bounds from content

        })),
    TestNameFromFilename);

// TODO: e-switch
// TODO: e-symbol
// TODO(text): e-text-
// TODO(text): e-textPath
// TODO(text): e-tspan

INSTANTIATE_TEST_SUITE_P(
    Use, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-use",
                                {
                                    {"e-use-008.svg", Params::Skip()},  // Not impl: External file.
                                })),
    TestNameFromFilename);

}  // namespace donner::svg
