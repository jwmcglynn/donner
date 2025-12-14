#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "donner/svg/renderer/tests/ImageComparisonTestFixture.h"

namespace donner::svg {
namespace {

void appendPixel(std::vector<uint8_t>& pixels, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 0xFF) {
  pixels.push_back(r);
  pixels.push_back(g);
  pixels.push_back(b);
  pixels.push_back(a);
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

TEST(ImageComparisonTerminalPreviewTest, RendersGridWithCaptionsAndPadding) {
  std::vector<uint8_t> actual;
  appendPixel(actual, 0xFF, 0x00, 0x00);
  appendPixel(actual, 0x00, 0x00, 0xFF);

  std::vector<uint8_t> expected;
  appendPixel(expected, 0x00, 0xFF, 0x00);
  appendPixel(expected, 0x00, 0x00, 0x00);

  std::vector<uint8_t> diff;
  appendPixel(diff, 0xFF, 0xFF, 0x00);
  appendPixel(diff, 0x00, 0x00, 0x00);

  const TerminalImageView actualView{actual.data(), 1, 2, 1};
  const TerminalImageView expectedView{expected.data(), 1, 2, 1};
  const TerminalImageView diffView{diff.data(), 1, 2, 1};

  TerminalImageViewerConfig viewerConfig{};
  viewerConfig.autoDetectCapabilities = false;
  viewerConfig.enableVscodeIntegration = false;
  viewerConfig.useTrueColor = true;

  const std::string grid = RenderTerminalComparisonGridForTesting(
      actualView, expectedView, diffView, /*maxTerminalWidth=*/80, TerminalPixelMode::kHalfPixel,
      viewerConfig);

  const std::string actualBlock = "\x1b[38;2;255;0;0m\x1b[48;2;0;0;255m▀\x1b[0m";
  const std::string expectedBlock = "\x1b[38;2;0;255;0m\x1b[48;2;0;0;0m▀\x1b[0m";
  const std::string diffBlock = "\x1b[38;2;255;255;0m\x1b[48;2;0;0;0m▀\x1b[0m";

  const std::string expectedGrid = "Actual  Expected\n" + actualBlock + "     " + expectedBlock +
                                   "     \n" + "Diff            \n" + diffBlock + "             \n";

  EXPECT_EQ(grid, expectedGrid);
}

TEST(ImageComparisonTerminalPreviewTest, SkipsPreviewWhenDisabled) {
  ImageComparisonParams params;
  params.showTerminalPreview = false;
  EXPECT_FALSE(PreviewConfigFromEnv(params).has_value());

  params.showTerminalPreview = true;
  ScopedEnvVar disablePreview("DONNER_ENABLE_TERMINAL_IMAGES", "0");
  EXPECT_FALSE(PreviewConfigFromEnv(params).has_value());
}

TEST(ImageComparisonTerminalPreviewTest, ReadsPreviewConfigFromEnvironment) {
  ImageComparisonParams params;
  params.showTerminalPreview = true;

  ScopedEnvVar enablePreview("DONNER_ENABLE_TERMINAL_IMAGES", "1");
  ScopedEnvVar forceHalf("DONNER_TERMINAL_PIXEL_MODE", "half");
  ScopedEnvVar setColumns("COLUMNS", "64");

  const std::optional<TerminalPreviewConfig> config = PreviewConfigFromEnv(params);
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->terminalWidth, 64);
  EXPECT_EQ(config->pixelMode, TerminalPixelMode::kHalfPixel);
}

}  // namespace
}  // namespace donner::svg
