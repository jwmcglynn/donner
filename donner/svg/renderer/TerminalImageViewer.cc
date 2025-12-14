#include "donner/svg/renderer/TerminalImageViewer.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/encoding/Base64.h"
#include "donner/svg/renderer/RendererImageIO.h"

namespace donner::svg {
namespace {
constexpr int kQuarterPixelWidth = 2;
constexpr int kQuarterPixelHeight = 2;
constexpr int kHalfPixelWidth = 1;
constexpr int kHalfPixelHeight = 2;

constexpr std::array<int, 6> kCubeValues = {0, 95, 135, 175, 215, 255};

constexpr std::array<const char*, 16> kQuarterBlockGlyphs = {
    " ", "▘", "▝", "▀", "▖", "▌", "▞", "▛", "▗", "▚", "▐", "▜", "▄", "▙", "▟", "█",
};

css::RGBA combineSamples(uint64_t weightedR, uint64_t weightedG, uint64_t weightedB,
                         uint64_t totalAlpha, int pixelCount) {
  if (pixelCount == 0 || totalAlpha == 0) {
    return css::RGBA(0, 0, 0, 0);
  }

  const uint8_t alpha = static_cast<uint8_t>(totalAlpha / pixelCount);
  const uint8_t red = static_cast<uint8_t>(weightedR / totalAlpha);
  const uint8_t green = static_cast<uint8_t>(weightedG / totalAlpha);
  const uint8_t blue = static_cast<uint8_t>(weightedB / totalAlpha);

  return css::RGBA(red, green, blue, alpha);
}

double luminance(const css::RGBA& color) {
  return 0.2126 * static_cast<double>(color.r) + 0.7152 * static_cast<double>(color.g) +
         0.0722 * static_cast<double>(color.b);
}

css::RGBA averageColors(const std::vector<std::pair<css::RGBA, uint64_t>>& samples) {
  uint64_t weightedR = 0;
  uint64_t weightedG = 0;
  uint64_t weightedB = 0;
  uint64_t totalAlpha = 0;
  int count = 0;

  for (const auto& [color, weight] : samples) {
    if (weight == 0) {
      continue;
    }

    weightedR += static_cast<uint64_t>(color.r) * weight;
    weightedG += static_cast<uint64_t>(color.g) * weight;
    weightedB += static_cast<uint64_t>(color.b) * weight;
    totalAlpha += weight;
    ++count;
  }

  return combineSamples(weightedR, weightedG, weightedB, totalAlpha, count);
}

uint8_t clampAlpha(uint8_t alpha) {
  return alpha == 0 ? 0x01 : alpha;
}

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) {
  if (needle.empty() || haystack.size() < needle.size()) {
    return false;
  }

  const std::string lowerHaystack = [&]() {
    std::string lower(haystack);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return lower;
  }();

  const std::string lowerNeedle = [&]() {
    std::string lower(needle);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return lower;
  }();

  return lowerHaystack.find(lowerNeedle) != std::string::npos;
}

void writeTrueColor(std::ostream& output, int selector, const css::RGBA& color) {
  output << "\x1b[" << selector << ";2;" << static_cast<int>(color.r) << ';'
         << static_cast<int>(color.g) << ';' << static_cast<int>(color.b) << 'm';
}

int colorDistanceSquared(int r1, int g1, int b1, int r2, int g2, int b2) {
  const int redDistance = r1 - r2;
  const int greenDistance = g1 - g2;
  const int blueDistance = b1 - b2;

  return redDistance * redDistance + greenDistance * greenDistance + blueDistance * blueDistance;
}

int nearestCubeLevel(uint8_t component) {
  int bestLevel = 0;
  int bestDistance = std::numeric_limits<int>::max();

  for (int i = 0; i < static_cast<int>(kCubeValues.size()); ++i) {
    const int distance = std::abs(static_cast<int>(component) - static_cast<int>(kCubeValues[i]));
    if (distance < bestDistance) {
      bestDistance = distance;
      bestLevel = i;
    }
  }

  return bestLevel;
}

int nearest256ColorIndex(const css::RGBA& color) {
  if (color.a == 0) {
    return 0;
  }

  const int redLevel = nearestCubeLevel(color.r);
  const int greenLevel = nearestCubeLevel(color.g);
  const int blueLevel = nearestCubeLevel(color.b);

  const int cubeRed = kCubeValues[redLevel];
  const int cubeGreen = kCubeValues[greenLevel];
  const int cubeBlue = kCubeValues[blueLevel];
  const int cubeDistance =
      colorDistanceSquared(color.r, color.g, color.b, cubeRed, cubeGreen, cubeBlue);
  const int cubeIndex = 16 + 36 * redLevel + 6 * greenLevel + blueLevel;

  const int average =
      (static_cast<int>(color.r) + static_cast<int>(color.g) + static_cast<int>(color.b)) / 3;
  const int grayLevel = std::clamp((average - 8 + 5) / 10, 0, 23);
  const int grayValue = 8 + grayLevel * 10;
  const int grayDistance =
      colorDistanceSquared(color.r, color.g, color.b, grayValue, grayValue, grayValue);
  const int grayIndex = 232 + grayLevel;

  if (grayDistance < cubeDistance) {
    return grayIndex;
  }

  return cubeIndex;
}

void write256Color(std::ostream& output, int selector, const css::RGBA& color) {
  output << "\x1b[" << selector << ";5;" << nearest256ColorIndex(color) << 'm';
}

uint8_t medianAlpha(const QuarterBlock& block) {
  std::array<uint8_t, 4> alphas = {block.topLeft.a, block.topRight.a, block.bottomLeft.a,
                                   block.bottomRight.a};
  std::sort(alphas.begin(), alphas.end());
  const int medianSum = static_cast<int>(alphas[1]) + static_cast<int>(alphas[2]);
  return static_cast<uint8_t>(medianSum / 2);
}

double medianLuminance(const QuarterBlock& block) {
  std::array<double, 4> luminances = {luminance(block.topLeft), luminance(block.topRight),
                                      luminance(block.bottomLeft), luminance(block.bottomRight)};
  std::sort(luminances.begin(), luminances.end());
  return (luminances[1] + luminances[2]) / 2.0;
}

bool envMatchesValue(std::string_view value, std::string_view expectation) {
  return !value.empty() && containsIgnoreCase(value, expectation);
}

std::optional<winsize> tryGetWindowSize() {
  // Try stderr first (most likely to be connected to the terminal when running tests),
  // then stdout, then stdin. Try ioctl even if isatty returns false, as some terminals
  // may still respond.
  for (int fd : {STDERR_FILENO, STDOUT_FILENO, STDIN_FILENO}) {
    struct winsize w;
    if (ioctl(fd, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0) {
      return w;
    }
  }
  return std::nullopt;
}

TerminalSize detectTerminalSize() {
  TerminalSize size;

  if (const auto w = tryGetWindowSize()) {
    size.columns = w->ws_col;
    size.rows = w->ws_row;
  }

  if (const char* columnsEnv = std::getenv("COLUMNS")) {
    if (const int columns = std::atoi(columnsEnv); columns > 0) {
      size.columns = columns;
    }
  }

  if (const char* linesEnv = std::getenv("LINES")) {
    if (const int rows = std::atoi(linesEnv); rows > 0) {
      size.rows = rows;
    }
  }

  return size;
}

double calculateAutoScale(int imageWidth, int imageHeight, const TerminalImageViewerConfig& config,
                          const TerminalSize& terminalSize) {
  const TerminalPixelMode mode = config.pixelMode;
  const int cellWidth =
      mode == TerminalPixelMode::kQuarterPixel ? kQuarterPixelWidth : kHalfPixelWidth;
  const int cellHeight =
      mode == TerminalPixelMode::kQuarterPixel ? kQuarterPixelHeight : kHalfPixelHeight;

  // Adjust vertical scale factor based on cell width to maintain consistent aspect ratio across
  // pixel modes. Quarter-pixel mode (2x2) uses the configured factor, while half-pixel mode (1x2)
  // needs double the vertical scaling to compensate for the narrower cells.
  const double modeAdjustedVerticalScale =
      config.verticalScaleFactor * (static_cast<double>(kQuarterPixelWidth) / cellWidth);

  // Target size: use the smaller of actual terminal size and configured max
  // This ensures images fit within the terminal while respecting the max bounds
  const int targetColumns = std::min(terminalSize.columns, config.maxTerminalWidth);
  const int targetRows = std::min(terminalSize.rows, config.maxTerminalHeight);

  // Calculate scale to fit within target, accounting for vertical scale factor
  const double scaleForWidth = (static_cast<double>(targetColumns) * cellWidth) / imageWidth;
  const double scaleForHeight =
      (static_cast<double>(targetRows) * cellHeight) / (imageHeight * modeAdjustedVerticalScale);

  // Use the smaller scale to ensure image fits in both dimensions
  return std::min(scaleForWidth, scaleForHeight);
}

TerminalCapabilities detectCapabilitiesFromEnvironment() {
  TerminalCapabilities capabilities{};

  // Allow manual override via environment variable for testing
  const char* forceIterm = std::getenv("DONNER_FORCE_ITERM_IMAGES");
  if (forceIterm && *forceIterm != '\0') {
    const bool enabled = std::strcmp(forceIterm, "1") == 0 || std::strcmp(forceIterm, "true") == 0;
    capabilities.supportsITermInlineImages = enabled;

    // Still detect true color support normally
    const char* colorTerm = std::getenv("COLORTERM");
    const std::string_view colorTermView =
        colorTerm == nullptr ? std::string_view() : std::string_view(colorTerm);
    capabilities.supportsTrueColor =
        envMatchesValue(colorTermView, "truecolor") || envMatchesValue(colorTermView, "24bit");
    if (!capabilities.supportsTrueColor) {
      const char* term = std::getenv("TERM");
      const std::string_view termView = term == nullptr ? std::string_view() : term;
      capabilities.supportsTrueColor = envMatchesValue(termView, "truecolor");
    }
    if (capabilities.supportsITermInlineImages) {
      capabilities.supportsTrueColor = true;
    }
    return capabilities;
  }

  const char* termProgram = std::getenv("TERM_PROGRAM");
  const char* lcTerminal = std::getenv("LC_TERMINAL");

  const std::string_view termProgramView =
      termProgram == nullptr ? std::string_view() : std::string_view(termProgram);
  const std::string_view lcTerminalView =
      lcTerminal == nullptr ? std::string_view() : std::string_view(lcTerminal);

  // Detect terminals that support iTerm2 inline image protocol BY DEFAULT
  // Note: VSCode requires terminal.integrated.enableImages=true, so we don't auto-enable it
  const bool isITerm =
      envMatchesValue(termProgramView, "iterm") || envMatchesValue(lcTerminalView, "iterm2");
  const bool isWezTerm = envMatchesValue(termProgramView, "wezterm");

  // Only auto-enable for terminals that support images by default
  capabilities.supportsITermInlineImages = isITerm || isWezTerm;

  const char* colorTerm = std::getenv("COLORTERM");
  const std::string_view colorTermView =
      colorTerm == nullptr ? std::string_view() : std::string_view(colorTerm);
  if (envMatchesValue(colorTermView, "truecolor") || envMatchesValue(colorTermView, "24bit")) {
    capabilities.supportsTrueColor = true;
  }

  if (!capabilities.supportsTrueColor) {
    const char* term = std::getenv("TERM");
    const std::string_view termView = term == nullptr ? std::string_view() : term;
    capabilities.supportsTrueColor = envMatchesValue(termView, "truecolor");
  }

  if (capabilities.supportsITermInlineImages) {
    capabilities.supportsTrueColor = true;
  }

  return capabilities;
}

}  // namespace

const TerminalCell& TerminalImage::cellAt(int column, int row) const {
  assert(column >= 0 && column < columns);
  assert(row >= 0 && row < rows);

  return cells[static_cast<size_t>(row * columns + column)];
}

TerminalImageViewerConfig TerminalImageViewer::DetectConfigFromEnvironment() {
  const TerminalCapabilities capabilities = detectCapabilitiesFromEnvironment();

  TerminalImageViewerConfig config;
  config.useTrueColor = capabilities.supportsTrueColor;
  config.enableITermInlineImages = capabilities.supportsITermInlineImages;
  return config;
}

TerminalSize TerminalImageViewer::DetectTerminalSize() {
  return detectTerminalSize();
}

TerminalCellSize TerminalImageViewer::DetectTerminalCellSize() {
  TerminalCellSize cellSize;

  if (const auto w = tryGetWindowSize()) {
    if (w->ws_xpixel > 0 && w->ws_ypixel > 0) {
      cellSize.widthPixels = w->ws_xpixel / w->ws_col;
      cellSize.heightPixels = w->ws_ypixel / w->ws_row;
    }
  }

  return cellSize;
}

TerminalImage TerminalImageViewer::sampleImage(const TerminalImageView& image,
                                               const TerminalImageViewerConfig& config) const {
  // Calculate the appropriate scale factor
  double effectiveScale = config.scale;
  if (config.autoScale) {
    const TerminalSize terminalSize = detectTerminalSize();
    effectiveScale = calculateAutoScale(image.width, image.height, config, terminalSize);
  }

  const TerminalPixelMode mode = config.pixelMode;
  const int cellWidth =
      mode == TerminalPixelMode::kQuarterPixel ? kQuarterPixelWidth : kHalfPixelWidth;
  const int cellHeight =
      mode == TerminalPixelMode::kQuarterPixel ? kQuarterPixelHeight : kHalfPixelHeight;

  // Adjust vertical scale factor based on cell width to maintain consistent aspect ratio across
  // pixel modes. Quarter-pixel mode (2x2) uses the configured factor, while half-pixel mode (1x2)
  // needs double the vertical scaling to compensate for the narrower cells.
  const double modeAdjustedVerticalScale =
      config.verticalScaleFactor * (static_cast<double>(kQuarterPixelWidth) / cellWidth);

  // Apply overall scale factor and vertical scaling factor to account for terminal character
  // aspect ratio
  const int scaledWidth = static_cast<int>(static_cast<double>(image.width) * effectiveScale);
  const int scaledHeight = static_cast<int>(static_cast<double>(image.height) * effectiveScale *
                                            modeAdjustedVerticalScale);

  const int columns = (scaledWidth + cellWidth - 1) / cellWidth;
  const int rows = (scaledHeight + cellHeight - 1) / cellHeight;

  std::vector<TerminalCell> cells;
  cells.reserve(static_cast<size_t>(columns * rows));

  // Inverse scaling to map from terminal space back to image space
  const double xScale = 1.0 / effectiveScale;
  const double yScale = 1.0 / (effectiveScale * modeAdjustedVerticalScale);

  for (int row = 0; row < rows; ++row) {
    // Map scaled row position back to original image coordinates
    const int startY = static_cast<int>(static_cast<double>(row * cellHeight) * yScale);

    for (int column = 0; column < columns; ++column) {
      // Map scaled column position back to original image coordinates
      const int startX = static_cast<int>(static_cast<double>(column * cellWidth) * xScale);

      QuarterBlock quarters{};
      HalfBlock halves{};

      if (mode == TerminalPixelMode::kQuarterPixel) {
        quarters.topLeft = sampleRegion(image, startX, startY, 1, 1);
        quarters.topRight = sampleRegion(image, startX + 1, startY, 1, 1);
        quarters.bottomLeft = sampleRegion(image, startX, startY + 1, 1, 1);
        quarters.bottomRight = sampleRegion(image, startX + 1, startY + 1, 1, 1);
      } else {
        halves.upper = sampleRegion(image, startX, startY, 1, 1);
        halves.lower = sampleRegion(image, startX, startY + 1, 1, 1);
      }

      cells.push_back({mode, quarters, halves});
    }
  }

  return TerminalImage{mode, columns, rows, std::move(cells)};
}

css::RGBA TerminalImageViewer::sampleRegion(const TerminalImageView& image, int startX, int startY,
                                            int regionWidth, int regionHeight) const {
  const int endX = std::min(startX + regionWidth, image.width);
  const int endY = std::min(startY + regionHeight, image.height);

  if (startX >= image.width || startY >= image.height || startX >= endX || startY >= endY) {
    return css::RGBA(0, 0, 0, 0);
  }

  uint64_t weightedR = 0;
  uint64_t weightedG = 0;
  uint64_t weightedB = 0;
  uint64_t totalAlpha = 0;
  int pixelCount = 0;

  for (int y = startY; y < endY; ++y) {
    const size_t rowOffset = static_cast<size_t>(y) * image.strideInPixels * 4;
    for (int x = startX; x < endX; ++x) {
      const size_t offset = rowOffset + static_cast<size_t>(x) * 4;
      const uint8_t alpha = image.data[offset + 3];

      weightedR += static_cast<uint64_t>(image.data[offset]) * alpha;
      weightedG += static_cast<uint64_t>(image.data[offset + 1]) * alpha;
      weightedB += static_cast<uint64_t>(image.data[offset + 2]) * alpha;
      totalAlpha += alpha;
      ++pixelCount;
    }
  }

  return combineSamples(weightedR, weightedG, weightedB, totalAlpha, pixelCount);
}

void TerminalImageViewer::render(const TerminalImageView& image, std::ostream& output,
                                 const TerminalImageViewerConfig& config) const {
  if (config.enableITermInlineImages) {
    renderITermInlineImage(image, output, config);
  } else {
    renderSampled(sampleImage(image, config), output, config);
  }
}

void TerminalImageViewer::renderSampled(const TerminalImage& sampledImage, std::ostream& output,
                                        const TerminalImageViewerConfig& config) const {
  for (int row = 0; row < sampledImage.rows; ++row) {
    for (int column = 0; column < sampledImage.columns; ++column) {
      const TerminalCell& cell = sampledImage.cellAt(column, row);

      auto writeColor = [&](int selector, const css::RGBA& color) {
        if (config.useTrueColor) {
          writeTrueColor(output, selector, color);
        } else {
          write256Color(output, selector, color);
        }
      };

      if (cell.mode == TerminalPixelMode::kQuarterPixel) {
        const QuarterBlock& quarters = cell.quarter;
        const uint8_t alphaThreshold = medianAlpha(quarters);
        const double luminanceThreshold = medianLuminance(quarters);
        const uint8_t minAlpha = std::min(std::min(quarters.topLeft.a, quarters.topRight.a),
                                          std::min(quarters.bottomLeft.a, quarters.bottomRight.a));
        const uint8_t maxAlpha = std::max(std::max(quarters.topLeft.a, quarters.topRight.a),
                                          std::max(quarters.bottomLeft.a, quarters.bottomRight.a));
        const bool alphaUniform = minAlpha == maxAlpha;

        uint8_t mask = 0;
        std::vector<std::pair<css::RGBA, uint64_t>> foregroundSamples;
        std::vector<std::pair<css::RGBA, uint64_t>> backgroundSamples;

        auto considerQuadrant = [&](const css::RGBA& color, int bit) {
          if (color.a == 0) {
            backgroundSamples.emplace_back(color, 0);
            return;
          }

          const double currentLuminance = luminance(color);
          bool prefersForeground = color.a >= alphaThreshold;
          if (alphaUniform) {
            prefersForeground = currentLuminance >= luminanceThreshold;
          }

          if (prefersForeground) {
            mask |= static_cast<uint8_t>(bit);
            foregroundSamples.emplace_back(color, clampAlpha(color.a));
          } else {
            backgroundSamples.emplace_back(color, clampAlpha(color.a));
          }
        };

        considerQuadrant(quarters.topLeft, 0b0001);
        considerQuadrant(quarters.topRight, 0b0010);
        considerQuadrant(quarters.bottomLeft, 0b0100);
        considerQuadrant(quarters.bottomRight, 0b1000);

        if (mask == 0 && !foregroundSamples.empty()) {
          mask = 0b0001;
          backgroundSamples.push_back(foregroundSamples.front());
          foregroundSamples.erase(foregroundSamples.begin());
        }

        const css::RGBA fgColor = averageColors(foregroundSamples);
        const css::RGBA bgColor = averageColors(backgroundSamples);

        writeColor(38, fgColor);
        writeColor(48, bgColor);
        output << kQuarterBlockGlyphs[mask];
      } else {
        const css::RGBA fgColor = cell.half.upper;
        const css::RGBA bgColor = cell.half.lower;

        writeColor(38, fgColor);
        writeColor(48, bgColor);
        output << "▀";
      }
    }

    output << "\x1b[0m\n";
  }
}

int TerminalImageViewer::calculateITermImageRows(int imageWidth, int imageHeight, int widthPercent,
                                                 const TerminalSize& terminalSize,
                                                 const TerminalCellSize& cellSize) const {
  // Calculate display width in pixels based on percentage
  const int displayWidthPixels = (terminalSize.columns * cellSize.widthPixels * widthPercent) / 100;

  // Calculate display height maintaining aspect ratio
  const double aspectRatio = static_cast<double>(imageHeight) / static_cast<double>(imageWidth);
  const int displayHeightPixels = static_cast<int>(displayWidthPixels * aspectRatio);

  // Convert to rows, rounding up
  const int rows = (displayHeightPixels + cellSize.heightPixels - 1) / cellSize.heightPixels;

  return rows;
}

void TerminalImageViewer::renderITermInlineImage(const TerminalImageView& image,
                                                 std::ostream& output,
                                                 const TerminalImageViewerConfig& config) const {
  // Encode to PNG in memory
  const std::vector<uint8_t> pngData = RendererImageIO::writeRgbaPixelsToPngMemory(
      image.data, image.width, image.height, image.strideInPixels);

  // Build the iTerm2 inline image escape sequence
  // Format: ESC ] 1337 ; File = inline=1[;name=<base64>][;width=N][;height=N] : <base64> BEL
  output << "\x1b]1337;File=inline=1";

  // Include image name if provided (must be base64-encoded per iTerm2 spec)
  if (!config.imageName.empty()) {
    const std::string encodedName = EncodeBase64Data(std::span(
        reinterpret_cast<const uint8_t*>(config.imageName.data()), config.imageName.size()));
    output << ";name=" + encodedName;
  }

  // Add size parameters (using percentage for width, auto for height for best quality)
  output << ";width=30%;height=auto:";

  // Base64 encode the PNG data
  const std::string base64Data = EncodeBase64Data(std::span(pngData));
  output << base64Data;
  output << "\a";
}

}  // namespace donner::svg
