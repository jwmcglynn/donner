#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <pixelmatch/pixelmatch.h>

#include <fstream>

#include "src/svg/renderer/renderer_skia.h"
#include "src/svg/renderer/renderer_utils.h"
#include "src/svg/renderer/tests/renderer_test_utils.h"
#include "src/svg/xml/xml_parser.h"

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
    // The size provided here specifies the default size, in most cases this is overridden by the
    // SVG.
    RendererSkia renderer(800, 600);
    renderer.draw(document);

    const size_t strideInPixels = renderer.width();
    const int width = renderer.width();
    const int height = renderer.height();
    Image goldenImage;

    const char* goldenImageDirToUpdate = getenv("UPDATE_GOLDEN_IMAGES_DIR");
    if (goldenImageDirToUpdate) {
      goldenImage = Image{width, height, strideInPixels, std::vector<uint8_t>(width * height * 4)};

      const std::filesystem::path goldenImagePath =
          std::filesystem::path(goldenImageDirToUpdate) / goldenImageFilename;

      RendererUtils::writeRgbaPixelsToPngFile(goldenImagePath.string().c_str(),
                                              renderer.pixelData(), width, height, strideInPixels);
    } else {
      auto maybeGoldenImage = RendererTestUtils::readRgbaImageFromPngFile(goldenImageFilename);
      ASSERT_TRUE(maybeGoldenImage.has_value());

      goldenImage = std::move(maybeGoldenImage.value());
      ASSERT_EQ(goldenImage.width, width);
      ASSERT_EQ(goldenImage.height, height);
      ASSERT_EQ(goldenImage.strideInPixels, strideInPixels);
      ASSERT_EQ(goldenImage.data.size(), renderer.pixelData().size());
    }

    std::vector<uint8_t> diffImage;
    diffImage.resize(strideInPixels * height * 4);

    pixelmatch::Options options;
    options.threshold = 0.0f;
    const int mismatchedPixels = pixelmatch::pixelmatch(
        goldenImage.data, renderer.pixelData(), diffImage, width, height, strideInPixels, options);

    if (mismatchedPixels != 0) {
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

TEST_F(RendererTests, Ghostscript_Tiger) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/Ghostscript_Tiger.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/Ghostscript_Tiger.png");
}

TEST_F(RendererTests, FillRule001) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/a-fill-rule-001.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/a-fill-rule-001.png");
}

TEST_F(RendererTests, FillRule002) {
  SVGDocument document = loadSVG("src/svg/renderer/testdata/a-fill-rule-002.svg");
  renderAndCompare(document, "src/svg/renderer/testdata/golden/a-fill-rule-002.png");
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

}  // namespace donner::svg
