/// @file
/// Tests for the embedded-SVG icon rasterizer. Beyond mask correctness, pins
/// that repeated icon renders share one renderer: on GPU backends every
/// renderer construction stands up a full WebGPU instance/adapter/device, so
/// a per-icon renderer floods startup with duplicate adapter/device creation.

#include "donner/editor/EmbeddedSvgIcon.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <span>
#include <string_view>

#include "donner/svg/renderer/Renderer.h"

#ifdef DONNER_GEODE_BACKEND_AVAILABLE
#include "donner/svg/renderer/geode/GeodeDevice.h"
#endif

namespace donner::editor {
namespace {

using ::testing::ElementsAre;

constexpr std::string_view kSquareIconSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<rect x="2" y="2" width="12" height="12" fill="#000"/></svg>)";

#ifdef DONNER_GEODE_BACKEND_AVAILABLE
constexpr std::string_view kCircleIconSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<circle cx="8" cy="8" r="6" fill="#000"/></svg>)";
#endif

std::span<const unsigned char> BytesOf(std::string_view svg) {
  return {reinterpret_cast<const unsigned char*>(svg.data()), svg.size()};
}

std::array<int, 4> PixelAt(const svg::RendererBitmap& bitmap, int x, int y) {
  const unsigned char* pixel = bitmap.pixels.data() +
                               static_cast<std::size_t>(y) * bitmap.rowBytes +
                               static_cast<std::size_t>(x) * 4u;
  return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

TEST(EmbeddedSvgIcon, RendersTintableAlphaMask) {
  const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(BytesOf(kSquareIconSvg),
                                                                          /*outputSizePx=*/16);
  ASSERT_TRUE(bitmap.has_value());
  EXPECT_EQ(bitmap->dimensions, Vector2i(16, 16));

  // Center pixel is inside the filled rect: fully opaque, RGB == alpha (white
  // mask). Corner pixel is outside: fully transparent.
  EXPECT_THAT(PixelAt(*bitmap, 8, 8), ElementsAre(255, 255, 255, 255));
  EXPECT_THAT(PixelAt(*bitmap, 0, 0), ElementsAre(0, 0, 0, 0));
}

TEST(EmbeddedSvgIcon, RejectsInvalidOutputSize) {
  EXPECT_FALSE(RenderEmbeddedSvgIcon(BytesOf(kSquareIconSvg), /*outputSizePx=*/0).has_value());
  EXPECT_FALSE(RenderEmbeddedSvgIcon(BytesOf(kSquareIconSvg), /*outputSizePx=*/-1).has_value());
}

TEST(EmbeddedSvgIcon, RejectsMalformedSvg) {
  constexpr std::string_view kMalformedSvg = "<svg><";

  EXPECT_FALSE(RenderEmbeddedSvgIcon(BytesOf(kMalformedSvg), /*outputSizePx=*/16).has_value());
}

#ifdef DONNER_GEODE_BACKEND_AVAILABLE
TEST(EmbeddedSvgIcon, RepeatedRendersShareOneHeadlessDevice) {
  // Prime the shared renderer (first call may create the one shared device).
  ASSERT_TRUE(RenderEmbeddedSvgIcon(BytesOf(kSquareIconSvg), 16).has_value());

  const int creationsAfterFirstRender = geode::GeodeDevice::headlessCreationCountForTesting();
  ASSERT_TRUE(RenderEmbeddedSvgIcon(BytesOf(kCircleIconSvg), 16).has_value());
  ASSERT_TRUE(RenderEmbeddedSvgIcon(BytesOf(kSquareIconSvg), 24).has_value());
  ASSERT_TRUE(RenderEmbeddedSvgIcon(BytesOf(kCircleIconSvg), 24).has_value());

  EXPECT_EQ(geode::GeodeDevice::headlessCreationCountForTesting(), creationsAfterFirstRender)
      << "Each embedded-icon render must reuse the shared renderer; a fresh renderer per icon "
         "creates a full WebGPU instance/adapter/device per icon (the duplicate "
         "'[Geode/wgpu-native] Adapter:' log spam at editor startup).";
}
#endif

}  // namespace
}  // namespace donner::editor
