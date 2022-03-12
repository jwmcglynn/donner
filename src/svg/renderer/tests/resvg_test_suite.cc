#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <pixelmatch/pixelmatch.h>

#include <filesystem>
#include <fstream>
#include <map>

#include "src/svg/renderer/renderer_skia.h"
#include "src/svg/renderer/renderer_utils.h"
#include "src/svg/renderer/tests/renderer_test_utils.h"
#include "src/svg/xml/xml_parser.h"

using testing::ValuesIn;

namespace donner::svg {

namespace {

// Circle rendering is slightly different since Donner uses four custom curves instead of arcTo.
// Allow a small number of mismatched pixels to accomodate.
static constexpr int kDefaultMismatchedPixels = 100;

// For most tests, a threshold of 0.01 is sufficient, but some specific tests have slightly
// different anti-aliasing artifacts, so a larger threshold is required:
// - a_transform_007 - 0.05 to pass
// - e_line_001 - 0.02 to pass
static constexpr float kDefaultThreshold = 0.01f;

static const std::filesystem::path kSvgDir = "external/resvg-test-suite/svg/";
static const std::filesystem::path kGoldenDir = "external/resvg-test-suite/png/";

struct Params {
  float threshold = kDefaultThreshold;
  int maxMismatchedPixels = kDefaultMismatchedPixels;
  bool skip = false;

  static Params Skip() {
    Params result;
    result.skip = true;
    return result;
  }

  static Params WithThreshold(float threshold, int maxMismatchedPixels = kDefaultMismatchedPixels) {
    Params result;
    result.threshold = threshold;
    result.maxMismatchedPixels = maxMismatchedPixels;
    return result;
  }
};

struct ResvgTestcase {
  std::filesystem::path svgFilename;
  Params params;

  friend bool operator<(const ResvgTestcase& lhs, const ResvgTestcase& rhs) {
    return lhs.svgFilename < rhs.svgFilename;
  }

  friend std::ostream& operator<<(std::ostream& os, const ResvgTestcase& rhs) {
    return os << rhs.svgFilename.string();
  }
};

std::string escapeFilename(std::string filename) {
  std::transform(filename.begin(), filename.end(), filename.begin(), [](char c) {
    if (c == '\\' || c == '/') {
      return '_';
    } else {
      return c;
    }
  });
  return filename;
}

std::string testNameFromFilename(const testing::TestParamInfo<ResvgTestcase>& info) {
  std::string name = info.param.svgFilename.stem().string();

  // Sanitize the test name, notably replacing '-' with '_'.
  std::transform(name.begin(), name.end(), name.begin(), [](char c) {
    if (!isalnum(c)) {
      return '_';
    } else {
      return c;
    }
  });

  if (info.param.params.skip) {
    return "DISABLED_" + name;
  } else {
    return name;
  }
}

std::vector<ResvgTestcase> getTestsWithPrefix(const char* prefix,
                                              std::map<std::string, Params> overrides = {}) {
  // Copy into a vector and sort the tests.
  std::vector<ResvgTestcase> testPlan;
  for (const auto& entry : std::filesystem::directory_iterator(kSvgDir)) {
    const std::string& filename = entry.path().filename().string();
    if (filename.find(prefix) == 0) {
      ResvgTestcase test;
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

class ResvgTestSuite : public testing::TestWithParam<ResvgTestcase> {
protected:
  SVGDocument loadSVG(const char* filename) {
    std::ifstream file(filename);
    EXPECT_TRUE(file) << "Failed to open file: " << filename;
    if (!file) {
      return SVGDocument();
    }

    file.seekg(0, std::ios::end);
    const size_t fileLength = file.tellg();
    file.seekg(0);

    std::vector<char> fileData(fileLength + 1);
    file.read(fileData.data(), fileLength);

    auto maybeResult = XMLParser::ParseSVG(fileData);
    EXPECT_FALSE(maybeResult.hasError())
        << "Parse Error: " << maybeResult.error().line << ":" << maybeResult.error().offset << ": "
        << maybeResult.error().reason;
    if (maybeResult.hasError()) {
      return SVGDocument();
    }

    return std::move(maybeResult.result());
  }

  void renderAndCompare(SVGDocument& document, const std::filesystem::path& svgFilename,
                        const char* goldenImageFilename) {
    std::cout << "[  COMPARE ] " << svgFilename.string()
              << ": ";  // No endl yet, the line will be continued

    // The canvas size to draw into, as a recommendation instead of a strict guideline, since some
    // SVGs may override.
    // TODO: Add a flag to disable this behavior.
    document.setCanvasSize(500, 500);

    RendererSkia renderer;
    renderer.draw(document);

    const size_t strideInPixels = renderer.width();
    const int width = renderer.width();
    const int height = renderer.height();

    auto maybeGoldenImage = RendererTestUtils::readRgbaImageFromPngFile(goldenImageFilename);
    ASSERT_TRUE(maybeGoldenImage.has_value());

    Image goldenImage = std::move(maybeGoldenImage.value());
    ASSERT_EQ(goldenImage.width, width);
    ASSERT_EQ(goldenImage.height, height);
    ASSERT_EQ(goldenImage.strideInPixels, strideInPixels);
    ASSERT_EQ(goldenImage.data.size(), renderer.pixelData().size());

    std::vector<uint8_t> diffImage;
    diffImage.resize(strideInPixels * height * 4);

    const Params params = GetParam().params;

    pixelmatch::Options options;
    options.threshold = params.threshold;
    const int mismatchedPixels = pixelmatch::pixelmatch(
        goldenImage.data, renderer.pixelData(), diffImage, width, height, strideInPixels, options);

    if (mismatchedPixels > params.maxMismatchedPixels) {
      std::cout << "FAIL (" << mismatchedPixels << " pixels differ, with "
                << params.maxMismatchedPixels << " max)" << std::endl;

      const std::filesystem::path actualImagePath =
          std::filesystem::temp_directory_path() / escapeFilename(goldenImageFilename);
      std::cout << "Actual rendering: " << actualImagePath.string() << std::endl;
      RendererUtils::writeRgbaPixelsToPngFile(actualImagePath.string().c_str(),
                                              renderer.pixelData(), width, height, strideInPixels);

      std::cout << "Expected: " << goldenImageFilename << std::endl;

      const std::filesystem::path diffFilePath =
          std::filesystem::temp_directory_path() / ("diff_" + escapeFilename(goldenImageFilename));
      std::cerr << "Diff: " << diffFilePath.string() << std::endl;

      RendererUtils::writeRgbaPixelsToPngFile(diffFilePath.string().c_str(), diffImage, width,
                                              height, strideInPixels);

      FAIL() << mismatchedPixels << " pixels different.";
    } else {
      std::cout << "PASS";
      if (mismatchedPixels != 0) {
        std::cout << " (" << mismatchedPixels << " pixels differ, out of "
                  << params.maxMismatchedPixels << " max)";
      }
      std::cout << std::endl;
    }
  }
};

TEST_P(ResvgTestSuite, Compare) {
  const ResvgTestcase& testcase = GetParam();

  const std::filesystem::path goldenFilename =
      kGoldenDir / testcase.svgFilename.filename().replace_extension(".png");

  SVGDocument document = loadSVG(testcase.svgFilename.string().c_str());
  renderAndCompare(document, testcase.svgFilename, goldenFilename.string().c_str());
}

// TODO(text): a-alignment-baseline
// TODO(text): a-baseline-shift
// TODO: a-clip
// TODO: a-color
// TODO: a-direction
// TODO: a-display
// TODO: a-dominant-baseline
// TODO: a-enable-background

INSTANTIATE_TEST_SUITE_P(
    Fill, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "a-fill",  //
        {
            {"a-fill-010.svg", Params::Skip()},          // UB: rgb(int int int)
            {"a-fill-015.svg", Params::Skip()},          // UB: ICC color
            {"a-fill-018.svg", Params::Skip()},          // Not impl: <pattern>
            {"a-fill-027.svg", Params::Skip()},          // Not impl: Fallback with icc-color
            {"a-fill-031.svg", Params::Skip()},          // Not impl: <text>
            {"a-fill-032.svg", Params::Skip()},          // Not impl: <text>
            {"a-fill-033.svg", Params::Skip()},          // Not impl: <pattern>, <text>
            {"a-fill-opacity-004.svg", Params::Skip()},  // Not impl: `fill-opacity` affects pattern
            {"a-fill-opacity-006.svg", Params::Skip()},  // Not impl: <text>
        })),
    testNameFromFilename);

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
    Opacity, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "a-opacity",
        {
            {"a-opacity-002.svg", Params::Skip()},  // Not impl: <clipPath>
            {"a-opacity-005.svg",
             Params::Skip()},  // Changed in css-color-4 to allow percentage in <alpha-value>, see
                               // https://www.w3.org/TR/css-color/#transparency
        })),
    testNameFromFilename);

// TODO(text): a-overflow

INSTANTIATE_TEST_SUITE_P(Shape, ResvgTestSuite,
                         ValuesIn(getTestsWithPrefix("a-shape",
                                                     {
                                                         {"a-shape-rendering-005.svg",
                                                          Params::Skip()},  // Not impl: <text>
                                                         {"a-shape-rendering-008.svg",
                                                          Params::Skip()},  // Not impl: <marker>
                                                     })),
                         testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StopAttributes, ResvgTestSuite, ValuesIn(getTestsWithPrefix("a-stop")),
                         testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Stroke, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "a-stroke",
        {
            {"a-stroke-004.svg", Params::Skip()},            // Not impl: <pattern>
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
            {"a-stroke-width-004.svg", Params::Skip()},     // UB: Nothing should be renderered
        })),
    testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Style, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "a-style",
        {
            {"a-style-003.svg",
             Params::Skip()},  // <svg version="1.1"> disables geometry attributes in style
        })),
    testNameFromFilename);

// TODO: a-systemLanguage
// TODO(text): a-text

INSTANTIATE_TEST_SUITE_P(
    Transform, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "a-transform",
        {
            {"a-transform-007.svg",
             Params::WithThreshold(0.05f)},  // Larger threshold due to anti-aliasing artifacts.
        })),
    testNameFromFilename);

// TODO(text): a-unicode
// TODO: a-visibility
// TODO(text): a-word-spacing
// TODO(text): a-writing-mode

// TODO: e-a-

INSTANTIATE_TEST_SUITE_P(Circle, ResvgTestSuite, ValuesIn(getTestsWithPrefix("e-circle")),
                         testNameFromFilename);

// TODO(clip): e-clipPath

INSTANTIATE_TEST_SUITE_P(Defs, ResvgTestSuite,
                         ValuesIn(getTestsWithPrefix("e-defs",
                                                     {
                                                         {"e-defs-007.svg",
                                                          Params::Skip()},  // Not impl: <text>
                                                     })),
                         testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Ellipse, ResvgTestSuite, ValuesIn(getTestsWithPrefix("e-ellipse")),
                         testNameFromFilename);

// TODO(filter): e-fe
// TODO(filter): e-filter

INSTANTIATE_TEST_SUITE_P(G, ResvgTestSuite, ValuesIn(getTestsWithPrefix("e-g")),
                         testNameFromFilename);

// TODO: e-image

INSTANTIATE_TEST_SUITE_P(
    Line, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "e-line-",
        {
            {"e-line-001.svg",
             Params::WithThreshold(0.02f)},  // Larger threshold due to anti-aliasing artifacts with
                                             // overlapping lines.
        })),
    testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    LinearGradient, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix("e-linearGradient",
                                {
                                    {"e-linearGradient-037.svg",
                                     Params::Skip()},  // UB: Invalid `gradientTransform`
                                })),
    testNameFromFilename);

// TODO: e-marker
// TODO: e-mask

INSTANTIATE_TEST_SUITE_P(Path, ResvgTestSuite, ValuesIn(getTestsWithPrefix("e-path")),
                         testNameFromFilename);

// TODO: e-pattern

INSTANTIATE_TEST_SUITE_P(Polygon, ResvgTestSuite, ValuesIn(getTestsWithPrefix("e-polygon")),
                         testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Polyline, ResvgTestSuite, ValuesIn(getTestsWithPrefix("e-polyline")),
                         testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    RadialGradient, ResvgTestSuite,
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
    testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Rect, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix("e-rect",
                                {
                                    {"e-rect-022.svg", Params::Skip()},  // Not impl: "em" units
                                    {"e-rect-023.svg", Params::Skip()},  // Not impl: "ex" units
                                    {"e-rect-029.svg", Params::Skip()},  // Not impl: "rem" units
                                    {"e-rect-031.svg", Params::Skip()},  // Not impl: "ch" units
                                    {"e-rect-034.svg", Params::Skip()},  // Bug? vw/vh
                                    {"e-rect-036.svg", Params::Skip()},  // Bug? vmin/vmax
                                })),
    testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StopElement, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix("e-stop",
                                {
                                    {"e-stop-011.svg",
                                     Params::Skip()},  // Bug? Strange edge case, stop-color
                                                       // inherited from <linearGradient>.
                                })),
    testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StyleElement, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "e-style",
        {
            {"e-style-004.svg", Params::Skip()},  // Not impl: Attribute matchers
            {"e-style-012.svg", Params::Skip()},  // Not impl: <svg version="1.1">
            {"e-style-014.svg", Params::Skip()},  // Not impl: CSS @import
        })),
    testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    SvgElement, ResvgTestSuite,
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
    testNameFromFilename);

// TODO: e-switch
// TODO: e-symbol
// TODO(text): e-text-
// TODO(text): e-textPath
// TODO(text): e-tspan

INSTANTIATE_TEST_SUITE_P(Use, ResvgTestSuite,
                         ValuesIn(getTestsWithPrefix(
                             "e-use",
                             {
                                 {"e-use-008.svg", Params::Skip()},  // Not impl: External file.
                                 {"e-use-019.svg", Params::Skip()},  // Not impl: display attribute.
                             })),
                         testNameFromFilename);

}  // namespace donner::svg
