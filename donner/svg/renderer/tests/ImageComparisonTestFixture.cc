#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

#include <pixelmatch/pixelmatch.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

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

bool isEnabledFromEnv(const char* name, bool defaultValue) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return defaultValue;
  }

  std::string lower(value);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });

  if (lower == "0" || lower == "false" || lower == "off") {
    return false;
  }

  if (lower == "1" || lower == "true" || lower == "on") {
    return true;
  }

  return defaultValue;
}

TerminalPixelMode pixelModeFromEnv() {
  const char* mode = std::getenv("DONNER_TERMINAL_PIXEL_MODE");
  if (mode == nullptr) {
    return TerminalPixelMode::kQuarterPixel;
  }

  std::string lower(mode);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });

  if (lower == "half") {
    return TerminalPixelMode::kHalfPixel;
  }

  return TerminalPixelMode::kQuarterPixel;
}

int visibleLength(std::string_view line) {
  int length = 0;
  for (size_t i = 0; i < line.size();) {
    if (line[i] == '\r') {
      ++i;
      continue;
    }

    if (line[i] == '\x1b' && (i + 1) < line.size() && line[i + 1] == '[') {
      i += 2;
      while (i < line.size() && line[i] != 'm') {
        ++i;
      }
      if (i < line.size()) {
        ++i;
      }
      continue;
    }

    // Handle UTF-8 multi-byte sequences
    const unsigned char byte = static_cast<unsigned char>(line[i]);
    if (byte < 0x80) {
      // Single-byte ASCII character
      i += 1;
    } else if ((byte & 0xE0) == 0xC0) {
      // 2-byte UTF-8 sequence
      i += 2;
    } else if ((byte & 0xF0) == 0xE0) {
      // 3-byte UTF-8 sequence
      i += 3;
    } else if ((byte & 0xF8) == 0xF0) {
      // 4-byte UTF-8 sequence
      i += 4;
    } else {
      // Invalid UTF-8 or continuation byte, skip it
      i += 1;
    }
    ++length;
  }

  return length;
}

int maxVisibleWidth(const std::vector<std::string>& lines) {
  int maxWidth = 0;
  for (const std::string& line : lines) {
    maxWidth = std::max(maxWidth, visibleLength(line));
  }

  return maxWidth;
}

void padToWidth(std::string& line, int targetWidth) {
  const int currentWidth = visibleLength(line);
  if (currentWidth >= targetWidth) {
    return;
  }

  line.append(static_cast<size_t>(targetWidth - currentWidth), ' ');
}

std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::string current;

  for (char character : text) {
    if (character == '\n') {
      if (!current.empty() && current.back() == '\r') {
        current.pop_back();
      }

      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(character);
    }
  }

  if (!current.empty()) {
    lines.push_back(current);
  }

  if (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }

  return lines;
}

struct ImageViewWithStorage {
  TerminalImageView view;
  std::vector<uint8_t> storage;
};

int cellColumns(const TerminalImageView& view, TerminalPixelMode mode) {
  const int cellWidth = mode == TerminalPixelMode::kQuarterPixel ? 2 : 1;
  return (view.width + cellWidth - 1) / cellWidth;
}

ImageViewWithStorage scaleImageIfNeeded(const TerminalImageView& source, TerminalPixelMode mode,
                                        int maxColumns) {
  const int columns = cellColumns(source, mode);
  if (columns <= maxColumns || source.width == 0 || source.height == 0) {
    return {source, {}};
  }

  const double scale = static_cast<double>(maxColumns) / static_cast<double>(columns);
  const int targetWidth = std::max(1, static_cast<int>(static_cast<double>(source.width) * scale));
  const int targetHeight =
      std::max(1, static_cast<int>(static_cast<double>(source.height) * scale));

  std::vector<uint8_t> pixels(static_cast<size_t>(targetWidth * targetHeight * 4));

  for (int y = 0; y < targetHeight; ++y) {
    const int sourceY =
        std::min(source.height - 1, static_cast<int>(static_cast<double>(y) / scale));
    const size_t sourceRowOffset = static_cast<size_t>(sourceY) * source.strideInPixels * 4;
    const size_t targetRowOffset = static_cast<size_t>(y) * targetWidth * 4;

    for (int x = 0; x < targetWidth; ++x) {
      const int sourceX =
          std::min(source.width - 1, static_cast<int>(static_cast<double>(x) / scale));
      const size_t sourceOffset = sourceRowOffset + static_cast<size_t>(sourceX) * 4;
      const size_t targetOffset = targetRowOffset + static_cast<size_t>(x) * 4;

      pixels[targetOffset] = source.data[sourceOffset];
      pixels[targetOffset + 1] = source.data[sourceOffset + 1];
      pixels[targetOffset + 2] = source.data[sourceOffset + 2];
      pixels[targetOffset + 3] = source.data[sourceOffset + 3];
    }
  }

  TerminalImageView scaledView{pixels, targetWidth, targetHeight, static_cast<size_t>(targetWidth)};
  return {scaledView, std::move(pixels)};
}

std::string renderCell(const TerminalImageView& view, std::string_view caption,
                       const TerminalImageViewer& viewer, TerminalPixelMode pixelMode,
                       const TerminalImageViewerConfig& baseConfig, int maxColumnWidth) {
  TerminalImageViewerConfig config = baseConfig;
  config.pixelMode = pixelMode;
  config.imageName = std::string(caption);  // Pass caption as image name

  std::ostringstream stream;

  if (config.enableITermInlineImages) {
    viewer.render(view, stream, config);
  } else {
    // For text mode, disable auto-scaling since we pre-scale to fit column width
    config.autoScale = false;
    const ImageViewWithStorage scaled = scaleImageIfNeeded(view, pixelMode, maxColumnWidth);
    viewer.render(scaled.view, stream, config);
  }

  return stream.str();
}

std::vector<std::string> combineColumns(const std::vector<std::string>& left,
                                        const std::vector<std::string>& right, int leftWidth,
                                        int rightWidth, int padding) {
  const size_t rows = std::max(left.size(), right.size());
  std::vector<std::string> combined;
  combined.reserve(rows);

  for (size_t row = 0; row < rows; ++row) {
    std::string leftLine = row < left.size() ? left[row] : std::string();
    std::string rightLine = row < right.size() ? right[row] : std::string();

    padToWidth(leftLine, leftWidth);
    padToWidth(rightLine, rightWidth);

    combined.push_back(leftLine + std::string(padding, ' ') + rightLine);
  }

  return combined;
}

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

std::optional<TerminalPreviewConfig> PreviewConfigFromEnv(const ImageComparisonParams& params) {
  if (!params.showTerminalPreview) {
    return std::nullopt;
  }

  if (!isEnabledFromEnv("DONNER_ENABLE_TERMINAL_IMAGES", /*defaultValue=*/true)) {
    return std::nullopt;
  }

  TerminalPreviewConfig config;
  config.pixelMode = pixelModeFromEnv();
  config.terminalWidth = TerminalImageViewer::DetectTerminalSize().columns;
  return config;
}

std::string TestNameFromFilename(const testing::TestParamInfo<ImageComparisonTestcase>& info) {
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

std::string RenderTerminalComparisonGridForTesting(const TerminalImageView& actual,
                                                   const TerminalImageView& expected,
                                                   const TerminalImageView& diff,
                                                   int maxTerminalWidth,
                                                   TerminalPixelMode pixelMode,
                                                   const TerminalImageViewerConfig& config) {
  const int columnPadding = 2;
  const int maxColumnWidth = std::max(10, (maxTerminalWidth - columnPadding) / 2);

  TerminalImageViewer viewer;
  std::string actualText = renderCell(actual, "Actual", viewer, pixelMode, config, maxColumnWidth);
  std::string expectedText =
      renderCell(expected, "Expected", viewer, pixelMode, config, maxColumnWidth);
  std::string diffText = renderCell(diff, "Diff", viewer, pixelMode, config, maxColumnWidth);

  std::string output;

  if (config.enableITermInlineImages) {
    // For iTerm inline images, calculate the actual row height based on image size
    // iTerm uses width=30%, so we calculate expected rows dynamically
    const int imageRows = viewer.calculateITermImageRows(
        actual.width, actual.height, 30, TerminalImageViewer::DetectTerminalSize(),
        TerminalImageViewer::DetectTerminalCellSize());

    const std::string ESC = "\x1b";

    std::ostringstream result;
    result << "Actual:\n";
    result << actualText;
    result << ESC << "[" << imageRows << "A";  // Move up to the start line
    result << "Expected:";
    result << ESC << "[1B";  // Move down one line
    result << ESC << "[9D";  // Move left to the start of the "Expected" image

    result << expectedText;
    result << "\n";

    // Output the diff on a second line
    result << "Diff:\n";
    result << diffText;

    output = result.str();
  } else {
    // Convert into two columns for side-by-side images.
    const std::vector<std::string> emptyLines = {""};

    std::vector<std::string> actualLines = splitLines(actualText);
    std::vector<std::string> expectedLines = splitLines(expectedText);
    std::vector<std::string> diffLines = splitLines(diffText);

    actualLines.insert(actualLines.begin(), "Actual");
    expectedLines.insert(expectedLines.begin(), "Expected");
    diffLines.insert(diffLines.begin(), "Diff");

    const int leftWidth = std::max(maxVisibleWidth(actualLines), maxVisibleWidth(diffLines));
    const int rightWidth = std::max(maxVisibleWidth(expectedLines), maxVisibleWidth(emptyLines));

    std::vector<std::string> combined =
        combineColumns(actualLines, expectedLines, leftWidth, rightWidth, columnPadding);
    std::vector<std::string> lower =
        combineColumns(diffLines, emptyLines, leftWidth, rightWidth, columnPadding);
    combined.insert(combined.end(), lower.begin(), lower.end());

    for (const std::string& line : combined) {
      output.append(line);
      output.push_back('\n');
    }
  }

  return output;
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

    const std::optional<TerminalPreviewConfig> previewConfig = PreviewConfigFromEnv(params);
    if (previewConfig.has_value()) {
      TerminalImageViewerConfig viewerConfig = TerminalImageViewer::DetectConfigFromEnvironment();
      viewerConfig.autoScale = true;  // Enable auto-scaling for terminal preview

      const TerminalImageView actualView{renderer.pixelData(), width, height, strideInPixels};
      const TerminalImageView expectedView{goldenImage.data, width, height, strideInPixels};
      const TerminalImageView diffView{diffImage, width, height, strideInPixels};

      std::cout << "Terminal preview:\n"
                << RenderTerminalComparisonGridForTesting(actualView, expectedView, diffView,
                                                          previewConfig->terminalWidth,
                                                          previewConfig->pixelMode, viewerConfig)
                << "\n";
    }

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

}  // namespace donner::svg
