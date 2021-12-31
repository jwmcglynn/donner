#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <pixelmatch/pixelmatch.h>

#include <fstream>

#include "src/renderer/renderer_skia.h"
#include "src/renderer/renderer_utils.h"
#include "src/renderer/tests/renderer_test_utils.h"
#include "src/svg/xml/xml_parser.h"

namespace donner {

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

  void renderAndCompare(SVGDocument& document, int width, int height,
                        const char* goldenImageFilename) {
    RendererSkia renderer(width, height);
    renderer.draw(document);

    const size_t strideInPixels = width;

    const char* goldenImagePath = std::getenv("UPDATE_GOLDEN_IMAGES");
    bool updateGoldenImages = (goldenImagePath != nullptr);
    Image goldenImage;

    if (updateGoldenImages) {
      goldenImage = Image{width, height, strideInPixels, std::vector<uint8_t>(width * height * 4)};
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

    if (updateGoldenImages) {
      const std::filesystem::path imageToUpdate =
          std::filesystem::path(goldenImagePath) / goldenImageFilename;
      RendererUtils::writeRgbaPixelsToPngFile(imageToUpdate.string().c_str(), renderer.pixelData(),
                                              width, height);
    } else if (mismatchedPixels != 0) {
      const std::filesystem::path actualDiffFilename =
          std::filesystem::temp_directory_path() / escapeFilename(goldenImageFilename);
      std::cerr << "Saving actual diff to: " << actualDiffFilename << std::endl;

      RendererUtils::writeRgbaPixelsToPngFile(actualDiffFilename.string().c_str(), diffImage, width,
                                              height);

      FAIL() << "Computed image diff and expected version in " << goldenImageFilename
             << " do not match, " << mismatchedPixels << " pixels do not match.";
    }
  }
};

TEST_F(RendererTests, Ghostscript_Tiger) {
  SVGDocument document = loadSVG("src/renderer/testdata/Ghostscript_Tiger.svg");
  renderAndCompare(document, 900, 900, "src/renderer/testdata/Ghostscript_Tiger.png");
}

}  // namespace donner
