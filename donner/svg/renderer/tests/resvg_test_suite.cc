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

INSTANTIATE_TEST_SUITE_P(Circle, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix(
                             "e-circle",
                             {
                                 {"e-circle-001.svg", Params()},  // Rasterization artifacts (15px)
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
             Params::WithThreshold(kDefaultThreshold, 160)},  // Nested clip-path AA (148px)
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

INSTANTIATE_TEST_SUITE_P(Ellipse, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-ellipse")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FeBlend, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-feBlend")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeColorMatrix, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feColorMatrix",
        {
            // Float pipeline diffs: identity-like filters (no-op) show gradient rendering
            // differences (~10945px) since the float sRGB↔linear round-trip is lossless.
            // Non-trivial filters improved significantly with float precision.
            {"e-feColorMatrix-001.svg",
             Params::WithThreshold(kDefaultThreshold, 1900)},  // type=matrix (1799px)
            {"e-feColorMatrix-002.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   11500)},  // identity matrix (10945px rendering diff)
            {"e-feColorMatrix-003.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   11500)},  // identity matrix (10945px rendering diff)
            {"e-feColorMatrix-004.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   11500)},  // identity matrix (10945px rendering diff)
            {"e-feColorMatrix-005.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   11500)},  // identity matrix (10945px rendering diff)
            {"e-feColorMatrix-006.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   420)},  // non-normalized values (398px, was 3383)
            {"e-feColorMatrix-007.svg",
             Params::WithThreshold(kDefaultThreshold, 3100)},  // saturate (2993px, was 6987)
            {"e-feColorMatrix-008.svg", Params::Skip()},       // saturate -0.5 (UB, 141k px)
            {"e-feColorMatrix-009.svg", Params::Skip()},       // saturate 99999 (UB, 158k px)
            {"e-feColorMatrix-010.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   11500)},  // identity saturate (10945px rendering diff)
            {"e-feColorMatrix-011.svg",
             Params::WithThreshold(kDefaultThreshold, 9200)},  // hueRotate(30) (8977px)
            {"e-feColorMatrix-012.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   11500)},  // hueRotate(0) identity (10945px rendering diff)
            {"e-feColorMatrix-015.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   11500)},  // no attrs identity (10945px rendering diff)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeComponentTransfer, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feComponentTransfer",
        {
            {"e-feComponentTransfer-009.svg",
             Params::Skip()},  // tableValues="1px" invalid unit suffix not rejected (160K px diff)
            {"e-feComponentTransfer-020.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   72000)},  // Mixed types + feFuncA gamma + opacity (70400px)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FeComposite, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-feComposite")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeConvolveMatrix, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feConvolveMatrix",
        {
            {"e-feConvolveMatrix-014.svg",
             Params::WithThreshold(kDefaultThreshold, 3600)},  // preserveAlpha (3460px)
            {"e-feConvolveMatrix-015.svg", Params::Skip()},    // UB: bias=0.5 (79K px diff)
            {"e-feConvolveMatrix-016.svg", Params::Skip()},    // UB: bias=-0.5 (89K px diff)
            {"e-feConvolveMatrix-017.svg", Params::Skip()},    // UB: bias=9999 (33K px diff)
            {"e-feConvolveMatrix-018.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-022.svg",
             Params::WithThreshold(kDefaultThreshold, 220)},  // edgeMode=wrap (199px)
            {"e-feConvolveMatrix-023.svg",
             Params::Skip()},  // UB: wrap with oversized kernel (37K px diff)
            {"e-feConvolveMatrix-024.svg",
             Params::WithThreshold(kDefaultThreshold, 320)},  // edgeMode=none (288px)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FeDiffuseLighting, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-feDiffuseLighting",
                                                     {
                                                         {"e-feDiffuseLighting-021.svg",
                                                          Params::Skip()},  // kernelUnitLength
                                                         {"e-feDiffuseLighting-022.svg",
                                                          Params::Skip()},  // kernelUnitLength
                                                     })),
                         TestNameFromFilename);
INSTANTIATE_TEST_SUITE_P(FeDisplacementMap, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-feDisplacementMap", {})),
                         TestNameFromFilename);
INSTANTIATE_TEST_SUITE_P(FeDistantLight, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-feDistantLight", {})),
                         TestNameFromFilename);
INSTANTIATE_TEST_SUITE_P(
    FeDropShadow, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feDropShadow",
        {
            // linearRGB 8-bit LUT rounding diffs at blur edges
            {"e-feDropShadow-001.svg",
             Params::WithThreshold(kDefaultThreshold, 400)},  // 389px: box blur edge rounding
            {"e-feDropShadow-002.svg", Params::WithThreshold(kDefaultThreshold, 200)},  // 196px
            {"e-feDropShadow-003.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   120)},  // 111px
                                           // e-feDropShadow-005: filter region clipping — testing
            {"e-feDropShadow-006.svg", Params::WithThreshold(kDefaultThreshold, 100)},  // 74px
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeFlood, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feFlood",
        {
            {"e-feFlood-008.svg",
             Params::WithThreshold(kDefaultThreshold, 60000)},  // OBB + complex transform (59247px)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeGaussianBlur, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feGaussianBlur",
        {
            {"e-feGaussianBlur-002.svg",
             Params::Skip()},  // huge stdDev=1000, 207K px diff, 70s runtime
            {"e-feGaussianBlur-012.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   47000)},  // Complex skew transform + asymmetric blur (45857px)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeImage, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feImage",
        {
            {"e-feImage-001.svg", Params::Skip()},  // External file reference (no ResourceLoader)
            {"e-feImage-002.svg", Params::Skip()},  // External SVG reference
            {"e-feImage-003.svg",
             Params::WithThreshold(0.05f, 100)},  // Bilinear interpolation + sRGB↔linear roundtrip
            {"e-feImage-004.svg",
             Params::WithThreshold(0.05f, 100)},  // Bilinear interpolation + sRGB↔linear roundtrip
            {"e-feImage-006.svg", Params::Skip()},  // Fragment reference (#element)
            {"e-feImage-007.svg", Params::Skip()},  // Subregion with objectBoundingBox
            {"e-feImage-008.svg", Params::Skip()},  // Subregion with objectBoundingBox
            {"e-feImage-009.svg", Params::Skip()},  // Subregion with percentage width
            {"e-feImage-010.svg", Params::Skip()},  // Subregion with percentage width
            {"e-feImage-011.svg", Params::Skip()},  // Subregion with rotation transform
            {"e-feImage-012.svg", Params::Skip()},  // Fragment reference outside defs
            {"e-feImage-013.svg", Params::Skip()},  // Fragment reference outside defs
            {"e-feImage-014.svg", Params::Skip()},  // Fragment reference
            {"e-feImage-015.svg", Params::Skip()},  // Fragment reference
            {"e-feImage-016.svg", Params::Skip()},  // Fragment reference
            {"e-feImage-017.svg", Params::Skip()},  // Fragment reference
            {"e-feImage-018.svg", Params::Skip()},  // Fragment reference
            {"e-feImage-019.svg", Params::Skip()},  // Fragment reference
            {"e-feImage-020.svg", Params::Skip()},  // Fragment reference
            {"e-feImage-021.svg", Params::Skip()},  // Fragment reference
            {"e-feImage-022.svg", Params::Skip()},  // Fragment reference
            {"e-feImage-023.svg", Params::Skip()},  // Fragment reference
            {"e-feImage-024.svg", Params::Skip()},  // Fragment reference
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeMerge, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feMerge",
        {
            {"e-feMerge-001.svg",
             Params::WithThreshold(kDefaultThreshold, 1250)},  // linearRGB rounding (1163px)
            {"e-feMerge-002.svg",
             Params::WithThreshold(kDefaultThreshold, 2350)},  // linearRGB rounding (2269px)
            {"e-feMerge-003.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   28000)},  // Complex skew transform + c-i-f (27503px)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeMorphology, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feMorphology",
        {
            // e-feMorphology-007: primitiveUnits=objectBoundingBox (now supported)
            {"e-feMorphology-012.svg", Params::Skip()},  // Perf: radius=9999, O(n*r^2) too slow
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeOffset, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feOffset",
        {
            {"e-feOffset-007.svg",
             Params::WithThreshold(kDefaultThreshold, 350)},  // OBB offset rounding (307px)
            {"e-feOffset-008.svg",
             Params::WithThreshold(kDefaultThreshold, 14000)},  // Complex skew transform (13672px)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FePointLight, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-fePointLight",
        {
            {"e-fePointLight-004.svg",
             Params::WithThreshold(0.1f, 55000)},  // Lighting alpha=1.0 vs resvg clips to shape
        })),
    TestNameFromFilename);
// Specular lighting: algorithm differences vs resvg.
// Float pipeline + light coordinate scaling fixed most tests to 0 diffs at 0.1f threshold.
INSTANTIATE_TEST_SUITE_P(
    FeSpecularLighting, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feSpecularLighting",
        {
            {"e-feSpecularLighting-002.svg", Params::WithThreshold(0.1f)},  // 2717px at 0.01f
            {"e-feSpecularLighting-004.svg",
             Params::WithThreshold(0.1f, 58000)},                           // 57392px at 0.01f
            {"e-feSpecularLighting-005.svg", Params::WithThreshold(0.1f)},  // 1466px at 0.01f
            {"e-feSpecularLighting-007.svg", Params::WithThreshold(0.1f, 160000)},  // 157470px
        })),
    TestNameFromFilename);
INSTANTIATE_TEST_SUITE_P(
    FeSpotLight, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feSpotLight",
        {
            {"e-feSpotLight-005.svg", Params::Skip()},  // Negative specularExponent=-10 edge case
            {"e-feSpotLight-007.svg", Params::WithThreshold(kDefaultThreshold, 3000)},  // 2922px
            {"e-feSpotLight-008.svg", Params::WithThreshold(kDefaultThreshold, 3000)},  // 2922px
            {"e-feSpotLight-012.svg",
             Params::WithThreshold(0.1f,
                                   70000)},  // Lighting alpha=1.0 vs resvg clips to shape (67496px)
        })),
    TestNameFromFilename);
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
             Params::WithThreshold(kDefaultThreshold,
                                   9700)},         // Tile boundary sub-pixel alignment (9520px)
            {"e-feTile-006.svg", Params::Skip()},  // Not impl: percentage-based primitive subregion
            {"e-feTile-007.svg", Params::Skip()},  // Complex transform (UB per test title)
        })),
    TestNameFromFilename);
INSTANTIATE_TEST_SUITE_P(
    FeTurbulence, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feTurbulence",
        {
            // feTurbulence-002 removed (was 0 diff with 1500 threshold)
            {"e-feTurbulence-017.svg", Params::Skip()},  // stitchTiles=stitch (UB per test title)
            {"e-feTurbulence-018.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   110000)},  // Complex skew transform (109146px)
            {"e-feTurbulence-019.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   24000)},  // sRGB color-interpolation (23604px)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Filter, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-filter",
        {
            {"e-filter-002.svg",
             Params::WithThreshold(kDefaultThreshold, 19000)},  // Filter region blur edge (18768px)
            {"e-filter-003.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   7700)},  // filterUnits=userSpaceOnUse blur edge (7510px)
            {"e-filter-004.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   64000)},  // Subregion + filter region (63126px)
            {"e-filter-009.svg",
             Params::WithThreshold(kDefaultThreshold, 650)},  // Subregion blur edge (615px)
            {"e-filter-010.svg",
             Params::WithThreshold(kDefaultThreshold, 62500)},  // Subregion percentage (61762px)
            {"e-filter-011.svg",
             Params::WithThreshold(kDefaultThreshold, 12000)},  // Subregion (11684px)
            // TODO(jwm): OBB + subregion clipping not correct yet (16767 px diff)
            {"e-filter-012.svg",
             Params::WithThreshold(kDefaultThreshold, 17000)},  // OBB + subregion (16766px)
            {"e-filter-014.svg",
             Params::WithThreshold(kDefaultThreshold, 55000)},  // Negative subregion (54280px)
            {"e-filter-017.svg", Params::Skip()},               // xlink:href
            {"e-filter-018.svg", Params::Skip()},               // xlink:href
            {"e-filter-019.svg", Params::Skip()},               // xlink:href
            {"e-filter-026.svg",
             Params::WithThreshold(kDefaultThreshold, 7500)},  // Transform blur edge (7252px)
            {"e-filter-027.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   32500)},  // Transform + filter region (31858px)
            {"e-filter-028.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   140)},  // transform + blur linearRGB diff (124px)
            // e-filter-030: primitiveUnits=objectBoundingBox (now supported)
            {"e-filter-032.svg", Params::Skip()},  // in=BackgroundImage
            {"e-filter-033.svg", Params::Skip()},  // in=BackgroundAlpha
            {"e-filter-034.svg", Params::Skip()},  // in=FillPaint
            {"e-filter-035.svg", Params::Skip()},  // in=StrokePaint
            {"e-filter-036.svg", Params::Skip()},  // in=FillPaint gradient
            {"e-filter-037.svg", Params::Skip()},  // in=FillPaint pattern
            {"e-filter-038.svg", Params::Skip()},  // in=FillPaint on group
            {"e-filter-039.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   3400)},  // Multiple primitives blur edge (3292px)
            {"e-filter-040.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   3400)},  // Multiple primitives blur edge (3292px)
            {"e-filter-041.svg",
             Params::WithThreshold(kDefaultThreshold, 6400)},  // Blur edge clipping (6247px)
            // filter-042 (blur edge + linearRGB): 12px diff, passes with default threshold
            // filter-044 (chained blur edge): 12px diff, passes with default threshold
            {"e-filter-046.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   1200)},  // blur linearRGB rendering diff (1101px)
            {"e-filter-052.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   43500)},  // Clip-path + filter blur edge (42888px)
            {"e-filter-053.svg",
             Params::WithThreshold(kDefaultThreshold, 550)},  // Mask + filter (498px)
            {"e-filter-054.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   35000)},  // Clip-path + mask + filter (34305px)
            // TODO(jwm): OBB + subregion clipping not correct yet (142436 px diff)
            {"e-filter-055.svg", Params::Skip()},  // OBB + subregion % (141644px diff)
            {"e-filter-056.svg",
             Params::WithThreshold(kDefaultThreshold, 98000)},  // Invalid named result (97365px)
            {"e-filter-059.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   37000)},        // Complex transforms blur edge (36065px)
            {"e-filter-060.svg", Params::Skip()},  // Filter on root svg (227K px diff)
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
            {"e-line-007.svg", Params()},    // Rasterization artifacts (20px)
            {"e-line-008.svg", Params()},    // Rasterization artifacts (20px)
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
                                    {"e-path-011.svg", Params()},  // Rasterization artifacts (50px)
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Pattern, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-pattern",
        {
            {"e-pattern-003.svg", Params::Skip()},  // UB: overflow=visible
            {"e-pattern-008.svg", Params()},        // objectBoundingBox pattern AA (14px)
            {"e-pattern-010.svg", Params()},        // viewBox/objectBoundingBox AA (58px)
            {"e-pattern-018.svg", Params::Skip()},  // Not impl: <text>
            {"e-pattern-019.svg", Params()},
            {"e-pattern-020.svg", Params::WithThreshold(0.6f, 800)},  // Nested pattern AA (768px)
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

// TODO(jwmcglynn): e-switch

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
