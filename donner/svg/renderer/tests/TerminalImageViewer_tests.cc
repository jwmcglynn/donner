#include "donner/svg/renderer/TerminalImageViewer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

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

  const TerminalImageView view{pixels.data(), 4, 4, 4};

  TerminalImageViewer viewer;
  const TerminalImage sampled = viewer.sampleImage(view, TerminalPixelMode::kQuarterPixel);

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

  const TerminalImageView view{pixels.data(), 2, 3, 2};

  TerminalImageViewer viewer;
  const TerminalImage sampled = viewer.sampleImage(view, TerminalPixelMode::kHalfPixel);

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

TEST(TerminalImageViewerTest, AlphaWeightedSamplingProducesPremultipliedAverage) {
  std::vector<uint8_t> pixels;
  pixels.reserve(1 * 2 * 4);

  appendPixel(pixels, makeColor(0xFF, 0x00, 0x00, 0x80));
  appendPixel(pixels, makeColor(0x00, 0x00, 0xFF, 0x40));

  const TerminalImageView view{pixels.data(), 1, 2, 1};

  TerminalImageViewer viewer;
  const css::RGBA blended = TerminalImageViewerTestPeer::SampleRegion(viewer, view, 0, 0, 1, 2);

  EXPECT_EQ(blended, makeColor(0xAA, 0x00, 0x55, 0x60));
}

TEST(TerminalImageViewerRenderTest, WritesHalfPixelANSISequences) {
  std::vector<uint8_t> pixels;
  pixels.reserve(1 * 2 * 4);

  appendPixel(pixels, makeColor(0x10, 0x20, 0x30));
  appendPixel(pixels, makeColor(0xA0, 0xB0, 0xC0));

  const TerminalImageView view{pixels.data(), 1, 2, 1};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(view, stream, {.pixelMode = TerminalPixelMode::kHalfPixel,
                               .autoDetectCapabilities = false});

  EXPECT_EQ(stream.str(), "\x1b[38;2;16;32;48m\x1b[48;2;160;176;192m▀\x1b[0m\n");
}

TEST(TerminalImageViewerRenderTest, WritesHalfPixelWith256ColorFallback) {
  std::vector<uint8_t> pixels;
  pixels.reserve(1 * 2 * 4);

  appendPixel(pixels, makeColor(0x10, 0x20, 0x30));
  appendPixel(pixels, makeColor(0xA0, 0xB0, 0xC0));

  const TerminalImageView view{pixels.data(), 1, 2, 1};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(view, stream,
                {.pixelMode = TerminalPixelMode::kHalfPixel, .useTrueColor = false,
                 .autoDetectCapabilities = false});

  EXPECT_EQ(stream.str(), "\x1b[38;5;234m\x1b[48;5;145m▀\x1b[0m\n");
}

TEST(TerminalImageViewerRenderTest, WritesQuarterPixelANSISequencesWithGlyphs) {
  std::vector<uint8_t> pixels;
  pixels.reserve(2 * 2 * 4);

  appendPixel(pixels, makeColor(0xFF, 0xFF, 0xFF));
  appendPixel(pixels, makeColor(0xEE, 0xEE, 0xEE));
  appendPixel(pixels, makeColor(0x00, 0x00, 0x00));
  appendPixel(pixels, makeColor(0x10, 0x10, 0x10));

  const TerminalImageView view{pixels.data(), 2, 2, 2};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(view, stream, {.pixelMode = TerminalPixelMode::kQuarterPixel,
                               .autoDetectCapabilities = false});

  EXPECT_EQ(stream.str(), "\x1b[38;2;246;246;246m\x1b[48;2;8;8;8m▀\x1b[0m\n");
}

TEST(TerminalImageViewerRenderTest, WritesQuarterPixelWith256ColorFallback) {
  std::vector<uint8_t> pixels;
  pixels.reserve(2 * 2 * 4);

  appendPixel(pixels, makeColor(0xFF, 0xFF, 0xFF));
  appendPixel(pixels, makeColor(0xEE, 0xEE, 0xEE));
  appendPixel(pixels, makeColor(0x00, 0x00, 0x00));
  appendPixel(pixels, makeColor(0x10, 0x10, 0x10));

  const TerminalImageView view{pixels.data(), 2, 2, 2};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(view, stream,
                {.pixelMode = TerminalPixelMode::kQuarterPixel, .useTrueColor = false,
                 .autoDetectCapabilities = false});

  EXPECT_EQ(stream.str(), "\x1b[38;5;255m\x1b[48;5;232m▀\x1b[0m\n");
}

TEST(TerminalImageViewerCapabilityDetectionTest, DetectsVscodeAndDefaultsToTrueColor) {
  ScopedEnvVar termProgram("TERM_PROGRAM", "vscode");
  ScopedEnvVar colorTerm("COLORTERM", "");
  TerminalImageViewerTestPeer::ResetCachedCapabilities();

  const TerminalCapabilities capabilities = TerminalImageViewerTestPeer::DetectCapabilities();

  EXPECT_TRUE(capabilities.isVscodeInteractive);
  EXPECT_TRUE(capabilities.supportsTrueColor);
}

TEST(TerminalImageViewerCapabilityDetectionTest, DetectsTrueColorFromColorterm) {
  ScopedEnvVar colorTerm("COLORTERM", "truecolor");
  ScopedEnvVar termProgram("TERM_PROGRAM", "xterm");
  TerminalImageViewerTestPeer::ResetCachedCapabilities();

  const TerminalCapabilities capabilities = TerminalImageViewerTestPeer::DetectCapabilities();

  EXPECT_FALSE(capabilities.isVscodeInteractive);
  EXPECT_TRUE(capabilities.supportsTrueColor);
}

TEST(TerminalImageViewerCapabilityDetectionTest, FallsBackTo256ColorWhenUnknown) {
  ScopedEnvVar colorTerm("COLORTERM", "");
  ScopedEnvVar termProgram("TERM_PROGRAM", "xterm");
  ScopedEnvVar term("TERM", "xterm-256color");
  TerminalImageViewerTestPeer::ResetCachedCapabilities();

  const TerminalCapabilities capabilities = TerminalImageViewerTestPeer::DetectCapabilities();

  EXPECT_FALSE(capabilities.supportsTrueColor);
  EXPECT_FALSE(capabilities.isVscodeInteractive);
}

TEST(TerminalImageViewerCapabilityDetectionTest, AutoDetectionInfluencesRenderingDefaults) {
  ScopedEnvVar colorTerm("COLORTERM", "");
  ScopedEnvVar termProgram("TERM_PROGRAM", "xterm");
  ScopedEnvVar term("TERM", "xterm-256color");
  TerminalImageViewerTestPeer::ResetCachedCapabilities();

  std::vector<uint8_t> pixels;
  pixels.reserve(1 * 2 * 4);

  appendPixel(pixels, makeColor(0x10, 0x20, 0x30));
  appendPixel(pixels, makeColor(0xA0, 0xB0, 0xC0));

  const TerminalImageView view{pixels.data(), 1, 2, 1};

  TerminalImageViewer viewer;
  std::ostringstream stream;
  viewer.render(view, stream, {.pixelMode = TerminalPixelMode::kHalfPixel});

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

TEST(TerminalImageViewerRenderTest, FuzzesRandomFramesAcrossModes) {
  std::mt19937 rng(0xC0FFEEu);
  TerminalImageViewer viewer;

  for (const TerminalPixelMode mode : {TerminalPixelMode::kQuarterPixel,
                                        TerminalPixelMode::kHalfPixel}) {
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

        const TerminalImageView view{pixels.data(), width, height,
                                     static_cast<size_t>(width)};

        std::ostringstream stream;
        viewer.render(view, stream, {.pixelMode = mode, .useTrueColor = useTrueColor,
                                     .autoDetectCapabilities = false});

        const std::string output = stream.str();
        const int expectedRows = (height + 1) / 2;

        EXPECT_GE(output.size(), static_cast<size_t>(expectedRows));
        EXPECT_EQ(countOccurrences(output, '\n'), expectedRows);
        EXPECT_EQ(countSubstring(output, "\x1b[0m"), expectedRows);
      }
    }
  }
}

}  // namespace
}  // namespace donner::svg
