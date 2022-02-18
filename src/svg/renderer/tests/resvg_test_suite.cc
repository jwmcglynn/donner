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
    options.threshold = 0.01;
    const int mismatchedPixels = pixelmatch::pixelmatch(
        goldenImage.data, renderer.pixelData(), diffImage, width, height, strideInPixels, options);

    if (mismatchedPixels != 0) {
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
      std::cout << "PASS" << std::endl;
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

INSTANTIATE_TEST_SUITE_P(
    Fill, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "a-fill",  //
        {
            "a-fill-010.svg",          // UB
            "a-fill-015.svg",          // UB
            "a-fill-016.svg",          // Not impl: <linearGradient>
            "a-fill-017.svg",          // Not impl: <radialGradient>
            "a-fill-018.svg",          // Not impl: <pattern>
            "a-fill-024.svg",          // Not impl: <linearGradient>
            "a-fill-025.svg",          // Not impl: <linearGradient>
            "a-fill-026.svg",          // Not impl: Paint reference fallback
            "a-fill-027.svg",          // Not impl: Paint reference fallback
            "a-fill-031.svg",          // Not impl: <linearGradient>
            "a-fill-032.svg",          // Not impl: <radialGradient>
            "a-fill-033.svg",          // Not impl: <pattern>
            "a-fill-034.svg",          // Not impl: Paint reference fallback
            "a-fill-035.svg",          // Not impl: Paint reference fallback
            "a-fill-042.svg",          // "transparent" color (SVG 2)
            "a-fill-044.svg",          // rgba(0, 127, 0, 0.5) (SVG 2)
            "a-fill-045.svg",          // rgba(0, 127, 0, 0) (SVG 2)
            "a-fill-046.svg",          // rgba(0, 127, 0, -1) (SVG 2)
            "a-fill-048.svg",          // rgba(0, 127, 0, 50%) (SVG 2)
            "a-fill-049.svg",          // rgba(0%, 50%, 0%, 0.5) (SVG 2)
            "a-fill-051.svg",          // #RRGGBBAA (SVG 2)
            "a-fill-052.svg",          // #RGBA (SVG 2)
            "a-fill-057.svg",          // hsla(120, 100%, 25%, 0.5) (SVG 2)
            "a-fill-058.svg",          // hsl(120, 100%, 25%, 0.5) (SVG 2)
            "a-fill-059.svg",          // `rgb(0, 127, 0, 0.5)` (SVG 2) (Technically
                                       // against spec, but UAs support it)
            "a-fill-opacity-002.svg",  // Not impl: "opacity"
            "a-fill-opacity-003.svg",  // Not impl: `fill-opacity`, <linearGradient>
            "a-fill-opacity-004.svg",  // Not impl: `fill-opacity` affects pattern
            "a-fill-opacity-006.svg",  // Not impl: <text>
        })));
INSTANTIATE_TEST_SUITE_P(
    Stroke, ResvgTestSuite,
    ValuesIn(getTestsWithPrefix(
        "a-stroke",
        {
            "a-stroke-002.svg",             // Not impl: <linearGradient>
            "a-stroke-003.svg",             // Not impl: <radialGradient>
            "a-stroke-004.svg",             // Not impl: <pattern>
            "a-stroke-007.svg",             // Not impl: <linearGradient>
            "a-stroke-008.svg",             // Not impl: <radialGradient>
            "a-stroke-009.svg",             // Not impl: <pattern>, <text>
            "a-stroke-011.svg",             // Not impl: Gradients, "gradientUnits" property
            "a-stroke-012.svg",             // Not imple: <pattern>
            "a-stroke-013.svg",             // Not impl: <pattern>, "gradientUnits"
            "a-stroke-dasharray-002.svg",   // Bug? Simple dasharray
            "a-stroke-dasharray-003.svg",   // Bug? Odd list
            "a-stroke-dasharray-004.svg",   // Bug? Dasharray with %
            "a-stroke-dasharray-005.svg",   // Bug? "em" units
            "a-stroke-dasharray-006.svg",   // Bug? "mm" units
            "a-stroke-dasharray-007.svg",   // UB (negative values)
            "a-stroke-dasharray-009.svg",   // UB (negative sum)
            "a-stroke-dasharray-010.svg",   // Bug? comma-ws
            "a-stroke-dasharray-011.svg",   // Bug? ws separator
            "a-stroke-dasharray-012.svg",   // Bug? Circle segments clockwise
            "a-stroke-dasharray-013.svg",   // Bug? Dasharray should be reset on a new subpath
            "a-stroke-dashoffset-001.svg",  // Bug? "Default", dashoffset=0?
            "a-stroke-dashoffset-003.svg",  // Bug? "mm" units
            "a-stroke-dashoffset-004.svg",  // Bug? dashoffset "em" units
            "a-stroke-dashoffset-005.svg",  // Bug? dashoffset %
            "a-stroke-linejoin-004.svg",    // UB (SVG 2), no UA supports `miter-clip`
            "a-stroke-linejoin-005.svg",    // UB (SVG 2), no UA supports `arcs`
            "a-stroke-opacity-002.svg",     // Not impl: "opacity"
            "a-stroke-opacity-003.svg",     // Not impl: <linearGradient>
            "a-stroke-opacity-004.svg",     // Not impl: <pattern>
            "a-stroke-opacity-006.svg",     // Not impl: <text>
            "a-stroke-width-004.svg",       // UB: Nothing should be renderered
            "a-stroke-width-005.svg",       // Bug? stroke-width %
        })));

}  // namespace donner::svg
