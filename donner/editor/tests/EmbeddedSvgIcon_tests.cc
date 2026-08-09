/// @file
/// Tests for the embedded-SVG icon rasterizer. Beyond mask correctness, pins
/// that CPU-bound icon generation never creates a Geode device or pays a GPU
/// readback during editor startup.

#include "donner/editor/EmbeddedSvgIcon.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#ifdef DONNER_GEODE_BACKEND_AVAILABLE
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#endif

namespace donner::editor {
namespace {

using ::testing::ElementsAre;

constexpr std::string_view kSquareIconSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<rect x="2" y="2" width="12" height="12" fill="#000"/></svg>)";

constexpr std::string_view kTwoToneIconSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<rect x="2" y="2" width="6" height="12" fill="#000"/>)"
    R"(<rect x="8" y="2" width="6" height="12" fill="#fff"/></svg>)";

#ifdef DONNER_GEODE_BACKEND_AVAILABLE
constexpr std::string_view kCircleIconSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<circle cx="8" cy="8" r="6" fill="#000"/></svg>)";
#endif

std::span<const unsigned char> BytesOf(std::string_view svg) {
  return {reinterpret_cast<const unsigned char*>(svg.data()), svg.size()};
}

constexpr std::string_view kTriangleIconSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<path d="M8 2 L14 14 L2 14 Z" fill="#000"/></svg>)";

constexpr std::string_view kRingIconSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">)"
    R"(<circle cx="8" cy="8" r="5" fill="none" stroke="#000" stroke-width="2"/></svg>)";

std::array<int, 4> PixelAt(const svg::RendererBitmap& bitmap, int x, int y) {
  const unsigned char* pixel = bitmap.pixels.data() +
                               static_cast<std::size_t>(y) * bitmap.rowBytes +
                               static_cast<std::size_t>(x) * 4u;
  return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

#ifdef DONNER_GEODE_BACKEND_AVAILABLE
struct ScopedEmbeddedIconRendererDeviceReset {
  svg::RendererInterface* renderer = nullptr;

  ~ScopedEmbeddedIconRendererDeviceReset() {
    if (renderer != nullptr) {
      ResetEmbeddedSvgIconRenderer(*renderer);
    }
  }
};

TEST(EmbeddedSvgIcon, ConfiguredDeviceAvoidsASecondHeadlessDeviceAndPreservesPixels) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  if (device == nullptr) {
    GTEST_SKIP() << "A Geode device is unavailable on this host";
  }

  svg::Renderer renderer(device);
  ConfigureEmbeddedSvgIconRenderer(renderer);
  const ScopedEmbeddedIconRendererDeviceReset reset{&renderer};
  const int creationsBeforeIcons = geode::GeodeDevice::headlessCreationCountForTesting();

  const std::optional<svg::RendererBitmap> mask =
      RenderEmbeddedSvgIcon(BytesOf(kSquareIconSvg), 16);
  const std::optional<svg::RendererBitmap> artwork =
      RenderEmbeddedSvgArtwork(BytesOf(kTwoToneIconSvg), 16);

  ASSERT_TRUE(mask.has_value());
  ASSERT_TRUE(artwork.has_value());
  EXPECT_THAT(PixelAt(*mask, 8, 8), ElementsAre(255, 255, 255, 255));
  EXPECT_THAT(PixelAt(*mask, 0, 0), ElementsAre(0, 0, 0, 0));
  EXPECT_THAT(PixelAt(*artwork, 4, 8), ElementsAre(0, 0, 0, 255));
  EXPECT_THAT(PixelAt(*artwork, 11, 8), ElementsAre(255, 255, 255, 255));
  EXPECT_EQ(geode::GeodeDevice::headlessCreationCountForTesting(), creationsBeforeIcons)
      << "Embedded editor icons must reuse the configured editor device instead of standing up "
         "another WebGPU instance/adapter/device.";
}

TEST(EmbeddedSvgIcon, ResetFromOlderOwnerDoesNotClearNewerRenderer) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  if (device == nullptr) {
    GTEST_SKIP() << "A Geode device is unavailable on this host";
  }

  svg::Renderer olderRenderer(device);
  svg::Renderer newerRenderer(device);
  ConfigureEmbeddedSvgIconRenderer(olderRenderer);
  ConfigureEmbeddedSvgIconRenderer(newerRenderer);
  const ScopedEmbeddedIconRendererDeviceReset reset{&newerRenderer};

  ResetEmbeddedSvgIconRenderer(olderRenderer);
  const int creationsBeforeIcon = geode::GeodeDevice::headlessCreationCountForTesting();
  const std::optional<svg::RendererBitmap> bitmap =
      RenderEmbeddedSvgIcon(BytesOf(kCircleIconSvg), 16);

  ASSERT_TRUE(bitmap.has_value());
  EXPECT_EQ(geode::GeodeDevice::headlessCreationCountForTesting(), creationsBeforeIcon)
      << "An older EditorShell destructor must not clear a renderer installed by a newer shell.";
}
#endif

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

TEST(EmbeddedSvgIcon, PreservesAuthoredArtworkColors) {
  const std::optional<svg::RendererBitmap> bitmap =
      RenderEmbeddedSvgArtwork(BytesOf(kTwoToneIconSvg), /*outputSizePx=*/16);
  ASSERT_TRUE(bitmap.has_value());
  EXPECT_EQ(bitmap->dimensions, Vector2i(16, 16));
  EXPECT_THAT(PixelAt(*bitmap, 4, 8), ElementsAre(0, 0, 0, 255));
  EXPECT_THAT(PixelAt(*bitmap, 11, 8), ElementsAre(255, 255, 255, 255));
  EXPECT_THAT(PixelAt(*bitmap, 0, 0), ElementsAre(0, 0, 0, 0));
}

TEST(EmbeddedSvgIcon, RejectsInvalidOutputSize) {
  EXPECT_FALSE(RenderEmbeddedSvgIcon(BytesOf(kSquareIconSvg), /*outputSizePx=*/0).has_value());
  EXPECT_FALSE(RenderEmbeddedSvgIcon(BytesOf(kSquareIconSvg), /*outputSizePx=*/-1).has_value());
  EXPECT_FALSE(RenderEmbeddedSvgArtwork(BytesOf(kSquareIconSvg), /*outputSizePx=*/0).has_value());
}

TEST(EmbeddedSvgIcon, RejectsMalformedSvg) {
  constexpr std::string_view kMalformedSvg = "<svg><";

  EXPECT_FALSE(RenderEmbeddedSvgIcon(BytesOf(kMalformedSvg), /*outputSizePx=*/16).has_value());
  EXPECT_FALSE(RenderEmbeddedSvgArtwork(BytesOf(kMalformedSvg), /*outputSizePx=*/16).has_value());
}

#ifdef DONNER_GEODE_BACKEND_AVAILABLE
TEST(EmbeddedSvgIcon, RepeatedRendersDoNotCreateGeodeDevices) {
  const int creationsBeforeRendering = geode::GeodeDevice::headlessCreationCountForTesting();
  ASSERT_TRUE(RenderEmbeddedSvgIcon(BytesOf(kSquareIconSvg), 16).has_value());
  ASSERT_TRUE(RenderEmbeddedSvgIcon(BytesOf(kCircleIconSvg), 16).has_value());
  ASSERT_TRUE(RenderEmbeddedSvgIcon(BytesOf(kSquareIconSvg), 24).has_value());
  ASSERT_TRUE(RenderEmbeddedSvgIcon(BytesOf(kCircleIconSvg), 24).has_value());

  EXPECT_EQ(geode::GeodeDevice::headlessCreationCountForTesting(), creationsBeforeRendering)
      << "Embedded icons are CPU bitmaps and must stay on TinySkia instead of creating or "
         "synchronously reading back a Geode device.";
}
#endif

/// Largest per-channel difference tolerated between a standalone render and the
/// same icon sliced out of an atlas.
///
/// Geode - the backend the editor rasterizes icons with, and the one this
/// change exists for - is byte-exact: coverage is analytic, so translating a
/// tile by whole pixels and enlarging the target cannot move a single byte.
/// TinySkia's supersampled scan conversion is position-dependent instead: the
/// same curved edge drawn at a different offset in a differently-sized target
/// lands a few 1/255 steps away. Measured on a 2 px stroked ring at 16 px that
/// is at most 4/255, on under a tenth of the pixels, all of them on the
/// antialiased edge; straight axis-aligned artwork is unaffected. The allowance
/// covers that. It is not a licence for a real appearance change, which would
/// move interior pixels and blow well past it.
#ifdef DONNER_GEODE_BACKEND_AVAILABLE
constexpr int kAtlasChannelTolerance = 0;
#else
constexpr int kAtlasChannelTolerance = 8;
#endif

/// Compare two bitmaps by visible content: dimensions, alpha interpretation,
/// and every pixel. Row padding differs between a full-target readback and an
/// atlas slice, so `rowBytes` is honored rather than compared.
::testing::AssertionResult BitmapsMatch(const svg::RendererBitmap& expected,
                                        const svg::RendererBitmap& actual) {
  if (expected.dimensions != actual.dimensions) {
    return ::testing::AssertionFailure()
           << "dimensions differ: " << expected.dimensions << " vs " << actual.dimensions;
  }
  if (expected.alphaType != actual.alphaType) {
    return ::testing::AssertionFailure() << "alphaType differs";
  }

  int differingPixels = 0;
  int maxDelta = 0;
  int firstX = 0;
  int firstY = 0;
  std::array<int, 4> firstExpected = {};
  std::array<int, 4> firstActual = {};
  for (int y = 0; y < expected.dimensions.y; ++y) {
    for (int x = 0; x < expected.dimensions.x; ++x) {
      const std::array<int, 4> expectedPixel = PixelAt(expected, x, y);
      const std::array<int, 4> actualPixel = PixelAt(actual, x, y);
      int pixelDelta = 0;
      for (std::size_t channel = 0; channel < expectedPixel.size(); ++channel) {
        pixelDelta = std::max(pixelDelta, std::abs(expectedPixel[channel] - actualPixel[channel]));
      }
      if (pixelDelta == 0) {
        continue;
      }
      if (differingPixels == 0) {
        firstX = x;
        firstY = y;
        firstExpected = expectedPixel;
        firstActual = actualPixel;
      }
      ++differingPixels;
      maxDelta = std::max(maxDelta, pixelDelta);
    }
  }

  if (maxDelta <= kAtlasChannelTolerance) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << differingPixels << " of " << (expected.dimensions.x * expected.dimensions.y)
         << " pixels differ, max channel delta " << maxDelta << " (tolerance "
         << kAtlasChannelTolerance << "); first at (" << firstX << ", " << firstY << "): ["
         << firstExpected[0] << ", " << firstExpected[1] << ", " << firstExpected[2] << ", "
         << firstExpected[3] << "] vs [" << firstActual[0] << ", " << firstActual[1] << ", "
         << firstActual[2] << ", " << firstActual[3] << "]";
}

// An atlas tile must be indistinguishable from the icon rendered on its own:
// the batch exists only to trade N GPU readbacks for one, never to change what
// the editor draws. Sizes differ across tiles so the shelf layout is exercised.
TEST(EmbeddedSvgIcon, AtlasSlicesMatchStandaloneRenders) {
  const std::optional<svg::RendererBitmap> standaloneTriangle =
      RenderEmbeddedSvgIcon(BytesOf(kTriangleIconSvg), /*outputSizePx=*/32);
  const std::optional<svg::RendererBitmap> standaloneRing =
      RenderEmbeddedSvgIcon(BytesOf(kRingIconSvg), /*outputSizePx=*/16);
  const std::optional<svg::RendererBitmap> standaloneArtwork =
      RenderEmbeddedSvgArtwork(BytesOf(kTwoToneIconSvg), /*outputSizePx=*/24);
  ASSERT_TRUE(standaloneTriangle.has_value());
  ASSERT_TRUE(standaloneRing.has_value());
  ASSERT_TRUE(standaloneArtwork.has_value());

  const std::array<EmbeddedSvgIconRequest, 3> requests = {{
      {BytesOf(kTriangleIconSvg), 32, /*tintableMask=*/true},
      {BytesOf(kRingIconSvg), 16, /*tintableMask=*/true},
      {BytesOf(kTwoToneIconSvg), 24, /*tintableMask=*/false},
  }};
  const std::vector<std::optional<svg::RendererBitmap>> batched =
      RenderEmbeddedSvgIconBatch(requests);

  ASSERT_EQ(batched.size(), requests.size());
  ASSERT_TRUE(batched[0].has_value());
  ASSERT_TRUE(batched[1].has_value());
  ASSERT_TRUE(batched[2].has_value());
  EXPECT_TRUE(BitmapsMatch(*standaloneTriangle, *batched[0]));
  EXPECT_TRUE(BitmapsMatch(*standaloneRing, *batched[1]));
  EXPECT_TRUE(BitmapsMatch(*standaloneArtwork, *batched[2]));
}

// The batch has to survive requests it cannot render without dropping the ones
// it can: a bad tile must not shift the atlas slices of its neighbors.
TEST(EmbeddedSvgIcon, AtlasSkipsUnrenderableRequestsInPlace) {
  constexpr std::string_view kMalformedSvg = "<svg><";

  const std::array<EmbeddedSvgIconRequest, 4> requests = {{
      {BytesOf(kMalformedSvg), 16, /*tintableMask=*/true},
      {BytesOf(kTriangleIconSvg), 16, /*tintableMask=*/true},
      {BytesOf(kRingIconSvg), 0, /*tintableMask=*/true},
      {BytesOf(kRingIconSvg), 16, /*tintableMask=*/true},
  }};
  const std::vector<std::optional<svg::RendererBitmap>> batched =
      RenderEmbeddedSvgIconBatch(requests);

  ASSERT_EQ(batched.size(), requests.size());
  EXPECT_FALSE(batched[0].has_value());
  EXPECT_FALSE(batched[2].has_value());
  ASSERT_TRUE(batched[1].has_value());
  ASSERT_TRUE(batched[3].has_value());

  const std::optional<svg::RendererBitmap> standaloneTriangle =
      RenderEmbeddedSvgIcon(BytesOf(kTriangleIconSvg), /*outputSizePx=*/16);
  const std::optional<svg::RendererBitmap> standaloneRing =
      RenderEmbeddedSvgIcon(BytesOf(kRingIconSvg), /*outputSizePx=*/16);
  ASSERT_TRUE(standaloneTriangle.has_value());
  ASSERT_TRUE(standaloneRing.has_value());
  EXPECT_TRUE(BitmapsMatch(*standaloneTriangle, *batched[1]));
  EXPECT_TRUE(BitmapsMatch(*standaloneRing, *batched[3]));
}

// Prewarming is what removes the readbacks from the boot path: the per-icon
// calls the panels already make have to be served from the batch.
TEST(EmbeddedSvgIcon, PrewarmedIconsServeLaterSingleIconCalls) {
  const std::optional<svg::RendererBitmap> standalone =
      RenderEmbeddedSvgIcon(BytesOf(kTriangleIconSvg), /*outputSizePx=*/20);
  ASSERT_TRUE(standalone.has_value());

  const std::array<EmbeddedSvgIconRequest, 2> requests = {{
      {BytesOf(kTriangleIconSvg), 20, /*tintableMask=*/true},
      {BytesOf(kTwoToneIconSvg), 20, /*tintableMask=*/false},
  }};
  PrewarmEmbeddedSvgIcons(requests);

  const std::optional<svg::RendererBitmap> served =
      RenderEmbeddedSvgIcon(BytesOf(kTriangleIconSvg), /*outputSizePx=*/20);
  ASSERT_TRUE(served.has_value());
  EXPECT_TRUE(BitmapsMatch(*standalone, *served));

  // The artwork request keeps its authored colors, so a prewarmed artwork entry
  // must not be confused with the tintable-mask entry for the same asset.
  const std::optional<svg::RendererBitmap> servedArtwork =
      RenderEmbeddedSvgArtwork(BytesOf(kTwoToneIconSvg), /*outputSizePx=*/20);
  ASSERT_TRUE(servedArtwork.has_value());
  EXPECT_THAT(PixelAt(*servedArtwork, 6, 12), ElementsAre(0, 0, 0, 255));
  EXPECT_THAT(PixelAt(*servedArtwork, 15, 12), ElementsAre(255, 255, 255, 255));
}

}  // namespace
}  // namespace donner::editor
