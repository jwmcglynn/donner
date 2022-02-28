#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <pixelmatch/pixelmatch.h>

#include <filesystem>
#include <fstream>

#include "src/svg/renderer/renderer_skia.h"
#include "src/svg/renderer/renderer_utils.h"
#include "src/svg/renderer/tests/renderer_test_utils.h"
#include "src/svg/xml/xml_parser.h"

using testing::ValuesIn;

namespace donner::svg {

namespace {

// Circle rendering is slightly different since Donner uses four custom curves instead of arcTo.
// Allow a small number of mismatched pixels to accomodate.
static constexpr size_t kMaxMismatchedPixels = 100;

static const std::filesystem::path kSvgDir = "external/resvg-test-suite/svg/";
static const std::filesystem::path kGoldenDir = "external/resvg-test-suite/png/";

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

std::string testNameFromFilename(const testing::TestParamInfo<std::filesystem::path>& info) {
  std::string name = info.param.stem().string();

  // Sanitize the test name, notably replacing '-' with '_'.
  std::transform(name.begin(), name.end(), name.begin(), [](char c) {
    if (!isalnum(c)) {
      return '_';
    } else {
      return c;
    }
  });

  return name;
}

std::vector<std::filesystem::path> getTestsWithPrefix(const char* prefix,
                                                      std::vector<std::string> exclude = {}) {
  // Copy into a vector and sort the tests.
  std::vector<std::filesystem::path> testPlan;
  for (const auto& entry : std::filesystem::directory_iterator(kSvgDir)) {
    const std::string& filename = entry.path().filename().string();
    if (filename.find(prefix) == 0) {
      // Skip excluded paths.
      if (std::find(exclude.begin(), exclude.end(), filename) != exclude.end()) {
        continue;
      }

      testPlan.push_back(entry.path());
    }
  }

  std::sort(testPlan.begin(), testPlan.end());
  return testPlan;
}

}  // namespace

class ResvgTestSuite : public testing::TestWithParam<std::filesystem::path> {
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

    // The size provided here specifies the default size, in most cases this is overridden by the
    // SVG.
    RendererSkia renderer(500, 500);
    renderer.overrideSize();
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

    pixelmatch::Options options;
    // For most tests, a tolerance of 0.01 is sufficient, but some specific tests have slightly
    // different anti-aliasing artifacts, so a larger tolerance is required:
    // - a_transform_007 - 0.05 to pass
    // - e_line_001 - 0.02 to pass
    options.threshold = 0.04;
    const int mismatchedPixels = pixelmatch::pixelmatch(
        goldenImage.data, renderer.pixelData(), diffImage, width, height, strideInPixels, options);

    if (mismatchedPixels > kMaxMismatchedPixels) {
      std::cout << "FAIL (" << mismatchedPixels << " pixels differ)" << std::endl;

      const std::filesystem::path actualImagePath =
          std::filesystem::temp_directory_path() / escapeFilename(goldenImageFilename);
      std::cout << "Actual rendering: " << actualImagePath.string() << std::endl;
      RendererUtils::writeRgbaPixelsToPngFile(actualImagePath.string().c_str(),
                                              renderer.pixelData(), width, height, strideInPixels);

      const std::filesystem::path diffFilePath =
          std::filesystem::temp_directory_path() / ("diff_" + escapeFilename(goldenImageFilename));
      std::cerr << "Diff: " << diffFilePath.string() << std::endl;

      RendererUtils::writeRgbaPixelsToPngFile(diffFilePath.string().c_str(), diffImage, width,
                                              height, strideInPixels);

      std::cout << "Expected: " << goldenImageFilename << std::endl;
      FAIL() << mismatchedPixels << " pixels different.";
    } else {
      std::cout << "PASS";
      if (mismatchedPixels != 0) {
        std::cout << " (" << mismatchedPixels << " pixels differ, within thresholds)";
      }
      std::cout << std::endl;
    }
  }
};

TEST_P(ResvgTestSuite, Compare) {
  const std::filesystem::path svgFilename = GetParam();
  const std::filesystem::path goldenFilename =
      kGoldenDir / svgFilename.filename().replace_extension(".png");

  SVGDocument document = loadSVG(svgFilename.string().c_str());
  renderAndCompare(document, svgFilename, goldenFilename.string().c_str());
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
            "a-fill-010.svg",          // UB
            "a-fill-015.svg",          // UB
            "a-fill-018.svg",          // Not impl: <pattern>
            "a-fill-027.svg",          // Not impl: Fallback with icc-color
            "a-fill-031.svg",          // Not impl: <text>
            "a-fill-032.svg",          // Not impl: <text>
            "a-fill-033.svg",          // Not impl: <pattern>, <text>
            "a-fill-opacity-002.svg",  // Not impl: "opacity"
            "a-fill-opacity-004.svg",  // Not impl: `fill-opacity` affects pattern
            "a-fill-opacity-006.svg",  // Not impl: <text>
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
// TODO(opacity): a-opacity
// TODO(text): a-overflow

INSTANTIATE_TEST_SUITE_P(
    Shape, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix("a-shape",
                                {
                                    "a-shape-rendering-005.svg",  // Not impl: <text>
                                    "a-shape-rendering-008.svg",  // Not impl: <marker>
                                })),
    testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(StopAttributes, ResvgTestSuite, ValuesIn(getTestsWithPrefix("a-stop")),
                         testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Stroke, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "a-stroke",
        {
            "a-stroke-004.svg",             // Not impl: <pattern>
            "a-stroke-007.svg",             // Not impl: <text>
            "a-stroke-008.svg",             // Not impl: <text>
            "a-stroke-009.svg",             // Not impl: <pattern>, <text>
            "a-stroke-012.svg",             // Not impl: <pattern>
            "a-stroke-013.svg",             // Not impl: <pattern>, "gradientUnits"
            "a-stroke-dasharray-005.svg",   // Not impl: "font-size"? "em" units (font-size="20" not
                                            // impl)
            "a-stroke-dasharray-007.svg",   // UB (negative values)
            "a-stroke-dasharray-009.svg",   // UB (negative sum)
            "a-stroke-dasharray-012.svg",   // Bug? Strange aliasing artifacts.
            "a-stroke-dasharray-013.svg",   // Bug? Dasharray should be reset on a new subpath
            "a-stroke-dashoffset-004.svg",  // Bug? dashoffset "em" units
            "a-stroke-linejoin-004.svg",    // UB (SVG 2), no UA supports `miter-clip`
            "a-stroke-linejoin-005.svg",    // UB (SVG 2), no UA supports `arcs`
            "a-stroke-opacity-002.svg",     // Not impl: "opacity"
            "a-stroke-opacity-004.svg",     // Not impl: <pattern>
            "a-stroke-opacity-006.svg",     // Not impl: <text>
            "a-stroke-width-004.svg",       // UB: Nothing should be renderered
        })),
    testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    Style, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "a-style",
        {
            "a-style-003.svg",  // <svg version="1.1"> disables geometry attributes in style
        })),
    testNameFromFilename);

// TODO: a-systemLanguage
// TODO(text): a-text

INSTANTIATE_TEST_SUITE_P(Transform, ResvgTestSuite, ValuesIn(getTestsWithPrefix("a-transform")),
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
                                                         "e-defs-007.svg",  // Not impl: <text>
                                                     })),
                         testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Ellipse, ResvgTestSuite, ValuesIn(getTestsWithPrefix("e-ellipse")),
                         testNameFromFilename);

// TODO(filter): e-fe
// TODO(filter) e-filter

INSTANTIATE_TEST_SUITE_P(G, ResvgTestSuite, ValuesIn(getTestsWithPrefix("e-g")),
                         testNameFromFilename);

// TODO: e-image

INSTANTIATE_TEST_SUITE_P(Line, ResvgTestSuite, ValuesIn(getTestsWithPrefix("e-line-")),
                         testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    LinearGradient, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix("e-linearGradient",
                                {
                                    "e-linearGradient-037.svg",  // UB: Invalid `gradientTransform`
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
            "e-radialGradient-031.svg",  // Test suite bug? In SVG2 this was changed to draw conical
                                         // gradient instead of correcting focal point.
            "e-radialGradient-032.svg",  // UB: Negative `r`
            "e-radialGradient-039.svg",  // UB: Invalid `gradientUnits`
            "e-radialGradient-040.svg",  // UB: Invalid `gradientTransform`
            "e-radialGradient-043.svg",  // UB: fr=0.5 (SVG 2)
            "e-radialGradient-044.svg",  // Test suite bug? fr > default value of r (0.5) should not
                                         //  render.
            "e-radialGradient-045.svg",  // UB: fr=-1 (SVG 2)
        })),
    testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(Rect, ResvgTestSuite,
                         ValuesIn(getTestsWithPrefix("e-rect",
                                                     {
                                                         "e-rect-022.svg",  // Not impl: "em" units
                                                         "e-rect-023.svg",  // Not impl: "ex" units
                                                         "e-rect-029.svg",  // Not impl: "rem" units
                                                         "e-rect-031.svg",  // Not impl: "ch" units
                                                         "e-rect-034.svg",  // Bug? vw/vh
                                                         "e-rect-036.svg",  // Bug? vmin/vmax
                                                     })),
                         testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StopElement, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix("e-stop",
                                {
                                    "e-stop-011.svg",  // Bug? Strange edge case, stop-color
                                                       // inherited from <linearGradient>.
                                })),
    testNameFromFilename);

INSTANTIATE_TEST_SUITE_P(
    StyleElement, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix("e-style",
                                {
                                    "e-style-004.svg",  // Not impl: Attribute matchers
                                    "e-style-012.svg",  // Not impl: <svg version="1.1">
                                    "e-style-013.svg",  // Bug? CSS transform
                                    "e-style-014.svg",  // Not impl: CSS @import
                                })),
    testNameFromFilename);

// TODO: e-svg
// TODO: e-switch
// TODO: e-symbol
// TODO(text): e-text-
// TODO(text): e-textPath
// TODO(text): e-tspan

INSTANTIATE_TEST_SUITE_P(
    Use, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix("e-use",
                                {
                                    "e-use-008.svg",  // Not impl: External file.
                                    "e-use-015.svg",  // Not impl: opacity attribute.
                                    "e-use-019.svg",  // Not impl: display attribute.
                                    "e-use-025.svg",  // Not impl: opacity attribute.
                                    "e-use-026.svg",  // Not impl: opacity attribute.
                                })),
    testNameFromFilename);

}  // namespace donner::svg
