#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <pixelmatch/pixelmatch.h>

#include <fstream>

#include "src/svg/renderer/renderer_image_io.h"
#include "src/svg/renderer/renderer_skia.h"
#include "src/svg/renderer/tests/renderer_test_utils.h"
#include "src/svg/xml/xml_parser.h"

// clang-format off
/**
 * @file
 *
 * Renderer image comparison tests, which render an SVG file and compare the result to a golden
 * image checked into the repo.
 *
 * If this test fails, update goldens with the following command:
 * ```sh
 * # Set this environment variable to the donner root directory
 * UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run //src/svg/renderer/tests:renderer_tests
 * ```
 */
// clang-format on

namespace donner::svg {

namespace {

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

}  // namespace

class RendererTests : public testing::Test {
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

  void renderAndCompare(SVGDocument& document, const char* goldenImageFilename) {
    RendererSkia renderer;
    renderer.draw(document);

    const size_t strideInPixels = renderer.width();
    const int width = renderer.width();
    const int height = renderer.height();

    const char* goldenImageDirToUpdate = getenv("UPDATE_GOLDEN_IMAGES_DIR");
    if (goldenImageDirToUpdate) {
      const std::filesystem::path goldenImagePath =
          std::filesystem::path(goldenImageDirToUpdate) / goldenImageFilename;

      RendererImageIO::writeRgbaPixelsToPngFile(
          goldenImagePath.string().c_str(), renderer.pixelData(), width, height, strideInPixels);

      std::cout << "Updated golden image: " << goldenImagePath.string() << std::endl;
      return;
    }

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
    options.threshold = 0.1f;  // Apply a non-zero threshold to account for anti-aliasing
                               // differences between platforms. Without this, macOS/Linux would be
                               // unable to use each other's goldens.
    const int mismatchedPixels = pixelmatch::pixelmatch(
        goldenImage.data, renderer.pixelData(), diffImage, width, height, strideInPixels, options);

    if (mismatchedPixels != 0) {
      const std::filesystem::path actualImagePath =
          std::filesystem::temp_directory_path() / escapeFilename(goldenImageFilename);
      std::cout << "Actual rendering: " << actualImagePath.string() << std::endl;
      RendererImageIO::writeRgbaPixelsToPngFile(
          actualImagePath.string().c_str(), renderer.pixelData(), width, height, strideInPixels);

      const std::filesystem::path diffFilePath =
          std::filesystem::temp_directory_path() / ("diff_" + escapeFilename(goldenImageFilename));
      std::cerr << "Diff: " << diffFilePath.string() << std::endl;

      RendererImageIO::writeRgbaPixelsToPngFile(diffFilePath.string().c_str(), diffImage, width,
                                                height, strideInPixels);

      FAIL() << "Computed image diff and expected version in " << goldenImageFilename
             << " do not match, " << mismatchedPixels << " pixels different.";
    }
  }
};

TEST_F(RendererTests, Ellipse1) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/ellipse1.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/ellipse1.png");
}

TEST_F(RendererTests, Rect2) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/rect2.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/rect2.png");
}

TEST_F(RendererTests, Skew1) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/skew1.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/skew1.png");
}

TEST_F(RendererTests, SizeTooLarge) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/size-too-large.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/size-too-large.png");
}

TEST_F(RendererTests, NestedSvgAspectRatio) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/nested-svg-aspectratio.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/nested-svg-aspectratio.png");
}

TEST_F(RendererTests, RadialFr1) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/radial-fr-1.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/radial-fr-1.png");
}

TEST_F(RendererTests, RadialFr2) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/radial-fr-2.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/radial-fr-2.png");
}

TEST_F(RendererTests, RadialConical1) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/radial-conical-1.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/radial-conical-1.png");
}

TEST_F(RendererTests, RadialConical2) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/radial-conical-2.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/radial-conical-2.png");
}

TEST_F(RendererTests, GhostscriptTiger) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/Ghostscript_Tiger.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/Ghostscript_Tiger.png");
}

TEST_F(RendererTests, Polygon) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/polygon.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/polygon.png");
}

TEST_F(RendererTests, Polyline) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/polyline.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/polyline.png");
}

TEST_F(RendererTests, Lion) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/lion.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/lion.png");
}

TEST_F(RendererTests, StrokingComplex) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/stroking_complex.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/stroking_complex.png");
}

TEST_F(RendererTests, StrokingDasharray) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/stroking_dasharray.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/stroking_dasharray.png");
}

TEST_F(RendererTests, StrokingDashoffset) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/stroking_dashoffset.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/stroking_dashoffset.png");
}

TEST_F(RendererTests, StrokingLinecap) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/stroking_linecap.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/stroking_linecap.png");
}

TEST_F(RendererTests, StrokingLinejoin) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/stroking_linejoin.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/stroking_linejoin.png");
}

TEST_F(RendererTests, StrokingMiterlimit) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/stroking_miterlimit.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/stroking_miterlimit.png");
}

TEST_F(RendererTests, StrokingStrokewidth) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/stroking_strokewidth.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/stroking_strokewidth.png");
}

TEST_F(RendererTests, StrokingPathLength) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/stroking_pathlength.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/stroking_pathlength.png");
}

TEST_F(RendererTests, PokerChips) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/poker_chips.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/poker_chips.png");
}

TEST_F(RendererTests, QuadBezier) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/quadbezier1.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/quadbezier1.png");
}

TEST_F(RendererTests, DonnerIcon) {
  SVGDocument document = loadSVG("donner_icon.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/donner_icon.png");
}

TEST_F(RendererTests, DonnerSplash) {
  SVGDocument document = loadSVG("donner_splash.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/donner_splash.png");
}

TEST_F(RendererTests, SVG2_e_use_001) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/svg2-e-use-001.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/svg2-e-use-001.png");
}

TEST_F(RendererTests, SVG2_e_use_002) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/svg2-e-use-002.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/svg2-e-use-002.png");
}

TEST_F(RendererTests, SVG2_e_use_003) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/svg2-e-use-003.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/svg2-e-use-003.png");
}

TEST_F(RendererTests, SVG2_e_use_004) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/svg2-e-use-004.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/svg2-e-use-004.png");
}

TEST_F(RendererTests, SVG2_e_use_005) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/svg2-e-use-005.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/svg2-e-use-005.png");
}

}  // namespace donner::svg
