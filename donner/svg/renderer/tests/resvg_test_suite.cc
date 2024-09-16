#include <gmock/gmock.h>

#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

using testing::ValuesIn;

namespace donner::svg {

using Params = ImageComparisonParams;

namespace {

static const std::filesystem::path kResourceSandboxDir = "external/resvg-test-suite/";
static const std::filesystem::path kSvgDir = "external/resvg-test-suite/svg/";
static const std::filesystem::path kGoldenDir = "external/resvg-test-suite/png/";

std::vector<ImageComparisonTestcase> getTestsWithPrefix(
    const char* prefix, std::map<std::string, ImageComparisonParams> overrides = {},
    ImageComparisonParams defaultParams = {}) {
  // Copy into a vector and sort the tests.
  std::vector<ImageComparisonTestcase> testPlan;
  for (const auto& entry : std::filesystem::directory_iterator(kSvgDir)) {
    const std::string& filename = entry.path().filename().string();
    if (filename.find(prefix) == 0) {
      ImageComparisonTestcase test;
      test.svgFilename = entry.path();
      test.params = defaultParams;

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

  SVGDocument document = loadSVG(testcase.svgFilename.string().c_str(), kResourceSandboxDir);
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
                                    {"a-display-005.svg", Params::Skip()},  // Not impl: <tspan>
                                    {"a-display-006.svg", Params::Skip()},  // Not impl: <tref>
                                    {"a-display-008.svg",
                                     Params::Skip()},  // Not impl: <clipPath> and `display: none`
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
            {"a-opacity-002.svg", Params::Skip()},  // Not impl: <clipPath> and `display: none`
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
            {"a-stroke-009.svg", Params::Skip()},            // Not impl: <text>
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
            {"a-stroke-opacity-004.svg",
             Params::Skip()},  // Not impl: <pattern> / stroke interaction
            {"a-stroke-opacity-006.svg", Params::Skip()},  // Not impl: <text>
            {"a-stroke-width-004.svg", Params::Skip()},    // UB: Nothing should be rendered
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

INSTANTIATE_TEST_SUITE_P(
    Visibility, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-visibility",  //
        {
            {"a-visibility-003.svg", Params::Skip()},  // Not impl: <tspan>
            {"a-visibility-004.svg", Params::Skip()},  // Not impl: <tspan>
            {"a-visibility-005.svg", Params::Skip()},  // Not impl: <clipPath> interaction
            {"a-visibility-006.svg", Params::Skip()},  // Not impl: <clipPath> interaction
            {"a-visibility-007.svg", Params::Skip()},  // Not impl: <clipPath> interaction
        })),
    TestNameFromFilename);

// TODO(text): a-word-spacing
// TODO(text): a-writing-mode

// TODO: e-a-

INSTANTIATE_TEST_SUITE_P(Circle, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-circle")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    ClipPath, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-clipPath",
        {
            {"e-clipPath-007.svg", Params::Skip()},  // Not impl: <text>
            {"e-clipPath-009.svg", Params::Skip()},  // Not impl: <text>
            {"e-clipPath-010.svg", Params::Skip()},  // Not impl: <text>
            {"e-clipPath-011.svg", Params::Skip()},  // Not impl: <text>
            {"e-clipPath-012.svg", Params::Skip()},  // Not impl: <text>, clip-rule
            {"e-clipPath-016.svg", Params::Skip()},  // Bug: g is invalid child
            {"e-clipPath-019.svg", Params::Skip()},  // Not impl: clip-path on child
            {"e-clipPath-020.svg", Params::Skip()},  // Not impl: clip-path on self, clip-rule
            {"e-clipPath-024.svg", Params::Skip()},  // Not impl: invisible child
            {"e-clipPath-025.svg", Params::Skip()},  // Not impl: invisible child
            {"e-clipPath-029.svg", Params::Skip()},  // Not impl: recursive child
            {"e-clipPath-031.svg",
             Params::Skip()},  // Not impl: `clip-path` on child with transform
            {"e-clipPath-032.svg", Params::Skip()},  // Invalid `clip-path` on self
            {"e-clipPath-033.svg", Params::Skip()},  // Invalid `clip-path` on child
            {"e-clipPath-034.svg", Params::Skip()},  // Not impl: `clip-path` on children
            {"e-clipPath-036.svg", Params::Skip()},  // Not impl: `clip-path` on self
            {"e-clipPath-037.svg", Params::Skip()},  // Not impl: Recursive with self
            {"e-clipPath-038.svg", Params::Skip()},  // Not impl: marker
            {"e-clipPath-039.svg", Params::Skip()},  // Not impl: `mask` has no effect
            {"e-clipPath-042.svg", Params::Skip()},  // UB: on root `<svg>` without size
            {"e-clipPath-044.svg", Params::Skip()},  // Not impl: <use> child
            {"e-clipPath-046.svg", Params::Skip()},  // Not impl: <switch>
            {"e-clipPath-047.svg", Params::Skip()},  // Not impl: <symbol>
        })),
    TestNameFromFilename);

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

INSTANTIATE_TEST_SUITE_P(
    Image, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-image",
                                {
                                    {"e-image-003.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-006.svg", Params::Skip()},  // Not impl: .svgz image
                                    {"e-image-007.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-008.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-017.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-018.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-019.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-020.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-021.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-022.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-023.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-024.svg", Params::Skip()},  // Not impl: .svg imageg
                                    {"e-image-029.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-030.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-031.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-032.svg", Params::Skip()},  // UB: Float size
                                    {"e-image-033.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-034.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-039.svg", Params::Skip()},  // Not impl: .svg image
                                    {"e-image-040.svg", Params::Skip()},  // Not impl: External URLs
                                    {"e-image-041.svg", Params::Skip()},  // Not impl: External URLs
                                },
                                Params::WithThreshold(0.2f).disableDebugSkpOnFailure())),
    TestNameFromFilename);

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

INSTANTIATE_TEST_SUITE_P(
    Mask, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-mask",
        {
            {"e-mask-017.svg", Params::Skip()},  // Not impl: color-interpolation
            {"e-mask-022.svg", Params::Skip()},  // UB: Recursive on child
            {"e-mask-023.svg", Params::Skip()},  // BUG: Self-recursive
            {"e-mask-024.svg", Params::Skip()},  // BUG: Recursive
            {"e-mask-025.svg", Params::Skip()},  // BUG: Self-recursive
            {"e-mask-026.svg", Params::Skip()},  // BUG: Mask on self
            {"e-mask-027.svg", Params::Skip()},  // BUG: Mask on child
            {"e-mask-029.svg", Params::Skip()},  // BUG: Crashes on serializing the skp
            {"e-mask-030.svg", Params::Skip()},  // BUG: Crashes on serializing the skp
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Path, ImageComparisonTestFixture, ValuesIn(getTestsWithPrefix("e-path")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Pattern, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-pattern",
        {
            {"e-pattern-003.svg", Params::Skip()},  // UB: overflow=visible
            {"e-pattern-008.svg",
             Params::WithThreshold(kDefaultThreshold, 250)},  // Anti-aliasing artifacts
            {"e-pattern-010.svg",
             Params::WithThreshold(kDefaultThreshold, 150)},          // Anti-aliasing artifacts
            {"e-pattern-018.svg", Params::Skip()},                    // Not impl: <text>
            {"e-pattern-019.svg", Params::WithThreshold(0.2f)},       // Anti-aliasing artifacts
            {"e-pattern-020.svg", Params::WithThreshold(0.6f, 300)},  // Anti-aliasing artifacts
            {"e-pattern-021.svg", Params::WithThreshold(0.2f)},       // Anti-aliasing artifacts
            {"e-pattern-022.svg", Params::WithThreshold(0.2f)},       // Anti-aliasing artifacts
            {"e-pattern-023.svg", Params::WithThreshold(0.2f)},       // Anti-aliasing artifacts
            {"e-pattern-028.svg", Params::Skip()},                // UB: Invalid patternTransform
            {"e-pattern-030.svg", Params::WithThreshold(0.02f)},  // Has anti-aliasing artifacts.
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
    ValuesIn(getTestsWithPrefix("e-style",
                                {
                                    {"e-style-012.svg",
                                     Params::Skip()},  // Not impl: <svg version="1.1">
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
