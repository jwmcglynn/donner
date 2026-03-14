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

INSTANTIATE_TEST_SUITE_P(
    AlignmentBaseline, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("a-alignment-baseline", {},
                                Params::WithThreshold(kDefaultThreshold, 13000))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    BaselineShift, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-baseline-shift",
        {
            {"a-baseline-shift-014.svg",
             Params::WithThreshold(kDefaultThreshold, 17000)},  // sub/super
            {"a-baseline-shift-015.svg",
             Params::WithThreshold(kDefaultThreshold, 21000)},  // percentage
            {"a-baseline-shift-016.svg",
             Params::WithThreshold(kDefaultThreshold, 21000)},  // percentage
            {"a-baseline-shift-017.svg",
             Params::WithThreshold(kDefaultThreshold, 21000)},  // percentage
            {"a-baseline-shift-018.svg",
             Params::WithThreshold(kDefaultThreshold, 21000)},  // percentage
            {"a-baseline-shift-020.svg",
             Params::WithThreshold(kDefaultThreshold, 19500)},  // nested
            {"a-baseline-shift-021.svg",
             Params::WithThreshold(kDefaultThreshold, 19500)},  // nested
        },
        Params::WithThreshold(kDefaultThreshold, 8500))),
    TestNameFromFilename);

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
                                    {"a-display-005.svg",
                                     Params::WithThreshold(kDefaultThreshold, 18000)},  // <tspan>
                                    {"a-display-006.svg", Params::Skip()},  // Not impl: <tref>
                                    {"a-display-009.svg",
                                     Params::WithThreshold(kDefaultThreshold, 18000)},  // <tspan>
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
            {"a-fill-031.svg",
             Params::WithThreshold(kDefaultThreshold, 23000)},  // <text>
            {"a-fill-032.svg",
             Params::WithThreshold(kDefaultThreshold, 23000)},  // <text>
            {"a-fill-033.svg", Params::Skip()},          // Not impl: <pattern> fill on text
            {"a-fill-opacity-004.svg", Params::Skip()},  // Not impl: `fill-opacity` affects pattern
            {"a-fill-opacity-006.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // <text>
        })),
    TestNameFromFilename);

// TODO(filter): a-filter
// TODO(filter): a-flood
// TODO(font): a-font
// TODO(font): a-glyph-orientation
// TODO(filter?): a-isolation
INSTANTIATE_TEST_SUITE_P(
    Kerning, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("a-kerning", {},
                                Params::WithThreshold(kDefaultThreshold, 10000))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    LengthAdjust, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("a-lengthAdjust", {},
                                Params::WithThreshold(kDefaultThreshold, 15000))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    LetterSpacing, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("a-letter-spacing", {},
                                Params::WithThreshold(kDefaultThreshold, 17000))),
    TestNameFromFilename);

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
                                                          Params::WithThreshold(
                                                              kDefaultThreshold, 18000)},  // <text>
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
            {"a-stroke-007.svg",
             Params::WithThreshold(kDefaultThreshold, 20000)},  // <text>
            {"a-stroke-008.svg",
             Params::WithThreshold(kDefaultThreshold, 22000)},  // <text>
            {"a-stroke-009.svg",
             Params::WithThreshold(kDefaultThreshold, 33000)},  // <text> stroke on complex text
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
            {"a-stroke-opacity-006.svg",
             Params::WithThreshold(kDefaultThreshold, 21000)},  // <text>
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
INSTANTIATE_TEST_SUITE_P(
    TextAnchor, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-text-anchor",
        {
            {"a-text-anchor-005.svg",
             Params::WithThreshold(kDefaultThreshold, 16000)},  // tspan with anchor
            {"a-text-anchor-010.svg",
             Params::WithThreshold(kDefaultThreshold, 21500)},  // RTL text
        },
        Params::WithThreshold(kDefaultThreshold, 10500))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextDecoration, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-text-decoration",
        {
            {"a-text-decoration-004.svg",
             Params::WithThreshold(kDefaultThreshold, 15000)},  // overline
            {"a-text-decoration-015.svg",
             Params::WithThreshold(kDefaultThreshold, 14500)},  // gradient decoration
            {"a-text-decoration-018.svg",
             Params::WithThreshold(kDefaultThreshold, 19500)},  // on rotated text
        },
        Params::WithThreshold(kDefaultThreshold, 13000))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextRendering, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("a-text-rendering", {},
                                Params::WithThreshold(kDefaultThreshold, 19500))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextLength, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-textLength",
        {
            {"a-textLength-008.svg",
             Params::WithThreshold(kDefaultThreshold, 24000)},  // textLength on text + tspan
        },
        Params::WithThreshold(kDefaultThreshold, 12000))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Transform, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-transform",
        {
            {"a-transform-007.svg",
             Params::WithThreshold(0.05f)},  // Larger threshold due to anti-aliasing artifacts.
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    UnicodeBidi, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("a-unicode", {},
                                Params::WithThreshold(kDefaultThreshold, 2500))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Visibility, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("a-visibility",  //
                                {
                                    {"a-visibility-003.svg",
                                     Params::WithThreshold(kDefaultThreshold, 18000)},  // <tspan>
                                    {"a-visibility-004.svg",
                                     Params::WithThreshold(kDefaultThreshold, 18000)},  // <tspan>
                                    {"a-visibility-007.svg",
                                     Params::WithThreshold(kDefaultThreshold, 18000)},  // <text>
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    WordSpacing, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-word-spacing",
        {
            {"a-word-spacing-007.svg", Params::Skip()},  // UB: word-spacing=-10000
        },
        Params::WithThreshold(kDefaultThreshold, 4500))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    WritingMode, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-writing-mode",
        {
            {"a-writing-mode-016.svg", Params::Skip()},  // UB: tb with rotate
            {"a-writing-mode-017.svg", Params::Skip()},  // UB: tb with rotate and underline
        },
        Params::WithThreshold(kDefaultThreshold, 17000))),
    TestNameFromFilename);

// TODO: e-a-

INSTANTIATE_TEST_SUITE_P(Circle, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-circle", {})), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    ClipPath, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-clipPath",
        {
            {"e-clipPath-007.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // <text>
            {"e-clipPath-009.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // <text>
            {"e-clipPath-010.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // <text>
            {"e-clipPath-011.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // <text>
            {"e-clipPath-012.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // <text>
            {"e-clipPath-034.svg",
             Params::WithThreshold(kDefaultThreshold, 160)},  // Nested clip-path AA (148px)
            {"e-clipPath-042.svg", Params::Skip()},           // UB: on root `<svg>` without size
            {"e-clipPath-044.svg", Params::Skip()},           // Not impl: <use> child
            {"e-clipPath-046.svg", Params::Skip()},           // Not impl: <switch>
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Defs, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-defs",
                                                     {
                                                         {"e-defs-007.svg",
                                                          Params::WithThreshold(
                                                              kDefaultThreshold, 18000)},  // <text>
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
            // All feColorMatrix diffs are from resvg's uint8 sRGB↔linear quantization vs our
            // float pipeline. Diffs are small per-pixel (below YIQ 0.05) but numerous because
            // the test gradient has many unique colors affected by quantization.
            {"e-feColorMatrix-001.svg", Params::WithThreshold(0.05f)},  // type=matrix
            {"e-feColorMatrix-002.svg", Params::WithThreshold(0.05f)},  // identity matrix
            {"e-feColorMatrix-003.svg", Params::WithThreshold(0.05f)},  // identity matrix
            {"e-feColorMatrix-004.svg", Params::WithThreshold(0.05f)},  // identity matrix
            {"e-feColorMatrix-005.svg", Params::WithThreshold(0.05f)},  // identity matrix
            {"e-feColorMatrix-006.svg", Params::WithThreshold(0.05f)},  // non-normalized values
            {"e-feColorMatrix-007.svg", Params::WithThreshold(0.05f)},  // saturate
            {"e-feColorMatrix-008.svg", Params::Skip()},                // saturate -0.5 (UB)
            {"e-feColorMatrix-009.svg", Params::Skip()},                // saturate 99999 (UB)
            {"e-feColorMatrix-010.svg", Params::WithThreshold(0.05f)},  // identity saturate
            {"e-feColorMatrix-011.svg", Params::WithThreshold(0.05f)},  // hueRotate(30)
            {"e-feColorMatrix-012.svg", Params::WithThreshold(0.05f)},  // hueRotate(0) identity
            {"e-feColorMatrix-015.svg", Params::WithThreshold(0.05f)},  // no attrs identity
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeComponentTransfer, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-feComponentTransfer")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FeComposite, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-feComposite")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeConvolveMatrix, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feConvolveMatrix",
        {
            {"e-feConvolveMatrix-014.svg",
             Params::WithThreshold(kDefaultThreshold, 7000)},  // filter region boundary edges (6584px)
            {"e-feConvolveMatrix-015.svg", Params::Skip()},    // UB: bias=0.5 (79K px diff)
            {"e-feConvolveMatrix-016.svg", Params::Skip()},    // UB: bias=-0.5 (89K px diff)
            {"e-feConvolveMatrix-017.svg", Params::Skip()},    // UB: bias=9999 (33K px diff)
            {"e-feConvolveMatrix-018.svg", Params::WithThreshold(kDefaultThreshold, 0)},
            {"e-feConvolveMatrix-022.svg",
             Params::WithThreshold(kDefaultThreshold, 220)},  // edgeMode=wrap (187px)
            {"e-feConvolveMatrix-023.svg",
             Params::Skip()},  // UB: wrap with oversized kernel (37K px diff)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(FeDiffuseLighting, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix(
                             "e-feDiffuseLighting",
                             {
                                 {"e-feDiffuseLighting-021.svg",
                                  Params::WithThreshold(0.1f, 750)},  // Transformed diffuse (690px)
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
             Params::WithThreshold(kDefaultThreshold, 300)},  // box blur edge rounding (246px)
            {"e-feDropShadow-002.svg", Params::WithThreshold(kDefaultThreshold, 150)},  // 129px
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeFlood, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-feFlood",
                                {
                                    {"e-feFlood-008.svg",
                                     Params::WithThreshold(kDefaultThreshold,
                                                           18000)},  // OBB + complex transform (17501px at 0.02)
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
                                   200)},  // Rotated asymmetric blur, transformed path
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
            {"e-feImage-006.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   9500)},  // Fragment ref edge AA (8-bit intermediate buffer)
            {"e-feImage-007.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   4500)},  // OBB subregion, bilinear edge diff (4143px at 0.02)
            {"e-feImage-008.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   4500)},  // OBB subregion with percentage (4143px at 0.02)
            {"e-feImage-009.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   13500)},  // Percentage width subregion (12113px at 0.02)
            {"e-feImage-010.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   14000)},  // Absolute subregion coords (12682px at 0.02)
            {"e-feImage-011.svg",
             Params::Skip()},  // Subregion with rotation: filter region sizing mismatch (64K px)
            {"e-feImage-012.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   36000)},  // Fragment ref outside defs, larger rect edge diff
            {"e-feImage-013.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   9500)},  // Fragment ref group, edge AA
            {"e-feImage-014.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   9500)},  // Fragment ref use element, edge AA
            {"e-feImage-017.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   9500)},  // Fragment ref outside defs (2), edge AA
            {"e-feImage-019.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   18200)},  // Fragment ref with skewX transform on element
            {"e-feImage-021.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   34200)},  // Fragment ref with complex transform (skewX+translate)
            {"e-feImage-023.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   9500)},  // Fragment ref with opacity, edge AA
            {"e-feImage-024.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   33600)},  // Chained feImage fragment refs
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeMerge, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feMerge",
        {
            {"e-feMerge-001.svg",
             Params::WithThreshold(kDefaultThreshold, 1250)},  // linearRGB rounding (1117px at 0.02)
            {"e-feMerge-002.svg",
             Params::WithThreshold(kDefaultThreshold, 2350)},  // linearRGB rounding (2203px at 0.02)
            {"e-feMerge-003.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   7000)},  // Complex skew transform + c-i-f (6372px at 0.02)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeMorphology, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-feMorphology",
                                {
                                    {"e-feMorphology-012.svg",
                                     Params::Skip()},  // Perf: radius=9999, O(n*r^2) too slow
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FeOffset, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feOffset",
        {
            {"e-feOffset-008.svg",
             Params::WithThreshold(kDefaultThreshold, 2000)},  // Complex skew transform (1828px at 0.02)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FePointLight, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-fePointLight",
        {
            {"e-fePointLight-004.svg",
             Params::WithThreshold(0.1f, 120)},  // Lighting alpha=1.0 vs resvg clips to shape (54px)
        })),
    TestNameFromFilename);
// Specular lighting: algorithm differences vs resvg.
// Float pipeline + light coordinate scaling fixed most tests to 0 diffs at 0.1f threshold.
// specularExponent out-of-range handling: <1 skips primitive (transparent), >128 clamps to 128.
INSTANTIATE_TEST_SUITE_P(
    FeSpecularLighting, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feSpecularLighting",
        {
            {"e-feSpecularLighting-002.svg", Params::WithThreshold(0.1f)},  // 2717px at 0.01f
            {"e-feSpecularLighting-004.svg",
             Params::WithThreshold(0.1f, 58000)},  // resvg golden bug: R=0 channel (BGRA issue in
                                                    // resvg ~v0.9.x)
            {"e-feSpecularLighting-005.svg", Params::WithThreshold(0.1f)},  // 1466px at 0.01f
        })),
    TestNameFromFilename);
INSTANTIATE_TEST_SUITE_P(
    FeSpotLight, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feSpotLight",
        {
            // feSpotLight-005: negative specularExponent=-10 on feSpotLight. Non-positive values
            // fall back to default 1.0, matching resvg's PositiveF32 validation.
            // feSpotLight-007/008: 0px at 0.02 threshold (were 2922px at 0.01)
            {"e-feSpotLight-012.svg",
             Params::WithThreshold(0.1f, 15200)},  // Lighting alpha=1.0 vs resvg clips to shape (14622px)
        })),
    TestNameFromFilename);
INSTANTIATE_TEST_SUITE_P(
    FeTile, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-feTile",
        {
            // feTile-001/002/004/005: 0px at 0.02 threshold (were 2500-9700px at 0.01)
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
                                   500)},  // Complex skew transform (426px at 0.02)
            {"e-feTurbulence-019.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   1100)},  // sRGB color-interpolation (990px at 0.02)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Filter, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-filter",
        {
            // filter-004/009/010/014/017/018/028/046/053/054: 0px at 0.02 threshold
            {"e-filter-011.svg",
             Params::WithThreshold(kDefaultThreshold, 8000)},  // Subregion (7240px at 0.02)
            {"e-filter-019.svg",
             Params::WithThreshold(kDefaultThreshold, 4100)},  // inherited filter blur edge (3700px at 0.02)
            {"e-filter-027.svg",
             Params::WithThreshold(kDefaultThreshold,
                                   6000)},  // Skew transform + narrow filter region (5406px at 0.02)
            {"e-filter-032.svg", Params::Skip()},  // in=BackgroundImage
            {"e-filter-033.svg", Params::Skip()},  // in=BackgroundAlpha
            {"e-filter-034.svg", Params::Skip()},  // UB: in=FillPaint
            {"e-filter-035.svg", Params::Skip()},  // UB: in=StrokePaint
            {"e-filter-036.svg", Params::Skip()},  // UB: in=FillPaint gradient
            {"e-filter-037.svg", Params::Skip()},  // UB: in=FillPaint pattern
            {"e-filter-038.svg", Params::Skip()},  // UB: in=FillPaint on group
            // e-filter-056.svg: fixed invalid named result fallback (was 97500px, now <100)
            {"e-filter-060.svg", Params::Skip()},               // Filter on root svg (227K px diff)
            {"e-filter-065.svg", Params::Skip()},               // UB: in=FillPaint on empty group
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
            {"e-marker-017.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // <text>
            {"e-marker-018.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // <text>
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

INSTANTIATE_TEST_SUITE_P(Path, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-path", {})), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Pattern, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-pattern",
        {
            {"e-pattern-003.svg", Params::Skip()},                    // UB: overflow=visible
            {"e-pattern-018.svg",
             Params::WithThreshold(kDefaultThreshold, 22000)},  // <text>
            {"e-pattern-020.svg", Params::WithThreshold(0.6f, 800)},  // Nested pattern AA (768px)
            {"e-pattern-021.svg",
             Params::WithThreshold(0.2f)},  // Larger threshold due to recursive pattern seams.
            {"e-pattern-022.svg",
             Params::WithThreshold(0.2f)},  // Larger threshold due to recursive pattern seams.
            {"e-pattern-023.svg",
             Params::WithThreshold(0.2f)},  // Larger threshold due to recursive pattern seams.
            {"e-pattern-028.svg", Params::Skip()},                // UB: Invalid patternTransform
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

INSTANTIATE_TEST_SUITE_P(
    TextElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-text-",
        {
            {"e-text-011.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // Rotate
            {"e-text-012.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // Per-char rotate
            {"e-text-014.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // Per-char rotate (excess values)
            {"e-text-018.svg",
             Params::WithThreshold(kDefaultThreshold, 24000)},  // Escaped text (HTML entities)
            {"e-text-027.svg",
             Params::WithThreshold(kDefaultThreshold, 18000)},  // Emoji (color font)
            {"e-text-030.svg",
             Params::WithThreshold(kDefaultThreshold, 15500)},  // Arabic RTL text
            {"e-text-033.svg",
             Params::WithThreshold(kDefaultThreshold, 14000)},  // Underline + pattern + rotate
            {"e-text-034.svg",
             Params::WithThreshold(kDefaultThreshold, 20500)},  // Rotate with complex text
            {"e-text-042.svg", Params::Skip()},                 // UB: grapheme split by tspan
            {"e-text-043.svg",
             Params::WithThreshold(kDefaultThreshold, 19000)},  // xml:lang=ja
        },
        Params::WithThreshold(kDefaultThreshold, 17500))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TextPathElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-textPath",
                                {
                                    // Glyph outline differences (stb_truetype vs FreeType/HarfBuzz).
                                    {"e-textPath-001.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4000)},  // Basic
                                    {"e-textPath-002.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4000)},  // startOffset=30
                                    {"e-textPath-003.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4000)},  // startOffset=5mm
                                    {"e-textPath-004.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4000)},  // startOffset=10%
                                    {"e-textPath-005.svg",
                                     Params::WithThreshold(kDefaultThreshold, 2000)},  // startOffset=-100
                                    {"e-textPath-007.svg", Params::Skip()},   // Not impl: method=stretch
                                    {"e-textPath-008.svg", Params::Skip()},   // Not impl: spacing=auto
                                    {"e-textPath-009.svg",
                                     Params::WithThreshold(kDefaultThreshold, 8000)},  // Two paths
                                    {"e-textPath-010.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4000)},  // Nested
                                    {"e-textPath-011.svg",
                                     Params::WithThreshold(kDefaultThreshold, 8200)},  // Mixed children (1)
                                    {"e-textPath-012.svg",
                                     Params::WithThreshold(kDefaultThreshold, 7700)},  // Mixed children (2)
                                    {"e-textPath-013.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4100)},  // Coords on text
                                    {"e-textPath-014.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4100)},  // Coords on textPath
                                    {"e-textPath-015.svg",
                                     Params::WithThreshold(kDefaultThreshold, 3600)},  // Very long text
                                    {"e-textPath-016.svg", Params::Skip()},   // Not impl: link to rect (SVG 2)
                                    {"e-textPath-019.svg",
                                     Params::WithThreshold(kDefaultThreshold, 2000)},  // text-anchor
                                    {"e-textPath-020.svg",
                                     Params::WithThreshold(kDefaultThreshold, 8200)},  // Closed path
                                    {"e-textPath-021.svg",
                                     Params::WithThreshold(kDefaultThreshold, 18000)},  // writing-mode=tb
                                    {"e-textPath-022.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4400)},  // tspan absolute pos
                                    {"e-textPath-023.svg",
                                     Params::WithThreshold(kDefaultThreshold, 5000)},  // tspan relative pos
                                    {"e-textPath-024.svg",
                                     Params::WithThreshold(kDefaultThreshold, 5000)},  // Path with subpaths
                                    {"e-textPath-025.svg",
                                     Params::WithThreshold(kDefaultThreshold, 5500)},  // Invalid textPath mid
                                    {"e-textPath-026.svg",
                                     Params::WithThreshold(kDefaultThreshold, 8600)},  // Path with ClosePath
                                    {"e-textPath-027.svg",
                                     Params::WithThreshold(kDefaultThreshold, 6600)},  // M L Z path
                                    {"e-textPath-028.svg",
                                     Params::WithThreshold(kDefaultThreshold, 18000)},  // underline
                                    {"e-textPath-029.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4000)},  // With rotate
                                    {"e-textPath-030.svg",
                                     Params::WithThreshold(kDefaultThreshold, 5400)},  // Complex
                                    {"e-textPath-031.svg",
                                     Params::WithThreshold(kDefaultThreshold, 18000)},  // letter-spacing
                                    {"e-textPath-032.svg",
                                     Params::WithThreshold(kDefaultThreshold, 18000)},  // baseline-shift
                                    {"e-textPath-033.svg", Params::Skip()},   // UB: baseline-shift + rotate
                                    {"e-textPath-034.svg",
                                     Params::WithThreshold(kDefaultThreshold, 5000)},  // M A path
                                    {"e-textPath-035.svg",
                                     Params::WithThreshold(kDefaultThreshold, 8000)},  // dy with tiny coords
                                    {"e-textPath-036.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4100)},  // transform on ref path
                                    {"e-textPath-037.svg",
                                     Params::WithThreshold(kDefaultThreshold, 4100)},  // transform outside ref
                                    {"e-textPath-038.svg",
                                     Params::WithThreshold(kDefaultThreshold, 18000)},  // letter-spacing
                                    {"e-textPath-039.svg",
                                     Params::WithThreshold(kDefaultThreshold, 1200)},  // Subpaths + startOffset
                                    {"e-textPath-040.svg", Params::Skip()},   // Not impl: filter on textPath
                                    {"e-textPath-041.svg", Params::Skip()},   // Not impl: side=right (SVG 2)
                                    {"e-textPath-042.svg", Params::Skip()},   // Not impl: path attr (SVG 2)
                                    {"e-textPath-043.svg", Params::Skip()},   // Not impl: path + xlink:href
                                    {"e-textPath-044.svg", Params::Skip()},   // Not impl: invalid path + href
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    TspanElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-tspan",
        {
            {"e-tspan-013.svg",
             Params::WithThreshold(kDefaultThreshold, 32000)},  // Pseudo-multi-line
            {"e-tspan-020.svg",
             Params::WithThreshold(kDefaultThreshold, 17500)},  // SVG 2: mask on tspan
            {"e-tspan-022.svg",
             Params::WithThreshold(kDefaultThreshold, 45000)},  // SVG 2: filter on tspan
            {"e-tspan-024.svg",
             Params::WithThreshold(kDefaultThreshold, 25500)},  // Cross-tspan shaping (weight)
            {"e-tspan-027.svg",
             Params::WithThreshold(kDefaultThreshold, 14000)},  // Multiple coordinates
            {"e-tspan-028.svg",
             Params::WithThreshold(kDefaultThreshold, 15500)},  // Mixed font-size
            {"e-tspan-029.svg",
             Params::WithThreshold(kDefaultThreshold, 14500)},  // Rotate + display:none
            {"e-tspan-031.svg",
             Params::WithThreshold(kDefaultThreshold, 15000)},  // Nested whitespaces
        },
        Params::WithThreshold(kDefaultThreshold, 17000))),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Use, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-use",
                                {
                                    {"e-use-008.svg", Params::Skip()},  // Not impl: External file.
                                })),
    TestNameFromFilename);

}  // namespace donner::svg
