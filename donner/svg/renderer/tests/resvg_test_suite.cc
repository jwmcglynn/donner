#include <gmock/gmock.h>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

using testing::ValuesIn;

namespace donner::svg {

using Params = ImageComparisonParams;

namespace {

std::vector<ImageComparisonTestcase> getTestsWithPrefix(
    const char* prefix, std::map<std::string, ImageComparisonParams> overrides = {},
    ImageComparisonParams defaultParams = {}) {
  const std::string kSvgDir = Runfiles::instance().RlocationExternal("resvg-test-suite", "svg");

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

      // Always set the canvas size to 500x500 for these tests.
      test.params.setCanvasSize(500, 500);

      testPlan.emplace_back(std::move(test));
    }
  }

  std::sort(testPlan.begin(), testPlan.end());
  return testPlan;
}

}  // namespace

TEST_P(ImageComparisonTestFixture, ResvgTest) {
  const ImageComparisonTestcase& testcase = GetParam();
  const std::string kGoldenDir = Runfiles::instance().RlocationExternal("resvg-test-suite", "png");

  std::filesystem::path goldenFilename;
  if (testcase.params.overrideGoldenFilename.empty()) {
    goldenFilename = kGoldenDir / testcase.svgFilename.filename().replace_extension(".png");
  } else {
    goldenFilename = testcase.params.overrideGoldenFilename;
  }

  SVGDocument document = loadSVG(testcase.svgFilename.string().c_str(),
                                 Runfiles::instance().RlocationExternal("resvg-test-suite", ""));
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

INSTANTIATE_TEST_SUITE_P(MarkerAttrib, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("a-marker")), TestNameFromFilename);

// TODO(filter): a-mark
// TODO(filter): a-mix-blend-mode

INSTANTIATE_TEST_SUITE_P(
    Opacity, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-opacity",
        {
            {"a-opacity-005.svg",
             Params::Skip()},  // Changed in css-color-4 to allow percentage in <alpha-value>, see
                               // https://www.w3.org/TR/css-color/#transparency
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Overflow, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("a-overflow")), TestNameFromFilename);

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
    ValuesIn(getTestsWithPrefix("a-visibility",  //
                                {
                                    {"a-visibility-003.svg", Params::Skip()},  // Not impl: <tspan>
                                    {"a-visibility-004.svg", Params::Skip()},  // Not impl: <tspan>
                                    {"a-visibility-007.svg", Params::Skip()},  // Not impl: <text>
                                })),
    TestNameFromFilename);

// TODO(text): a-word-spacing
// TODO(text): a-writing-mode

// TODO: e-a-

INSTANTIATE_TEST_SUITE_P(
    Circle, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-circle",
                                {
                                    {"e-circle-001.svg",
                                     Params::WithThreshold(kDefaultThreshold,
                                                           150)},  // Larger threshold due to
                                                                   // rasterization artifacts.
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    ClipPath, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-clipPath",
        {
            {"e-clipPath-007.svg", Params::Skip()},  // Not impl: <text>
            {"e-clipPath-009.svg", Params::Skip()},  // Not impl: <text>
            {"e-clipPath-010.svg", Params::Skip()},  // Not impl: <text>
            {"e-clipPath-011.svg", Params::Skip()},  // Not impl: <text>
            {"e-clipPath-012.svg", Params::Skip()},  // Not impl: <text>
            {"e-clipPath-029.svg", Params()},
            {"e-clipPath-034.svg",
             Params::WithThreshold(kDefaultThreshold, 200)},  // Larger threshold due to nested
                                                              // clip-path AA artifacts.
            {"e-clipPath-037.svg", Params()},
            {"e-clipPath-042.svg", Params::Skip()},  // UB: on root `<svg>` without size
            {"e-clipPath-044.svg", Params::Skip()},  // Not impl: <use> child
            {"e-clipPath-046.svg", Params::Skip()},  // Not impl: <switch>
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Defs, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-defs",
                                                     {
                                                         {"e-defs-007.svg",
                                                          Params::Skip()},  // Not impl: <text>
                                                     })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Ellipse, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-ellipse",
                                {
                                    {"e-ellipse-001.svg",
                                     Params::WithThreshold(kDefaultThreshold,
                                                           150)},  // Larger threshold due to
                                                                   // rasterization artifacts.
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeBlend, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feBlend",
        {
            {"e-feBlend-007.svg", Params::Skip()},  // Not impl: primitiveUnits subregion
            {"e-feBlend-008.svg", Params::Skip()},  // Not impl: primitiveUnits subregion
            {"e-feBlend-009.svg", Params::Skip()},  // Not impl: color-burn blend mode (SVG2)
            {"e-feBlend-010.svg", Params::Skip()},  // Not impl: hue blend mode (SVG2)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeColorMatrix, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feColorMatrix",
        {
            // TODO(filter): All linearRGB rounding diffs are due to 8-bit intermediate
            // quantization in sRGB<->linear LUT conversion. Fix by processing filters in
            // float precision.
            {"e-feColorMatrix-001.svg", Params::Skip()},  // linearRGB rounding (597px)
            {"e-feColorMatrix-002.svg", Params::Skip()},  // linearRGB rounding (identity)
            {"e-feColorMatrix-003.svg", Params::Skip()},  // linearRGB rounding (identity)
            {"e-feColorMatrix-004.svg", Params::Skip()},  // linearRGB rounding (identity)
            {"e-feColorMatrix-005.svg", Params::Skip()},  // linearRGB rounding (identity)
            {"e-feColorMatrix-006.svg", Params::Skip()},  // linearRGB rounding (3383px)
            {"e-feColorMatrix-007.svg", Params::Skip()},  // linearRGB rounding (saturate)
            {"e-feColorMatrix-008.svg", Params::Skip()},  // saturate -0.5 (UB, 141k px)
            {"e-feColorMatrix-009.svg", Params::Skip()},  // saturate 99999 (UB, 158k px)
            {"e-feColorMatrix-010.svg", Params::Skip()},  // linearRGB rounding (identity)
            {"e-feColorMatrix-011.svg", Params::Skip()},  // linearRGB rounding (hueRotate)
            {"e-feColorMatrix-012.svg", Params::Skip()},  // hueRotate(0) rounding diff
            {"e-feColorMatrix-015.svg", Params::Skip()},  // linearRGB rounding (identity)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeComponentTransfer, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feComponentTransfer",
        {
            {"e-feComponentTransfer-009.svg",
             Params::Skip()},  // tableValues="1px" invalid unit suffix not rejected
            {"e-feComponentTransfer-020.svg",
             Params::Skip()},  // Mixed types with feFuncA gamma + opacity, linearRGB rounding
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeComposite, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feComposite",
        {
            {"e-feComposite-007.svg", Params::Skip()},   // Not impl: default subregion
            {"e-feComposite-009.svg", Params::Skip()},   // filter region (k4 nonzero)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeConvolveMatrix, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feConvolveMatrix",
        {
            {"e-feConvolveMatrix-001.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-002.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-003.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-004.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-005.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-006.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-007.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-008.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-009.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-010.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-011.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-012.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-013.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-014.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-015.svg", Params::Skip()},  // UB: bias=0.5
            {"e-feConvolveMatrix-016.svg", Params::Skip()},  // UB: bias=-0.5
            {"e-feConvolveMatrix-017.svg", Params::Skip()},  // UB: bias=9999
            {"e-feConvolveMatrix-018.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-019.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-020.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-021.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-022.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-023.svg", Params::Skip()},  // UB: wrap with oversized kernel
            {"e-feConvolveMatrix-024.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
            {"e-feConvolveMatrix-025.svg", Params::WithThreshold(kDefaultThreshold, 5000)},
        })),
    TestNameFromFilename);

// TODO(filter): e-feDiffuseLighting (not implemented)
INSTANTIATE_TEST_SUITE_P(
    FeDisplacementMap, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-feDisplacementMap", {})),
    TestNameFromFilename);
// TODO(filter): e-feDistantLight (not implemented)
INSTANTIATE_TEST_SUITE_P(
    FeDropShadow, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feDropShadow",
        {
            // linearRGB 8-bit LUT rounding diffs at blur edges
            {"e-feDropShadow-001.svg", Params::WithThreshold(kDefaultThreshold, 400)},
            {"e-feDropShadow-002.svg", Params::WithThreshold(kDefaultThreshold, 200)},
            {"e-feDropShadow-003.svg", Params::WithThreshold(kDefaultThreshold, 120)},
            {"e-feDropShadow-005.svg", Params::Skip()},  // Not impl: default filter region clipping
            {"e-feDropShadow-006.svg", Params::WithThreshold(kDefaultThreshold, 200)},
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeFlood, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feFlood",
        {
            {"e-feFlood-006.svg", Params::Skip()},  // Not impl: default subregion + negative coords
            {"e-feFlood-007.svg", Params::Skip()},  // Not impl: primitiveUnits
            {"e-feFlood-008.svg", Params::Skip()},  // Not impl: primitiveUnits
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeGaussianBlur, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feGaussianBlur",
        {
            {"e-feGaussianBlur-002.svg", Params::Skip()},  // Huge stdDev clipping
            {"e-feGaussianBlur-012.svg", Params::Skip()},  // Complex skew transform + asymmetric blur
        })),
    TestNameFromFilename);

// TODO(filter): e-feImage (not implemented)

INSTANTIATE_TEST_SUITE_P(
    FeMerge, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feMerge",
        {
            {"e-feMerge-001.svg", Params::Skip()},  // Not impl: color-interpolation-filters
            {"e-feMerge-002.svg", Params::Skip()},  // Not impl: color-interpolation-filters
            {"e-feMerge-003.svg", Params::Skip()},  // Not impl: color-interpolation-filters
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeMorphology, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feMorphology",
        {
            {"e-feMorphology-007.svg", Params::Skip()},  // Not impl: primitiveUnits=objectBoundingBox
            {"e-feMorphology-012.svg", Params::Skip()},  // Perf: radius=9999, O(n*r^2) too slow
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeOffset, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feOffset",
        {
            {"e-feOffset-007.svg", Params::Skip()},  // Not impl: primitiveUnits=objectBoundingBox
            {"e-feOffset-008.svg", Params::Skip()},  // Not impl: complex skew transform
        })),
    TestNameFromFilename);

// TODO(filter): e-fePointLight (not implemented)
// TODO(filter): e-feSpecularLighting (not implemented)
// TODO(filter): e-feSpotLight (not implemented)
INSTANTIATE_TEST_SUITE_P(
    FeTile, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feTile",
        {
            {"e-feTile-001.svg",
             Params::WithThreshold(kDefaultThreshold, 5000)},  // linearRGB rounding + tile boundary
            {"e-feTile-002.svg",
             Params::WithThreshold(kDefaultThreshold, 2500)},  // linearRGB rounding + tile boundary
            {"e-feTile-004.svg",
             Params::WithThreshold(kDefaultThreshold, 8500)},  // Tile boundary sub-pixel alignment
            {"e-feTile-005.svg",
             Params::WithThreshold(kDefaultThreshold, 10000)},  // Tile boundary sub-pixel alignment
            {"e-feTile-006.svg", Params::Skip()},  // Not impl: percentage-based primitive subregion
            {"e-feTile-007.svg", Params::Skip()},  // Complex transform (UB per test title)
        })),
    TestNameFromFilename);
INSTANTIATE_TEST_SUITE_P(
    FeTurbulence, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feTurbulence",
        {
            {"e-feTurbulence-002.svg",
             Params::WithThreshold(kDefaultThreshold, 1500)},  // Boundary rounding
            {"e-feTurbulence-004.svg",
             Params::WithThreshold(kDefaultThreshold, 1500)},  // Boundary rounding
            {"e-feTurbulence-005.svg",
             Params::WithThreshold(kDefaultThreshold, 1500)},  // Boundary rounding
            {"e-feTurbulence-008.svg", Params::Skip()},  // Not impl: primitiveUnits=objectBoundingBox
            {"e-feTurbulence-013.svg",
             Params::WithThreshold(kDefaultThreshold, 1500)},  // Boundary rounding (invalid type)
            {"e-feTurbulence-014.svg",
             Params::WithThreshold(kDefaultThreshold, 1500)},  // Boundary rounding (seed=20)
            {"e-feTurbulence-015.svg",
             Params::WithThreshold(kDefaultThreshold, 1500)},  // Boundary rounding (seed=-20)
            {"e-feTurbulence-016.svg",
             Params::WithThreshold(kDefaultThreshold, 1500)},  // Boundary rounding (seed=1.5)
            {"e-feTurbulence-017.svg", Params::Skip()},  // stitchTiles=stitch (UB per test title)
            {"e-feTurbulence-018.svg", Params::Skip()},  // Complex skew transform
            {"e-feTurbulence-019.svg",
             Params::WithThreshold(kDefaultThreshold, 25000)},  // sRGB color-interpolation
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Filter, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-filter",
        {
            {"e-filter-002.svg", Params::Skip()},  // filter region (blur edge clipping)
            {"e-filter-003.svg", Params::Skip()},  // filterUnits=userSpaceOnUse (blur edge)
            {"e-filter-004.svg", Params::Skip()},  // subregion + filter region
            {"e-filter-009.svg", Params::Skip()},  // subregion (blur edge, 615px)
            {"e-filter-010.svg", Params::Skip()},  // subregion percentage
            {"e-filter-011.svg", Params::Skip()},  // subregion
            {"e-filter-012.svg", Params::Skip()},  // primitiveUnits=objectBoundingBox
            {"e-filter-014.svg", Params::Skip()},  // negative subregion
            {"e-filter-017.svg", Params::Skip()},  // xlink:href
            {"e-filter-018.svg", Params::Skip()},  // xlink:href
            {"e-filter-019.svg", Params::Skip()},  // xlink:href
            {"e-filter-026.svg", Params::Skip()},  // transform on shape (blur edge)
            {"e-filter-027.svg", Params::Skip()},  // transform + filter region
            {"e-filter-030.svg", Params::Skip()},  // primitiveUnits=objectBoundingBox
            {"e-filter-032.svg", Params::Skip()},  // in=BackgroundImage
            {"e-filter-033.svg", Params::Skip()},  // in=BackgroundAlpha
            {"e-filter-034.svg", Params::Skip()},  // in=FillPaint
            {"e-filter-035.svg", Params::Skip()},  // in=StrokePaint
            {"e-filter-036.svg", Params::Skip()},  // in=FillPaint gradient
            {"e-filter-037.svg", Params::Skip()},  // in=FillPaint pattern
            {"e-filter-038.svg", Params::Skip()},  // in=FillPaint on group
            {"e-filter-039.svg", Params::Skip()},  // multiple primitives (blur edge clipping)
            {"e-filter-040.svg", Params::Skip()},  // multiple primitives (blur edge clipping)
            {"e-filter-041.svg", Params::Skip()},  // blur edge clipping
            {"e-filter-042.svg", Params::Skip()},  // blur edge clipping + linearRGB
            {"e-filter-044.svg", Params::Skip()},  // chained blur edge clipping
            {"e-filter-046.svg", Params::Skip()},  // color-interpolation-filters
            {"e-filter-052.svg", Params::Skip()},  // clip-path + filter (blur edge)
            {"e-filter-053.svg", Params::Skip()},  // mask + filter (blur edge)
            {"e-filter-054.svg", Params::Skip()},  // clip-path + mask + filter (blur edge)
            {"e-filter-055.svg", Params::Skip()},  // primitiveUnits=objectBoundingBox
            {"e-filter-056.svg", Params::Skip()},  // in to invalid (filter region)
            {"e-filter-059.svg", Params::Skip()},  // complex transforms (blur edge)
            {"e-filter-060.svg", Params::Skip()},  // filter on root svg
            {"e-filter-065.svg", Params::Skip()},  // in=FillPaint on empty group
        })),
    TestNameFromFilename);

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
            {"e-line-007.svg",
             Params::WithThreshold(kDefaultThreshold, 120)},  // Larger threshold due to
                                                              // rasterization artifacts.
            {"e-line-008.svg",
             Params::WithThreshold(kDefaultThreshold, 120)},  // Larger threshold due to
                                                              // rasterization artifacts.
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

INSTANTIATE_TEST_SUITE_P(
    Marker, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-marker",
        {
            {"e-marker-008.svg", Params::Skip()},  // UB: with `viewBox`
            {"e-marker-017.svg", Params::Skip()},  // Not impl: `text`
            {"e-marker-018.svg", Params::Skip()},  // Not impl: `text`
            {"e-marker-019.svg", Params::Skip()},  // Not impl: .svg image
            {"e-marker-022.svg", Params::Skip()},  // BUG: Nested
            {"e-marker-032.svg", Params::Skip()},  // UB: Target with subpaths
            {"e-marker-044.svg", Params::Skip()},  // BUG: Multiple closepaths (M L L Z Z Z)
            // Resvg bug? Direction to place markers at the beginning/end of closed shapes.
            {"e-marker-045.svg", Params::WithGoldenOverride(
                                     "donner/svg/renderer/testdata/golden/resvg-e-marker-045.png")},
            // BUG? Disagreement about marker direction on cusp
            {"e-marker-051.svg", Params::WithGoldenOverride(
                                     "donner/svg/renderer/testdata/golden/resvg-e-marker-051.png")},
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Mask, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-mask",
        {
            {"e-mask-017.svg", Params::Skip()},  // Not impl: color-interpolation
            {"e-mask-022.svg", Params::Skip()},  // UB: Recursive on child
            {"e-mask-025.svg", Params::Skip()},  // BUG: Rendering issue, mask is clipped. Repros in
                                                 // renderer_tool but not viewer.
            {"e-mask-026.svg", Params::Skip()},  // BUG: Mask on self, also a bug in browsers
            {"e-mask-027.svg", Params::Skip()},  // BUG: Mask on child doesn't apply
            {"e-mask-029.svg", Params::Skip()},  // BUG: Crashes on serializing the skp
            {"e-mask-030.svg", Params::Skip()},  // BUG: Crashes on serializing the skp
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Path, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-path",
                                {
                                    {"e-path-011.svg",
                                     Params::WithThreshold(kDefaultThreshold,
                                                           120)},  // Larger threshold due to
                                                                   // rasterization artifacts.
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Pattern, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-pattern",
        {
            {"e-pattern-003.svg", Params::Skip()},  // UB: overflow=visible
            {"e-pattern-008.svg",
             Params::WithThreshold(kDefaultThreshold, 250)},  // Larger threshold due to
                                                              // objectBoundingBox pattern AA.
            {"e-pattern-010.svg",
             Params::WithThreshold(kDefaultThreshold, 150)},  // Larger threshold due to
                                                              // viewBox/objectBoundingBox AA.
            {"e-pattern-018.svg", Params::Skip()},            // Not impl: <text>
            {"e-pattern-019.svg", Params()},
            {"e-pattern-020.svg",
             Params::WithThreshold(0.6f, 1000)},  // Larger threshold due to nested pattern AA.
            {"e-pattern-021.svg",
             Params::WithThreshold(0.2f)},  // Larger threshold due to recursive pattern seams.
            {"e-pattern-022.svg",
             Params::WithThreshold(0.2f)},  // Larger threshold due to recursive pattern seams.
            {"e-pattern-023.svg",
             Params::WithThreshold(0.2f)},  // Larger threshold due to recursive pattern seams.
            {"e-pattern-024.svg", Params()},
            {"e-pattern-025.svg", Params()},
            {"e-pattern-026.svg", Params()},
            {"e-pattern-027.svg", Params()},
            {"e-pattern-028.svg", Params::Skip()},  // UB: Invalid patternTransform
            {"e-pattern-029.svg", Params()},
            {"e-pattern-030.svg", Params::WithThreshold(0.02f)},  // Has anti-aliasing artifacts.
        })),
    TestNameFromFilename);

// TinySkia tolerance: stroked polygon/polyline edges have sub-pixel AA differences between
// tiny-skia v0.6 (resvg golden images) and v0.12 (our C++ port base).
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
             Params::Skip()},  // Test suite bug? In SVG2 this was changed to draw
                               // conical gradient instead of correcting focal point.
            {"e-radialGradient-032.svg", Params::Skip()},  // UB: Negative `r`
            {"e-radialGradient-039.svg", Params::Skip()},  // UB: Invalid `gradientUnits`
            {"e-radialGradient-040.svg", Params::Skip()},  // UB: Invalid `gradientTransform`
            {"e-radialGradient-043.svg", Params::Skip()},  // UB: fr=0.5 (SVG 2)
            {"e-radialGradient-044.svg", Params::Skip()},  // Test suite bug? fr > default value of
                                                           // r (0.5) should not
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

INSTANTIATE_TEST_SUITE_P(
    SymbolElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-symbol",
                                {

                                    {"e-symbol-010.svg",
                                     Params::Skip()},  // New SVG2 feature, transform on symbol

                                })),
    TestNameFromFilename);

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
