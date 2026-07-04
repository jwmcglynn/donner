/// @file
/// Golden-image tests for the renderer API used by Layers panel thumbnails.

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/tests/Runfiles.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/editor/tests/BitmapTestMatchers.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor::tests {
namespace {

constexpr Vector2i kLayerThumbnailMaxSize(42, 24);

struct LayerGoldenCase {
  std::string_view selector;
  std::string_view label;
  std::string_view goldenPath;
};

constexpr std::array<LayerGoldenCase, 10> kDonnerSplashLayerCases = {{
    {"svg > g", "root_group",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_root_group.png"},
    {"#Background", "Background",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_background.png"},
    {"#Sunburst", "Sunburst",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_sunburst.png"},
    {"#Background_sticker", "Background_sticker",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_background_sticker.png"},
    {"#Blue_center_burst", "Blue_center_burst",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_blue_center_burst.png"},
    {"#Lightning_logo_hit_bursts", "Lightning_logo_hit_bursts",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_lightning_logo_hit_bursts.png"},
    {"#Donner", "Donner", "donner/editor/tests/testdata/layer_thumbnails/donner_splash_donner.png"},
    {"#Donner_line", "Donner_line",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_donner_line.png"},
    {"#Lightning_glow_dark", "Lightning_glow_dark",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_lightning_glow_dark.png"},
    {"#Lightning_glow_bright", "Lightning_glow_bright",
     "donner/editor/tests/testdata/layer_thumbnails/donner_splash_lightning_glow_bright.png"},
}};

std::string ReadFixture(std::string_view path) {
  std::ifstream input{donner::Runfiles::instance().Rlocation(std::string(path))};
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

svg::SVGDocument ParseDonnerSplash() {
  const std::string source = ReadFixture("donner_splash.svg");
  EXPECT_FALSE(source.empty());

  ParseWarningSink warningSink = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(source, warningSink);
  EXPECT_FALSE(parsed.hasError()) << parsed.error();
  return std::move(parsed.result());
}

}  // namespace

TEST(LayerThumbnailGoldenTest, DonnerSplashLayerThumbnailsMatchGoldens) {
  svg::SVGDocument document = ParseDonnerSplash();
  svg::Renderer renderer;

  for (const LayerGoldenCase& testCase : kDonnerSplashLayerCases) {
    std::optional<svg::SVGElement> element = document.querySelector(testCase.selector);
    ASSERT_TRUE(element.has_value()) << "Missing layer selector " << testCase.selector;

    const svg::RendererBitmap bitmap =
        renderer.renderElementToBitmap(*element, kLayerThumbnailMaxSize);
    ASSERT_THAT(bitmap, NonEmptyRendererBitmap())
        << "Layer " << testCase.label << " produced an empty thumbnail";

    CompareBitmapToGolden(bitmap, testCase.goldenPath, testCase.label, PixelmatchIdentityParams());
  }
}

}  // namespace donner::editor::tests
