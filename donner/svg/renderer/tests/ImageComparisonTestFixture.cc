#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

#include <pixelmatch/pixelmatch.h>

#include <filesystem>
#include <fstream>

#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererImageIO.h"
#include "donner/svg/renderer/RendererSkia.h"
#include "donner/svg/renderer/tests/RendererTestUtils.h"
#include "donner/svg/resources/SandboxedFileResourceLoader.h"

// Skia
#include "include/core/SkData.h"
#include "include/core/SkPicture.h"

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

std::string TestNameFromFilename(const testing::TestParamInfo<ImageComparisonTestcase>& info) {
  std::string name = info.param.displayName.empty() ? info.param.svgFilename.stem().string()
                                                    : info.param.displayName;

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

SVGDocument ImageComparisonTestFixture::loadSVG(
    const char* filename, const std::optional<std::filesystem::path>& resourceDir) {
  std::ifstream file(filename);
  EXPECT_TRUE(file) << "Failed to open file: " << filename;
  if (!file) {
    return SVGDocument();
  }

  std::string fileData;
  file.seekg(0, std::ios::end);
  const std::streamsize fileLength = file.tellg();
  file.seekg(0);

  fileData.resize(fileLength);
  file.read(fileData.data(), fileLength);

  parser::SVGParser::Options options;
  options.enableExperimental = true;

  std::unique_ptr<ResourceLoaderInterface> resourceLoader;
  if (resourceDir) {
    resourceLoader = std::make_unique<SandboxedFileResourceLoader>(*resourceDir,
                                                                   std::filesystem::path(filename));
  }

  auto maybeResult = parser::SVGParser::ParseSVG(fileData, /*outWarnings=*/nullptr, options,
                                                 std::move(resourceLoader));
  EXPECT_FALSE(maybeResult.hasError()) << "Parse Error: " << maybeResult.error();
  if (maybeResult.hasError()) {
    return SVGDocument();
  }

  return std::move(maybeResult.result());
}

void ImageComparisonTestFixture::renderAndCompare(SVGDocument& document,
                                                  const std::filesystem::path& svgFilename,
                                                  const char* goldenImageFilename) {
  renderAndCompare(document, svgFilename, goldenImageFilename, GetParam().params);
}

void ImageComparisonTestFixture::renderAndCompare(SVGDocument& document,
                                                  const std::filesystem::path& svgFilename,
                                                  const char* goldenImageFilename,
                                                  const ImageComparisonParams& params) {
  std::cout << "[  COMPARE ] " << svgFilename.string()
            << ": ";  // No endl yet, the line will be continued

  // The canvas size to draw into, as a recommendation instead of a strict guideline, since some
  // SVGs may override.
  if (params.canvasSize) {
    document.setCanvasSize(params.canvasSize->x, params.canvasSize->y);
  }

  RendererSkia renderer(/*verbose*/ false);
  renderer.draw(document);

  const size_t strideInPixels = renderer.width();
  const int width = renderer.width();
  const int height = renderer.height();

  if (params.updateGoldenFromEnv) {
    const char* goldenImageDirToUpdate = getenv("UPDATE_GOLDEN_IMAGES_DIR");
    if (goldenImageDirToUpdate != nullptr) {
      const std::filesystem::path goldenImagePath =
          std::filesystem::path(goldenImageDirToUpdate) / goldenImageFilename;
      RendererImageIO::writeRgbaPixelsToPngFile(
          goldenImagePath.string().c_str(), renderer.pixelData(), width, height, strideInPixels);
      std::cout << "Updated golden image: " << goldenImagePath.string() << "\n";
      return;
    }
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
  options.threshold = params.threshold;
  const int mismatchedPixels = pixelmatch::pixelmatch(
      goldenImage.data, renderer.pixelData(), diffImage, width, height, strideInPixels, options);

  if (mismatchedPixels > params.maxMismatchedPixels) {
    std::cout << "FAIL (" << mismatchedPixels << " pixels differ, with "
              << params.maxMismatchedPixels << " max)\n";

    const std::filesystem::path actualImagePath =
        std::filesystem::temp_directory_path() / escapeFilename(goldenImageFilename);
    RendererImageIO::writeRgbaPixelsToPngFile(actualImagePath.string().c_str(),
                                              renderer.pixelData(), width, height, strideInPixels);

    const std::filesystem::path diffFilePath =
        std::filesystem::temp_directory_path() / ("diff_" + escapeFilename(goldenImageFilename));
    RendererImageIO::writeRgbaPixelsToPngFile(diffFilePath.string().c_str(), diffImage, width,
                                              height, strideInPixels);

    if (params.saveDebugSkpOnFailure) {
      std::cout << "=> Re-rendering with verbose output and creating .skp (SkPicture)\n";

      {
        RendererSkia rendererVerbose(/*verbose*/ true);
        sk_sp<SkPicture> picture = rendererVerbose.drawIntoSkPicture(document);

        sk_sp<SkData> pictureData = picture->serialize();

        const std::filesystem::path skpFilePath =
            std::filesystem::temp_directory_path() / (escapeFilename(goldenImageFilename) + ".skp");
        {
          std::ofstream file(skpFilePath.string());
          file.write(
              reinterpret_cast<const char*>(pictureData->data()),  // NOLINT: Intentional cast
              static_cast<std::streamsize>(pictureData->size()));
          EXPECT_TRUE(file.good());
        }

        std::cout << "Load this .skp into https://debugger.skia.org/\n"
                  << "=> " << skpFilePath.string() << "\n\n";
      }
    }

    ADD_FAILURE() << mismatchedPixels << " pixels different.";

    // Output the SVG content for easier debugging
    std::cout << "\n\nSVG Content for " << svgFilename.filename().string() << ":\n---\n";
    std::ifstream svgFile(svgFilename);
    if (svgFile) {
      std::string svgContent((std::istreambuf_iterator<char>(svgFile)),
                             std::istreambuf_iterator<char>());
      std::cout << svgContent << "\n---\n";
    } else {
      std::cout << "Could not read SVG file: " << svgFilename << "\n---\n";
    }

    std::cout << "Actual rendering: " << actualImagePath.string() << "\n";
    std::cout << "Expected: " << goldenImageFilename << "\n";
    std::cout << "Diff: " << diffFilePath.string() << "\n\n";

    if (params.updateGoldenFromEnv) {
      std::cout << "To update the golden image, prefix UPDATE_GOLDEN_IMAGES_DIR=$(bazel info "
                   "workspace) to the bazel run command, e.g.:\n";
      std::cout << "  UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run "
                   "//donner/svg/renderer/tests:renderer_tests\n\n";
    }
  } else {
    std::cout << "PASS";
    if (mismatchedPixels != 0) {
      std::cout << " (" << mismatchedPixels << " pixels differ, out of "
                << params.maxMismatchedPixels << " max)";
    }
    std::cout << "\n";
  }
}

void ImageComparisonTestFixture::compareRgbaImages(const std::vector<uint8_t>& expected,
                                                   size_t expectedStrideBytes,
                                                   const std::vector<uint8_t>& actual,
                                                   size_t actualStrideBytes, int width, int height,
                                                   std::string_view debugName,
                                                   const ImageComparisonParams& params) {
  const size_t expectedBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  ASSERT_EQ(expectedBytes, expected.size());
  ASSERT_EQ(expectedBytes, actual.size());
  ASSERT_EQ(expectedStrideBytes % 4, 0u);
  ASSERT_EQ(actualStrideBytes % 4, 0u);

  const int expectedStridePixels = static_cast<int>(expectedStrideBytes / 4);
  const int actualStridePixels = static_cast<int>(actualStrideBytes / 4);
  ASSERT_EQ(expectedStridePixels, actualStridePixels);
  ASSERT_EQ(expectedStridePixels, width);

  std::cout << "[  COMPARE ] " << debugName << ": ";

  std::vector<uint8_t> diffImage(expectedBytes, 0);
  pixelmatch::Options options;
  options.threshold = params.threshold;
  const int mismatchedPixels =
      pixelmatch::pixelmatch(expected, actual, diffImage, width, height, expectedStridePixels,
                             options);

  if (mismatchedPixels > params.maxMismatchedPixels) {
    std::cout << "FAIL (" << mismatchedPixels << " pixels differ, with "
              << params.maxMismatchedPixels << " max)\n";

    const std::string sanitizedName = escapeFilename(std::string(debugName));
    const auto tempDir = std::filesystem::temp_directory_path();
    const auto expectedPath = tempDir / (sanitizedName + "_expected.png");
    const auto actualPath = tempDir / (sanitizedName + "_actual.png");
    const auto diffPath = tempDir / (sanitizedName + "_diff.png");

    if (params.saveDebugSkpOnFailure) {
      RendererImageIO::writeRgbaPixelsToPngFile(expectedPath.string().c_str(), expected, width,
                                                height, expectedStridePixels);
      RendererImageIO::writeRgbaPixelsToPngFile(actualPath.string().c_str(), actual, width, height,
                                                expectedStridePixels);
      RendererImageIO::writeRgbaPixelsToPngFile(diffPath.string().c_str(), diffImage, width, height,
                                                expectedStridePixels);
    }

    ADD_FAILURE() << mismatchedPixels << " pixels different. Expected: "
                  << expectedPath.string() << " Actual: " << actualPath.string()
                  << " Diff: " << diffPath.string();
  } else {
    std::cout << "PASS";
    if (mismatchedPixels != 0) {
      std::cout << " (" << mismatchedPixels << " pixels differ, out of "
                << params.maxMismatchedPixels << " max)";
    }
    std::cout << "\n";
  }
}

}  // namespace donner::svg
