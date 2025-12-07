#include <gmock/gmock.h>

#include "donner/base/tests/Runfiles.h"
#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

using testing::ValuesIn;

namespace donner::svg {

using Params = ImageComparisonParams;

namespace {

// Maps old test prefixes to new directory paths in the resvg-test-suite.
// The test suite was restructured in May 2023, moving from a flat structure
// (svg/*.svg, png/*.png) to a nested structure (tests/**/*.svg with .png alongside).
std::string getPrefixDirectory(const char* prefix) {
  static const std::map<std::string, std::string> kPrefixMap = {
      // Attribute tests (a-*)
      {"a-alignment-baseline", "tests/text/alignment-baseline"},
      {"a-baseline-shift", "tests/text/baseline-shift"},
      {"a-clip-path", "tests/masking/clip-path"},
      {"a-clip-rule", "tests/masking/clip-rule"},
      {"a-clip", "tests/masking/clip"},
      {"a-color-interpolation-filters", "tests/filters/color-interpolation-filters"},
      {"a-color", "tests/painting/color"},
      {"a-direction", "tests/text/direction"},
      {"a-display", "tests/painting/display"},
      {"a-dominant-baseline", "tests/text/dominant-baseline"},
      {"a-enable-background", "tests/filters/enable-background"},
      {"a-fill-opacity", "tests/painting/fill-opacity"},
      {"a-fill-rule", "tests/painting/fill-rule"},
      {"a-fill", "tests/painting/fill"},
      {"a-filter", "tests/filters/filter-functions"},
      {"a-flood-color", "tests/filters/flood-color"},
      {"a-flood-opacity", "tests/filters/flood-opacity"},
      {"a-font-family", "tests/text/font-family"},
      {"a-font-kerning", "tests/text/font-kerning"},
      {"a-font-size-adjust", "tests/text/font-size-adjust"},
      {"a-font-size", "tests/text/font-size"},
      {"a-font-stretch", "tests/text/font-stretch"},
      {"a-font-style", "tests/text/font-style"},
      {"a-font-variant", "tests/text/font-variant"},
      {"a-font-weight", "tests/text/font-weight"},
      {"a-font", "tests/text/font"},
      {"a-glyph-orientation-horizontal", "tests/text/glyph-orientation-horizontal"},
      {"a-glyph-orientation-vertical", "tests/text/glyph-orientation-vertical"},
      {"a-image-rendering", "tests/painting/image-rendering"},
      {"a-isolation", "tests/painting/isolation"},
      {"a-kerning", "tests/text/kerning"},
      {"a-lengthAdjust", "tests/text/lengthAdjust"},
      {"a-letter-spacing", "tests/text/letter-spacing"},
      {"a-marker-end", "tests/painting/marker-end"},
      {"a-marker-mid", "tests/painting/marker-mid"},
      {"a-marker-start", "tests/painting/marker-start"},
      {"a-marker", "tests/painting/marker"},
      {"a-mix-blend-mode", "tests/painting/mix-blend-mode"},
      {"a-opacity", "tests/painting/opacity"},
      {"a-overflow", "tests/painting/overflow"},
      {"a-paint-order", "tests/painting/paint-order"},
      {"a-shape-rendering", "tests/painting/shape-rendering"},
      {"a-stop-color", "tests/paint-servers/stop-color"},
      {"a-stop-opacity", "tests/paint-servers/stop-opacity"},
      {"a-stroke-dasharray", "tests/painting/stroke-dasharray"},
      {"a-stroke-dashoffset", "tests/painting/stroke-dashoffset"},
      {"a-stroke-linecap", "tests/painting/stroke-linecap"},
      {"a-stroke-linejoin", "tests/painting/stroke-linejoin"},
      {"a-stroke-miterlimit", "tests/painting/stroke-miterlimit"},
      {"a-stroke-opacity", "tests/painting/stroke-opacity"},
      {"a-stroke-width", "tests/painting/stroke-width"},
      {"a-stroke", "tests/painting/stroke"},
      {"a-style", "tests/structure/style-attribute"},
      {"a-systemLanguage", "tests/structure/systemLanguage"},
      {"a-text-anchor", "tests/text/text-anchor"},
      {"a-text-decoration", "tests/text/text-decoration"},
      {"a-text-rendering", "tests/text/text-rendering"},
      {"a-textLength", "tests/text/textLength"},
      {"a-transform-origin", "tests/structure/transform-origin"},
      {"a-transform", "tests/structure/transform"},
      {"a-unicode-bidi", "tests/text/unicode-bidi"},
      {"a-visibility", "tests/painting/visibility"},
      {"a-word-spacing", "tests/text/word-spacing"},
      {"a-writing-mode", "tests/text/writing-mode"},
      // Element tests (e-*)
      {"e-a", "tests/structure/a"},
      {"e-circle", "tests/shapes/circle"},
      {"e-clipPath", "tests/masking/clipPath"},
      {"e-defs", "tests/structure/defs"},
      {"e-ellipse", "tests/shapes/ellipse"},
      {"e-feBlend", "tests/filters/feBlend"},
      {"e-feColorMatrix", "tests/filters/feColorMatrix"},
      {"e-feComponentTransfer", "tests/filters/feComponentTransfer"},
      {"e-feComposite", "tests/filters/feComposite"},
      {"e-feConvolveMatrix", "tests/filters/feConvolveMatrix"},
      {"e-feDiffuseLighting", "tests/filters/feDiffuseLighting"},
      {"e-feDisplacementMap", "tests/filters/feDisplacementMap"},
      {"e-feDistantLight", "tests/filters/feDistantLight"},
      {"e-feDropShadow", "tests/filters/feDropShadow"},
      {"e-feFlood", "tests/filters/feFlood"},
      {"e-feGaussianBlur", "tests/filters/feGaussianBlur"},
      {"e-feImage", "tests/filters/feImage"},
      {"e-feMerge", "tests/filters/feMerge"},
      {"e-feMorphology", "tests/filters/feMorphology"},
      {"e-feOffset", "tests/filters/feOffset"},
      {"e-fePointLight", "tests/filters/fePointLight"},
      {"e-feSpecularLighting", "tests/filters/feSpecularLighting"},
      {"e-feSpotLight", "tests/filters/feSpotLight"},
      {"e-feTile", "tests/filters/feTile"},
      {"e-feTurbulence", "tests/filters/feTurbulence"},
      {"e-filter", "tests/filters/filter"},
      {"e-g", "tests/structure/g"},
      {"e-image", "tests/structure/image"},
      {"e-line", "tests/shapes/line"},
      {"e-linearGradient", "tests/paint-servers/linearGradient"},
      {"e-marker", "tests/painting/marker"},
      {"e-mask", "tests/masking/mask"},
      {"e-path", "tests/shapes/path"},
      {"e-pattern", "tests/paint-servers/pattern"},
      {"e-polygon", "tests/shapes/polygon"},
      {"e-polyline", "tests/shapes/polyline"},
      {"e-radialGradient", "tests/paint-servers/radialGradient"},
      {"e-rect", "tests/shapes/rect"},
      {"e-stop", "tests/paint-servers/stop"},
      {"e-style", "tests/structure/style"},
      {"e-svg", "tests/structure/svg"},
      {"e-switch", "tests/structure/switch"},
      {"e-symbol", "tests/structure/symbol"},
      {"e-text", "tests/text/text"},
      {"e-textPath", "tests/text/textPath"},
      {"e-tref", "tests/text/tref"},
      {"e-tspan", "tests/text/tspan"},
      {"e-use", "tests/structure/use"},
  };

  auto it = kPrefixMap.find(prefix);
  if (it != kPrefixMap.end()) {
    return it->second;
  }
  return "";
}

std::vector<ImageComparisonTestcase> getTestsWithPrefix(
    const char* prefix, std::map<std::string, ImageComparisonParams> overrides = {},
    ImageComparisonParams defaultParams = {}) {
  const std::string testDir = getPrefixDirectory(prefix);
  if (testDir.empty()) {
    // Prefix not found in mapping, return empty vector
    return {};
  }

  const std::string kTestsRoot =
      Runfiles::instance().RlocationExternal("resvg-test-suite", testDir);

  // Copy into a vector and sort the tests.
  std::vector<ImageComparisonTestcase> testPlan;
  
  // Check if directory exists
  if (!std::filesystem::exists(kTestsRoot)) {
    return {};
  }

  for (const auto& entry : std::filesystem::directory_iterator(kTestsRoot)) {
    if (entry.path().extension() == ".svg") {
      const std::string& filename = entry.path().filename().string();
      
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

  std::filesystem::path goldenFilename;
  if (testcase.params.overrideGoldenFilename.empty()) {
    // In the new structure, PNG files are in the same directory as SVG files
    goldenFilename = testcase.svgFilename;
    goldenFilename.replace_extension(".png");
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
                                                     {})),
                         TestNameFromFilename);

// TODO: a-direction

INSTANTIATE_TEST_SUITE_P(
    Display, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("a-display",  //
                                {
                                    {"none-on-tspan-1.svg", Params::Skip()},  // Not impl: <tspan>
                                    {"none-on-tref.svg", Params::Skip()},  // Not impl: <tref>
                                    {"none-on-tspan-2.svg", Params::Skip()},  // Not impl: <tspan>
                                })),
    TestNameFromFilename);

// TODO: a-dominant-baseline
// TODO: a-enable-background

INSTANTIATE_TEST_SUITE_P(
    Fill, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-fill",  //
        {
            {"rgb-int-int-int.svg", Params::Skip()},          // UB: rgb(int int int)
            {"icc-color.svg", Params::Skip()},          // UB: ICC color
            {"valid-FuncIRI-with-a-fallback-ICC-color.svg", Params::Skip()},          // Not impl: Fallback with icc-color
            {"linear-gradient-on-text.svg", Params::Skip()},          // Not impl: <text>
            {"radial-gradient-on-text.svg", Params::Skip()},          // Not impl: <text>
            {"pattern-on-text.svg", Params::Skip()},          // Not impl: <pattern>, <text>
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    FillOpacity, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-fill-opacity",  //
        {
            {"with-pattern.svg", Params::Skip()},  // Not impl: `fill-opacity` affects pattern
            {"on-text.svg", Params::Skip()},  // Not impl: <text>
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
            {"50percent.svg",
             Params::Skip()},  // Changed in css-color-4 to allow percentage in <alpha-value>, see
                               // https://www.w3.org/TR/css-color/#transparency
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Overflow, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("a-overflow")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Shape, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("a-shape-rendering",
                                                     {
                                                         {"optimizeSpeed-on-text.svg",
                                                          Params::Skip()},  // Not impl: <text>
                                                         {"path-with-marker.svg",
                                                          Params::Skip()},  // Not impl: <marker>
                                                     })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StopColor, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("a-stop-color")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StopOpacity, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("a-stop-opacity")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Stroke, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-stroke",
        {
            {"linear-gradient-on-text.svg", Params::Skip()},            // Not impl: <text>
            {"radial-gradient-on-text.svg", Params::Skip()},            // Not impl: <text>
            {"pattern-on-text.svg", Params::Skip()},            // Not impl: <text>
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StrokeDasharray, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-stroke-dasharray",
        {
            {"even-list-with-em.svg", Params::Skip()},  // Not impl: "font-size"? "em" units
                                                             // (font-size="20" not impl)
            {"negative-values.svg", Params::Skip()},  // UB (negative values)
            {"negative-sum.svg", Params::Skip()},  // UB (negative sum)
            {"multiple-subpaths.svg",
             Params::WithThreshold(0.13f)},  // Larger threshold due to anti-aliasing artifacts.
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StrokeDashoffset, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-stroke-dashoffset",
        {
            {"em-value.svg", Params::Skip()},  // Not impl: dashoffset "em" units
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StrokeLinejoin, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-stroke-linejoin",
        {
            {"miter-clip.svg",
             Params::Skip()},  // UB (SVG 2), no UA supports `miter-clip`
            {"arcs.svg", Params::Skip()},  // UB (SVG 2), no UA supports `arcs`
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StrokeOpacity, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-stroke-opacity",
        {
            {"with-pattern.svg",
             Params::Skip()},  // Not impl: <pattern> / stroke interaction
            {"on-text.svg", Params::Skip()},  // Not impl: <text>
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StrokeWidth, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-stroke-width",
        {
            {"negative.svg", Params::Skip()},    // UB: Nothing should be rendered
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Style, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "a-style",
        {
            {"non-presentational-attribute.svg",
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
            {"rotate-at-position.svg",
             Params::WithThreshold(0.05f)},  // Larger threshold due to anti-aliasing artifacts.
        })),
    TestNameFromFilename);

// TODO(text): a-unicode

INSTANTIATE_TEST_SUITE_P(
    Visibility, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("a-visibility",  //
                                {
                                    {"hidden-on-tspan.svg", Params::Skip()},  // Not impl: <tspan>
                                    {"collapse-on-tspan.svg", Params::Skip()},  // Not impl: <tspan>
                                    {"bBox-impact-3.svg", Params::Skip()},  // Not impl: <text>
                                })),
    TestNameFromFilename);

// TODO(text): a-word-spacing
// TODO(text): a-writing-mode

// TODO: e-a-

INSTANTIATE_TEST_SUITE_P(Circle, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-circle")), TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(ClipPath, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix(
                             "e-clipPath",
                             {
                                 {"clip-path-with-transform-on-text.svg", Params::Skip()},  // Not impl: <text>
                                 {"clipping-with-text.svg", Params::Skip()},  // Not impl: <text>
                                 {"clipping-with-complex-text-1.svg", Params::Skip()},  // Not impl: <text>
                                 {"clipping-with-complex-text-2.svg", Params::Skip()},  // Not impl: <text>
                                 {"clipping-with-complex-text-and-clip-rule.svg", Params::Skip()},  // Not impl: <text>
                                 {"on-the-root-svg-without-size.svg",
                                  Params::Skip()},  // UB: on root `<svg>` without size
                                 {"with-use-child.svg", Params::Skip()},  // Not impl: <use> child
                                 {"switch-is-not-a-valid-child.svg", Params::Skip()},  // Not impl: <switch>
                             })),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Defs, ImageComparisonTestFixture,
                         ValuesIn(getTestsWithPrefix("e-defs",
                                                     {
                                                         {"style-inheritance-on-text.svg",
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
                                    {"external-svg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"external-svgz.svg", Params::Skip()},  // Not impl: .svgz image
                                    {"embedded-svg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"embedded-svgz.svg", Params::Skip()},  // Not impl: .svg image
                                    {"external-svg-with-transform.svg", Params::Skip()},  // Not impl: .svg image
                                    {"preserveAspectRatio=none-on-svg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"preserveAspectRatio=xMinYMin-meet-on-svg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"preserveAspectRatio=xMidYMid-meet-on-svg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"preserveAspectRatio=xMaxYMax-meet-on-svg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"preserveAspectRatio=xMinYMin-slice-on-svg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"preserveAspectRatio=xMidYMid-slice-on-svg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"preserveAspectRatio=xMaxYMax-slice-on-svg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"embedded-svg-with-text.svg", Params::Skip()},  // Not impl: .svg image
                                    {"embedded-jpeg-as-image-jpeg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"embedded-jpeg-as-image-jpg.svg", Params::Skip()},  // Not impl: .svg image
                                    {"float-size.svg", Params::Skip()},  // UB: Float size
                                    {"embedded-png.svg", Params::Skip()},  // Not impl: .svg image
                                    {"recursive-2.svg", Params::Skip()},  // Not impl: .svg image
                                    {"embedded-svg-without-mime.svg", Params::Skip()},  // Not impl: .svg image
                                    {"url-to-png.svg", Params::Skip()},  // Not impl: External URLs
                                    {"url-to-svg.svg", Params::Skip()},  // Not impl: External URLs
                                },
                                Params::WithThreshold(0.2f).disableDebugSkpOnFailure())),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Line, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-line",
        {
            {"simple-case.svg",
             Params::WithThreshold(0.02f)},  // Larger threshold due to anti-aliasing artifacts with
                                             // overlapping lines.
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    LinearGradient, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-linearGradient",
                                {
                                    {"invalid-gradientTransform.svg",
                                     Params::Skip()},  // UB: Invalid `gradientTransform`
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Marker, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-marker",
        {
            {"with-viewBox-1.svg", Params::Skip()},  // UB: with `viewBox`
            {"marker-on-text.svg", Params::Skip()},  // Not impl: `text`
            {"with-a-text-child.svg", Params::Skip()},  // Not impl: `text`
            {"embedded-svg.svg", Params::Skip()},  // Not impl: .svg image
            {"nested.svg", Params::Skip()},  // BUG: Nested
            {"target-with-subpaths-2.svg", Params::Skip()},  // UB: Target with subpaths
            {"orient=auto-on-M-L-L-Z-Z-Z.svg", Params::Skip()},  // BUG: Multiple closepaths (M L L Z Z Z)
            // Resvg bug? Direction to place markers at the beginning/end of closed shapes.
            {"orient=auto-on-M-L-Z.svg", Params::WithGoldenOverride(
                                     "donner/svg/renderer/testdata/golden/resvg-e-marker-045.png")},
            // BUG? Disagreement about marker direction on cusp
            {"orient=auto-on-M-C-C-4.svg", Params::WithGoldenOverride(
                                     "donner/svg/renderer/testdata/golden/resvg-e-marker-051.png")},
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Mask, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-mask",
        {
            {"color-interpolation=linearRGB.svg", Params::Skip()},  // Not impl: color-interpolation
            {"recursive-on-child.svg", Params::Skip()},  // UB: Recursive on child
            {"recursive-on-self.svg", Params::Skip()},  // BUG: Rendering issue, mask is clipped. Repros in
                                                 // renderer_tool but not viewer.
            {"mask-on-self.svg", Params::Skip()},  // BUG: Mask on self, also a bug in browsers
            {"mask-on-child.svg", Params::Skip()},  // BUG: Mask on child doesn't apply
            {"with-image.svg", Params::Skip()},  // BUG: Crashes on serializing the skp
            {"with-grayscale-image.svg", Params::Skip()},  // BUG: Crashes on serializing the skp
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Path, ImageComparisonTestFixture, ValuesIn(getTestsWithPrefix("e-path")),
                         TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Pattern, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-pattern",
        {
            {"overflow=visible.svg", Params::Skip()},  // UB: overflow=visible
            {"patternContentUnits=objectBoundingBox.svg",
             Params::WithThreshold(kDefaultThreshold, 250)},  // Anti-aliasing artifacts
            {"patternContentUnits-with-viewBox.svg",
             Params::WithThreshold(kDefaultThreshold, 150)},          // Anti-aliasing artifacts
            {"text-child.svg", Params::Skip()},                    // Not impl: <text>
            {"pattern-on-child.svg", Params::WithThreshold(0.2f)},       // Anti-aliasing artifacts
            {"out-of-order-referencing.svg", Params::WithThreshold(0.6f, 300)},  // Anti-aliasing artifacts
            {"recursive-on-child.svg", Params::WithThreshold(0.2f)},       // Anti-aliasing artifacts
            {"self-recursive.svg", Params::WithThreshold(0.2f)},       // Anti-aliasing artifacts
            {"self-recursive-on-child.svg", Params::WithThreshold(0.2f)},       // Anti-aliasing artifacts
            {"invalid-patternTransform.svg", Params::Skip()},                // UB: Invalid patternTransform
            {"tiny-pattern-upscaled.svg", Params::WithThreshold(0.02f)},  // Has anti-aliasing artifacts.
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
            {"focal-point-correction.svg",
             Params::Skip()},  // Test suite bug? In SVG2 this was changed to draw
                               // conical gradient instead of correcting focal point.
            {"negative-r.svg", Params::Skip()},  // UB: Negative `r`
            {"invalid-gradientUnits.svg", Params::Skip()},  // UB: Invalid `gradientUnits`
            {"invalid-gradientTransform.svg", Params::Skip()},  // UB: Invalid `gradientTransform`
            {"fr=0.5.svg", Params::Skip()},  // UB: fr=0.5 (SVG 2)
            {"fr=0.7.svg", Params::Skip()},  // Test suite bug? fr > default value of
                                                           // r (0.5) should not
                                                           //  render.
            {"fr=-1.svg", Params::Skip()},  // UB: fr=-1 (SVG 2)
        })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Rect, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-rect",
                                {
                                    {"em-values.svg", Params::Skip()},  // Not impl: "em" units
                                    {"ex-values.svg", Params::Skip()},  // Not impl: "ex" units
                                    {"rem-values.svg", Params::Skip()},  // Not impl: "rem" units
                                    {"ch-values.svg", Params::Skip()},  // Not impl: "ch" units
                                    {"vw-and-vh-values.svg", Params::Skip()},  // Bug? vw/vh
                                    {"vmin-and-vmax-values.svg", Params::Skip()},  // Bug? vmin/vmax
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StopElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-stop",
                                {
                                    {"stop-color-with-inherit-1.svg",
                                     Params::Skip()},  // Bug? Strange edge case, stop-color
                                                       // inherited from <linearGradient>.
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StyleElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-style",
                                {
                                    {"non-presentational-attribute.svg",
                                     Params::Skip()},  // Not impl: <svg version="1.1">
                                    {"@import.svg", Params::Skip()},  // Not impl: CSS @import
                                })),
    TestNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    SvgElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix(
        "e-svg",
        {
            {"xmlns-validation.svg", Params::Skip()},                // Bug? xmlns validation
            {"mixed-namespaces.svg", Params::Skip()},                // Bug? mixed namespaces
            {"attribute-value-via-ENTITY-reference.svg", Params::Skip()},                // Bug/Not impl? XML Entity references
            {"not-UTF-8-encoding.svg", Params::Skip()},                // Bug/Not impl? Non-UTF8 encoding
            {"preserveAspectRatio=none.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"preserveAspectRatio=xMinYMin.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"preserveAspectRatio=xMidYMid.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"preserveAspectRatio=xMaxYMax.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"preserveAspectRatio=xMinYMin-slice.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"preserveAspectRatio=xMidYMid-slice.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"preserveAspectRatio=xMaxYMax-slice.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"preserveAspectRatio-with-viewBox-not-at-zero-pos.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"viewBox-not-at-zero-pos.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"proportional-viewBox.svg", Params::WithThreshold(0.13f)},  // Has anti-aliasing artifacts.
            {"invalid-id-attribute-1.svg", Params::Skip()},                // UB: Invalid id attribute
            {"invalid-id-attribute-2.svg", Params::Skip()},                // UB: Invalid id attribute
            {"funcIRI-parsing.svg", Params::Skip()},                // UB: FuncIRI parsing
            {"funcIRI-with-invalid-characters.svg", Params::Skip()},                // UB: FuncIRI with invalid chars
            {"nested-svg-with-overflow-visible.svg", Params::Skip()},                // Not impl: overflow
            {"nested-svg-with-overflow-auto.svg", Params::Skip()},                // Not impl: overflow
            {"elements-via-ENTITY-reference-2.svg", Params::Skip()},                // Bug/Not impl? XML Entity references
            {"elements-via-ENTITY-reference-3.svg", Params::Skip()},                // Bug/Not impl? XML Entity references
            {"rect-inside-a-non-svg-element.svg", Params::Skip()},                // Bug? Rect inside unknown element
            {"no-size.svg", Params::Skip()},  // Not impl: Computed bounds from content

        })),
    TestNameFromFilename);

// TODO: e-switch

INSTANTIATE_TEST_SUITE_P(
    SymbolElement, ImageComparisonTestFixture,
    ValuesIn(getTestsWithPrefix("e-symbol",
                                {

                                    {"with-transform.svg",
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
                                    {"external-file.svg", Params::Skip()},  // Not impl: External file.
                                })),
    TestNameFromFilename);

}  // namespace donner::svg
