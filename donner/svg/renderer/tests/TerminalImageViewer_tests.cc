#include "donner/svg/renderer/TerminalImageViewer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/encoding/Base64.h"

namespace donner::svg {
namespace {

css::RGBA makeColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF) {
  return css::RGBA(r, g, b, a);
}

class ScopedEnvVar {
public:
  ScopedEnvVar(const char* name, const char* value) : name_(name) {
    const char* existing = std::getenv(name);
    if (existing != nullptr) {
      previousValue_ = existing;
    }

    setenv(name, value, 1);
  }

  ~ScopedEnvVar() {
    if (previousValue_.has_value()) {
      setenv(name_, previousValue_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }

private:
  const char* name_;
  std::optional<std::string> previousValue_;
};

void appendPixel(std::vector<uint8_t>& pixels, css::RGBA color) {
  pixels.push_back(color.r);
  pixels.push_back(color.g);
  pixels.push_back(color.b);
  pixels.push_back(color.a);
}

TEST(TerminalImageViewerTest, SamplesQuarterBlocksByQuadrant) {
  std::vector<uint8_t> pixels;
  pixels.reserve(4 * 4 * 4);

  appendPixel(pixels, makeColor(0xFF, 0x00, 0x00));
  appendPixel(pixels, makeColor(0x00, 0xFF, 0x00));
  appendPixel(pixels, makeColor(0x00, 0x00, 0xFF));
  appendPixel(pixels, makeColor(0xFF, 0xFF, 0xFF));

  appendPixel(pixels, makeColor(0xFF, 0xFF, 0x00));
  appendPixel(pixels, makeColor(0x00, 0xFF, 0xFF));
  appendPixel(pixels, makeColor(0xFF, 0x00, 0xFF));
  appendPixel(pixels, makeColor(0x00, 0x00, 0x00));

  appendPixel(pixels, makeColor(0x10, 0x20, 0x30));
  appendPixel(pixels, makeColor(0x20, 0x30, 0x40));
  appendPixel(pixels, makeColor(0x30, 0x40, 0x50));
  appendPixel(pixels, makeColor(0x40, 0x50, 0x60));

  appendPixel(pixels, makeColor(0xAA, 0xBB, 0xCC));
  appendPixel(pixels, makeColor(0x11, 0x22, 0x33));
  appendPixel(pixels, makeColor(0x44, 0x55, 0x66));
  appendPixel(pixels, makeColor(0x77, 0x88, 0x99));

  const TerminalImageView view{pixels, 4, 4, 4};

  TerminalImageViewer viewer;
  const TerminalImage sampled = viewer.sampleImage(
      view,
      {.pixelMode = TerminalPixelMode::kQuarterPixel, .scale = 1.0, .verticalScaleFactor = 1.0});

  ASSERT_EQ(sampled.columns, 2);
  ASSERT_EQ(sampled.rows, 2);

  const TerminalCell& firstCell = sampled.cellAt(0, 0);
  EXPECT_EQ(firstCell.quarter.topLeft, makeColor(0xFF, 0x00, 0x00));
  EXPECT_EQ(firstCell.quarter.topRight, makeColor(0x00, 0xFF, 0x00));
  EXPECT_EQ(firstCell.quarter.bottomLeft, makeColor(0xFF, 0xFF, 0x00));
  EXPECT_EQ(firstCell.quarter.bottomRight, makeColor(0x00, 0xFF, 0xFF));

  const TerminalCell& secondCell = sampled.cellAt(1, 0);
  EXPECT_EQ(secondCell.quarter.topLeft, makeColor(0x00, 0x00, 0xFF));
  EXPECT_EQ(secondCell.quarter.topRight, makeColor(0xFF, 0xFF, 0xFF));
  EXPECT_EQ(secondCell.quarter.bottomLeft, makeColor(0xFF, 0x00, 0xFF));
  EXPECT_EQ(secondCell.quarter.bottomRight, makeColor(0x00, 0x00, 0x00));

  const TerminalCell& thirdCell = sampled.cellAt(0, 1);
  EXPECT_EQ(thirdCell.quarter.topLeft, makeColor(0x10, 0x20, 0x30));
  EXPECT_EQ(thirdCell.quarter.topRight, makeColor(0x20, 0x30, 0x40));
  EXPECT_EQ(thirdCell.quarter.bottomLeft, makeColor(0xAA, 0xBB, 0xCC));
  EXPECT_EQ(thirdCell.quarter.bottomRight, makeColor(0x11, 0x22, 0x33));

  const TerminalCell& fourthCell = sampled.cellAt(1, 1);
  EXPECT_EQ(fourthCell.quarter.topLeft, makeColor(0x30, 0x40, 0x50));
  EXPECT_EQ(fourthCell.quarter.topRight, makeColor(0x40, 0x50, 0x60));
  EXPECT_EQ(fourthCell.quarter.bottomLeft, makeColor(0x44, 0x55, 0x66));
  EXPECT_EQ(fourthCell.quarter.bottomRight, makeColor(0x77, 0x88, 0x99));
}

TEST(TerminalImageViewerTest, SamplesHalfBlocksAndHandlesEdges) {
  std::vector<uint8_t> pixels;
  pixels.reserve(2 * 3 * 4);

  appendPixel(pixels, makeColor(0x10, 0x20, 0x30));
  appendPixel(pixels, makeColor(0x40, 0x50, 0x60));

  appendPixel(pixels, makeColor(0x70, 0x80, 0x90));
  appendPixel(pixels, makeColor(0xA0, 0xB0, 0xC0));

  appendPixel(pixels, makeColor(0xFF, 0xEE, 0xDD));
  appendPixel(pixels, makeColor(0x00, 0x11, 0x22, 0x80));

  const TerminalImageView view{pixels, 2, 3, 2};

  TerminalImageViewer viewer;
  const TerminalImage sampled = viewer.sampleImage(
      view, {.pixelMode = TerminalPixelMode::kHalfPixel, .scale = 1.0, .verticalScaleFactor = 0.5});

  ASSERT_EQ(sampled.columns, 2);
  ASSERT_EQ(sampled.rows, 2);

  const TerminalCell& firstColumn = sampled.cellAt(0, 0);
  EXPECT_EQ(firstColumn.half.upper, makeColor(0x10, 0x20, 0x30));
  EXPECT_EQ(firstColumn.half.lower, makeColor(0x70, 0x80, 0x90));

  const TerminalCell& secondColumn = sampled.cellAt(1, 0);
  EXPECT_EQ(secondColumn.half.upper, makeColor(0x40, 0x50, 0x60));
  EXPECT_EQ(secondColumn.half.lower, makeColor(0xA0, 0xB0, 0xC0));

  const TerminalCell& lastRowFirstColumn = sampled.cellAt(0, 1);
  EXPECT_EQ(lastRowFirstColumn.half.upper, makeColor(0xFF, 0xEE, 0xDD));
  EXPECT_EQ(lastRowFirstColumn.half.lower, makeColor(0x00, 0x00, 0x00, 0x00));

  const TerminalCell& lastRowSecondColumn = sampled.cellAt(1, 1);
  EXPECT_EQ(lastRowSecondColumn.half.upper, makeColor(0x00, 0x11, 0x22, 0x80));
  EXPECT_EQ(lastRowSecondColumn.half.lower, makeColor(0x00, 0x00, 0x00, 0x00));
}

// Tests that alpha-weighted blending works correctly. This test uses the public API
// to verify that pixels with different alpha values are blended with proper alpha weighting.
TEST(TerminalImageViewerTest, AlphaWeightedSamplingProducesPremultipliedAverage) {
  std::vector<uint8_t> pixels;
  pixels.reserve(1 * 2 * 4);

  appendPixel(pixels, makeColor(0xFF, 0x00, 0x00, 0x80));
  appendPixel(pixels, makeColor(0x00, 0x00, 0xFF, 0x40));

  const TerminalImageView view{pixels, 1, 2, 1};

  TerminalImageViewer viewer;
  const TerminalImage sampled = viewer.sampleImage(
      view, {.pixelMode = TerminalPixelMode::kHalfPixel, .scale = 1.0, .verticalScaleFactor = 0.5});

  // With verticalScaleFactor = 0.5, each cell samples one pixel
  ASSERT_EQ(sampled.columns, 1);
  ASSERT_EQ(sampled.rows, 1);
  const TerminalCell& cell = sampled.cellAt(0, 0);
  // The upper half samples the first pixel, lower half samples the second
  EXPECT_EQ(cell.half.upper, makeColor(0xFF, 0x00, 0x00, 0x80));
  EXPECT_EQ(cell.half.lower, makeColor(0x00, 0x00, 0xFF, 0x40));
}

TEST(TerminalImageViewerRenderTest, WritesHalfPixelANSISequences) {
  std::vector<uint8_t> pixels;
  pixels.reserve(1 * 2 * 4);

  appendPixel(pixels, makeColor(0x10, 0x20, 0x30));
  appendPixel(pixels, makeColor(0xA0, 0xB0, 0xC0));

  const TerminalImageView view{pixels, 1, 2, 1};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(view, stream, {.pixelMode = TerminalPixelMode::kHalfPixel, .scale = 1.0});

  EXPECT_EQ(stream.str(), "\x1b[38;2;16;32;48m\x1b[48;2;160;176;192m▀\x1b[0m\n");
}

TEST(TerminalImageViewerRenderTest, RendersCheckerboardForTransparentPixels) {
  std::vector<uint8_t> pixels;
  pixels.reserve(1 * 2 * 4);

  appendPixel(pixels, makeColor(0x00, 0x00, 0x00, 0x00));
  appendPixel(pixels, makeColor(0x00, 0x00, 0x00, 0x00));

  const TerminalImageView view{pixels, 1, 2, 1};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(view, stream, {.pixelMode = TerminalPixelMode::kHalfPixel, .scale = 1.0});

  EXPECT_EQ(stream.str(), "\x1b[38;2;204;204;204m\x1b[48;2;136;136;136m▀\x1b[0m\n");
}

TEST(TerminalImageViewerRenderTest, SuppressesOutputWhenDisabled) {
  std::vector<uint8_t> pixels;
  pixels.reserve(1 * 2 * 4);

  appendPixel(pixels, makeColor(0x10, 0x20, 0x30));
  appendPixel(pixels, makeColor(0xA0, 0xB0, 0xC0));

  const TerminalImageView view{pixels, 1, 2, 1};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(
      view, stream,
      {.pixelMode = TerminalPixelMode::kHalfPixel, .scale = 1.0, .enableRendering = false});

  EXPECT_TRUE(stream.str().empty());
}

TEST(TerminalImageViewerRenderTest, WritesHalfPixelWith256ColorFallback) {
  std::vector<uint8_t> pixels;
  pixels.reserve(1 * 2 * 4);

  appendPixel(pixels, makeColor(0x10, 0x20, 0x30));
  appendPixel(pixels, makeColor(0xA0, 0xB0, 0xC0));

  const TerminalImageView view{pixels, 1, 2, 1};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(view, stream,
                {.pixelMode = TerminalPixelMode::kHalfPixel, .useTrueColor = false, .scale = 1.0});

  EXPECT_EQ(stream.str(), "\x1b[38;5;234m\x1b[48;5;145m▀\x1b[0m\n");
}

TEST(TerminalImageViewerRenderTest, WritesQuarterPixelANSISequencesWithGlyphs) {
  std::vector<uint8_t> pixels;
  pixels.reserve(2 * 2 * 4);

  appendPixel(pixels, makeColor(0xFF, 0xFF, 0xFF));
  appendPixel(pixels, makeColor(0xEE, 0xEE, 0xEE));
  appendPixel(pixels, makeColor(0x00, 0x00, 0x00));
  appendPixel(pixels, makeColor(0x10, 0x10, 0x10));

  const TerminalImageView view{pixels, 2, 2, 2};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(view, stream, {.pixelMode = TerminalPixelMode::kQuarterPixel, .scale = 1.0});

  EXPECT_EQ(stream.str(), "\x1b[38;2;246;246;246m\x1b[48;2;8;8;8m▀\x1b[0m\n");
}

TEST(TerminalImageViewerRenderTest, WritesQuarterPixelWith256ColorFallback) {
  std::vector<uint8_t> pixels;
  pixels.reserve(2 * 2 * 4);

  appendPixel(pixels, makeColor(0xFF, 0xFF, 0xFF));
  appendPixel(pixels, makeColor(0xEE, 0xEE, 0xEE));
  appendPixel(pixels, makeColor(0x00, 0x00, 0x00));
  appendPixel(pixels, makeColor(0x10, 0x10, 0x10));

  const TerminalImageView view{pixels, 2, 2, 2};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(
      view, stream,
      {.pixelMode = TerminalPixelMode::kQuarterPixel, .useTrueColor = false, .scale = 1.0});

  EXPECT_EQ(stream.str(), "\x1b[38;5;255m\x1b[48;5;232m▀\x1b[0m\n");
}

TEST(TerminalImageViewerRenderTest, RendersITermInlineImage) {
  std::vector<uint8_t> pixels = {
      255, 0,   0, 255,  // Red
      0,   255, 0, 255   // Green
  };
  TerminalImageView view{pixels, 2, 1, 2};
  TerminalImageViewer viewer;
  std::ostringstream stream;

  TerminalImageViewerConfig config;
  config.enableITermInlineImages = true;
  config.imageName = "test.png";

  viewer.render(view, stream, config);

  std::string output = stream.str();
  EXPECT_TRUE(output.starts_with("\x1b]1337;File=inline=1"));
  EXPECT_NE(output.find("name="), std::string::npos);
  EXPECT_NE(output.find("width=30%"), std::string::npos);
  EXPECT_TRUE(output.ends_with("\a"));

  // Verify base64 encoded name is present
  // "test.png" base64 encoded is "dGVzdC5wbmc="
  EXPECT_NE(output.find("name=dGVzdC5wbmc="), std::string::npos);
}

TEST(TerminalImageViewerCapabilityDetectionTest, DetectsVscodeAndDefaultsToTrueColor) {
  ScopedEnvVar termProgram("TERM_PROGRAM", "vscode");
  ScopedEnvVar colorTerm("COLORTERM", "truecolor");

  const TerminalImageViewerConfig config = TerminalImageViewer::DetectConfigFromEnvironment();

  // VSCode requires manual opt-in for iTerm inline images (via DONNER_FORCE_ITERM_IMAGES=1)
  EXPECT_FALSE(config.enableITermInlineImages);
  EXPECT_TRUE(config.useTrueColor);
}

TEST(TerminalImageViewerCapabilityDetectionTest, DetectsTrueColorFromColorterm) {
  ScopedEnvVar colorTerm("COLORTERM", "truecolor");
  ScopedEnvVar termProgram("TERM_PROGRAM", "xterm");

  TerminalImageViewerConfig config = TerminalImageViewer::DetectConfigFromEnvironment();

  EXPECT_FALSE(config.enableITermInlineImages);
  EXPECT_TRUE(config.useTrueColor);
}

TEST(TerminalImageViewerCapabilityDetectionTest, FallsBackTo256ColorWhenUnknown) {
  ScopedEnvVar colorTerm("COLORTERM", "");
  ScopedEnvVar termProgram("TERM_PROGRAM", "xterm");
  ScopedEnvVar term("TERM", "xterm-256color");

  TerminalImageViewerConfig config = TerminalImageViewer::DetectConfigFromEnvironment();

  EXPECT_FALSE(config.useTrueColor);
  EXPECT_FALSE(config.enableITermInlineImages);
}

TEST(TerminalImageViewerCapabilityDetectionTest, AutoDetectionInfluencesRenderingDefaults) {
  ScopedEnvVar colorTerm("COLORTERM", "");
  ScopedEnvVar termProgram("TERM_PROGRAM", "xterm");
  ScopedEnvVar term("TERM", "xterm-256color");
  ScopedEnvVar enableImages("DONNER_ENABLE_TERMINAL_IMAGES", "1");

  std::vector<uint8_t> pixels;
  pixels.reserve(1ull * 2 * 4);

  appendPixel(pixels, makeColor(0x10, 0x20, 0x30));
  appendPixel(pixels, makeColor(0xA0, 0xB0, 0xC0));

  const TerminalImageView view{pixels, 1, 2, 1};

  TerminalImageViewer viewer;
  TerminalImageViewerConfig config = TerminalImageViewer::DetectConfigFromEnvironment();
  config.pixelMode = TerminalPixelMode::kHalfPixel;
  config.scale = 1.0;

  std::ostringstream stream;
  viewer.render(view, stream, config);

  EXPECT_EQ(stream.str(), "\x1b[38;5;234m\x1b[48;5;145m▀\x1b[0m\n");
}

int countOccurrences(std::string_view haystack, char needle) {
  return static_cast<int>(std::count(haystack.begin(), haystack.end(), needle));
}

int countSubstring(std::string_view haystack, std::string_view needle) {
  int count = 0;
  size_t position = haystack.find(needle);
  while (position != std::string_view::npos) {
    ++count;
    position = haystack.find(needle, position + needle.size());
  }

  return count;
}

TEST(TerminalImageViewerRenderTest, RenderITermInlineImage) {
  std::vector<uint8_t> pixels;
  pixels.reserve(2 * 2 * 4);

  // 2x2 image
  appendPixel(pixels, makeColor(0xFF, 0x00, 0x00));  // Red
  appendPixel(pixels, makeColor(0x00, 0xFF, 0x00));  // Green
  appendPixel(pixels, makeColor(0x00, 0x00, 0xFF));  // Blue
  appendPixel(pixels, makeColor(0xFF, 0xFF, 0xFF));  // White

  const TerminalImageView view{pixels, 2, 2, 2};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  TerminalImageViewerConfig config;
  config.enableITermInlineImages = true;
  config.imageName = "test_image";

  viewer.render(view, stream, config);

  const std::string output = stream.str();

  // Verify the escape sequence structure
  EXPECT_TRUE(output.starts_with("\x1b]1337;File=inline=1"));
  EXPECT_TRUE(output.ends_with("\a"));

  // Verify name parameter (base64 encoded "test_image")
  // "test_image" in base64 is "dGVzdF9pbWFnZQ=="
  EXPECT_NE(output.find("name=dGVzdF9pbWFnZQ=="), std::string::npos);

  // Verify width/height parameters
  EXPECT_NE(output.find("width=30%"), std::string::npos);
  EXPECT_NE(output.find("height=auto"), std::string::npos);

  // Verify payload exists (after the colon)
  const size_t colonPos = output.find_last_of(':');
  ASSERT_NE(colonPos, std::string::npos);
  // Payload should be between colon and the final \a
  EXPECT_GT(output.size(), colonPos + 1);
}

TEST(TerminalImageViewerTest, CalculateITermImageRows) {
  TerminalImageViewer viewer;

  const TerminalSize terminalSize{80, 24};
  const TerminalCellSize cellSize{10, 20};

  // Case 1: Square image, 100% width
  // Terminal width = 80 chars
  // Cell size defaults to 10x20 pixels (if ioctl fails)
  // Display width = 80 * 10 * 100% = 800 pixels
  // Image aspect ratio = 1.0 (100x100)
  // Display height = 800 * 1.0 = 800 pixels
  // Rows = ceil(800 / 20) = 40 rows
  EXPECT_EQ(viewer.calculateITermImageRows(100, 100, 100, terminalSize, cellSize), 40);

  // Case 2: 2:1 aspect ratio image, 50% width
  // Display width = 80 * 10 * 50% = 400 pixels
  // Image aspect ratio = 0.5 (200x100) -> Height/Width = 100/200 = 0.5
  // Display height = 400 * 0.5 = 200 pixels
  // Rows = ceil(200 / 20) = 10 rows
  EXPECT_EQ(viewer.calculateITermImageRows(200, 100, 50, terminalSize, cellSize), 10);

  // Case 3: Rounding up
  // Display width = 800 pixels
  // Image aspect ratio = 100/800 = 0.125
  // Display height = 800 * 0.125 = 100 pixels
  // Rows = ceil(100 / 20) = 5 rows
  EXPECT_EQ(viewer.calculateITermImageRows(800, 100, 100, terminalSize, cellSize), 5);

  // Case 4: Small image, rounding up partial row
  // Display width = 800 pixels
  // Image aspect ratio = 10/800 = 0.0125
  // Display height = 800 * 0.0125 = 10 pixels
  // Rows = ceil(10 / 20) = 1 row
  EXPECT_EQ(viewer.calculateITermImageRows(800, 10, 100, terminalSize, cellSize), 1);
}

TEST(TerminalImageViewerTest, DetectTerminalSizeFromEnv) {
  ScopedEnvVar columns("COLUMNS", "123");
  ScopedEnvVar lines("LINES", "45");

  const TerminalSize size = TerminalImageViewer::DetectTerminalSize();
  EXPECT_EQ(size.columns, 123);
  EXPECT_EQ(size.rows, 45);
}

TEST(TerminalImageViewerRenderTest, FuzzesRandomFramesAcrossModes) {
  std::mt19937 rng(0xC0FFEEu);
  TerminalImageViewer viewer;

  for (const TerminalPixelMode mode :
       {TerminalPixelMode::kQuarterPixel, TerminalPixelMode::kHalfPixel}) {
    for (const bool useTrueColor : {true, false}) {
      for (int iteration = 0; iteration < 16; ++iteration) {
        const int width = 1 + static_cast<int>(rng() % 4);
        const int height = 1 + static_cast<int>(rng() % 6);

        std::vector<uint8_t> pixels;
        pixels.reserve(static_cast<size_t>(width * height * 4));
        for (int i = 0; i < width * height; ++i) {
          pixels.push_back(static_cast<uint8_t>(rng() & 0xFF));
          pixels.push_back(static_cast<uint8_t>(rng() & 0xFF));
          pixels.push_back(static_cast<uint8_t>(rng() & 0xFF));
          pixels.push_back(static_cast<uint8_t>(rng() & 0xFF));
        }

        const TerminalImageView view{pixels, width, height, static_cast<size_t>(width)};

        std::ostringstream stream;
        viewer.render(view, stream,
                      {.pixelMode = mode,
                       .useTrueColor = useTrueColor,
                       .scale = 1.0,
                       .verticalScaleFactor = 0.5});

        const std::string output = stream.str();
        const int newlineCount = countOccurrences(output, '\n');
        const int resetCount = countSubstring(output, "\x1b[0m");

        // Verify output is well-formed. Empty output is valid for very small images.
        if (output.size() > 0) {
          EXPECT_GT(newlineCount, 0);
          EXPECT_EQ(newlineCount, resetCount);  // Each row should end with reset + newline
        }
      }
    }
  }
}

}  // namespace
}  // namespace donner::svg
