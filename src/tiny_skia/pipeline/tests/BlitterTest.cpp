#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "tiny_skia/BlendMode.h"
#include "tiny_skia/Mask.h"
#include "tiny_skia/Pixmap.h"
#include "tiny_skia/pipeline/Blitter.h"

namespace {

using testing::ElementsAre;
using testing::Eq;

[[nodiscard]] std::uint8_t alphaAt(const tiny_skia::Pixmap& pixmap, std::uint32_t x,
                                   std::uint32_t y) {
  const auto bytes = pixmap.data();
  return bytes[(y * pixmap.width() + x) * 4 + 3];
}

[[nodiscard]] std::uint8_t channelAt(const tiny_skia::Pixmap& pixmap, std::uint32_t x,
                                     std::uint32_t y, std::size_t channel) {
  const auto bytes = pixmap.data();
  return bytes[(y * pixmap.width() + x) * 4 + channel];
}

TEST(PipelineBlitterTest, CreateMaskRejectsNullPixmap) {
  EXPECT_THAT(tiny_skia::pipeline::RasterPipelineBlitter::createMask(nullptr), Eq(std::nullopt));
}

TEST(PipelineBlitterTest, CreateRejectsNullPixmap) {
  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 200u);
  EXPECT_THAT(tiny_skia::pipeline::RasterPipelineBlitter::create(color, nullptr), Eq(std::nullopt));
}

TEST(PipelineBlitterTest, CreateRejectsMaskSizeMismatch) {
  auto pixmap = tiny_skia::Pixmap::fromSize(2, 2);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto mask = tiny_skia::Mask::fromSize(1, 2);
  ASSERT_TRUE(mask.has_value());

  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 200u);
  EXPECT_THAT(tiny_skia::pipeline::RasterPipelineBlitter::create(
                  color, &sub, tiny_skia::BlendMode::SourceOver, mask->submask()),
              Eq(std::nullopt));
}

TEST(PipelineBlitterTest, CreateWithExternalMaskModulatesRectAlpha) {
  auto pixmap = tiny_skia::Pixmap::fromSize(2, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto mask = tiny_skia::Mask::fromVec(std::vector<std::uint8_t>{0u, 128u},
                                       tiny_skia::IntSize::fromWH(2, 1).value());
  ASSERT_TRUE(mask.has_value());

  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 200u);
  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::create(
      color, &sub, tiny_skia::BlendMode::SourceOver, mask->submask());
  ASSERT_TRUE(blitter.has_value());
  EXPECT_FALSE(blitter->isMaskOnly());

  blitter->blitRect(tiny_skia::ScreenIntRect::fromXYWHSafe(0, 0, 2u, 1u));

  // Pipeline lowp div255: (v+255)>>8 ≈ v/256 (not exact v/255).
  EXPECT_THAT(alphaAt(*pixmap, 0, 0), Eq(0u));
  EXPECT_THAT(alphaAt(*pixmap, 1, 0), Eq(100u));
}

TEST(PipelineBlitterTest, CreateBlitMaskWritesColorWithMaskCoverage) {
  auto pixmap = tiny_skia::Pixmap::fromSize(2, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto mask = tiny_skia::Mask::fromVec(std::vector<std::uint8_t>{64u, 255u},
                                       tiny_skia::IntSize::fromWH(2, 1).value());
  ASSERT_TRUE(mask.has_value());

  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 200u);
  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::create(color, &sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitMask(*mask, tiny_skia::ScreenIntRect::fromXYWHSafe(0, 0, 2u, 1u));

  EXPECT_THAT(alphaAt(*pixmap, 0, 0), Eq(50u));
  EXPECT_THAT(alphaAt(*pixmap, 1, 0), Eq(200u));
}

TEST(PipelineBlitterTest, CreateBlitMaskCombinesWithExternalMaskCoverage) {
  auto pixmap = tiny_skia::Pixmap::fromSize(2, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto external = tiny_skia::Mask::fromVec(std::vector<std::uint8_t>{255u, 128u},
                                           tiny_skia::IntSize::fromWH(2, 1).value());
  ASSERT_TRUE(external.has_value());
  auto incoming = tiny_skia::Mask::fromVec(std::vector<std::uint8_t>{255u, 255u},
                                           tiny_skia::IntSize::fromWH(2, 1).value());
  ASSERT_TRUE(incoming.has_value());

  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 200u);
  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::create(
      color, &sub, tiny_skia::BlendMode::SourceOver, external->submask());
  ASSERT_TRUE(blitter.has_value());

  blitter->blitMask(*incoming, tiny_skia::ScreenIntRect::fromXYWHSafe(0, 0, 2u, 1u));

  EXPECT_THAT(alphaAt(*pixmap, 0, 0), Eq(200u));
  EXPECT_THAT(alphaAt(*pixmap, 1, 0), Eq(100u));
}

TEST(PipelineBlitterTest, CreateBlitMaskPartialCoverageComposesAcrossPasses) {
  auto pixmap = tiny_skia::Pixmap::fromSize(1, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto incoming = tiny_skia::Mask::fromVec(std::vector<std::uint8_t>{255u},
                                           tiny_skia::IntSize::fromWH(1, 1).value());
  ASSERT_TRUE(incoming.has_value());

  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 100u);
  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::create(color, &sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitMask(*incoming, tiny_skia::ScreenIntRect::fromXYWHSafe(0, 0, 1u, 1u));
  blitter->blitMask(*incoming, tiny_skia::ScreenIntRect::fromXYWHSafe(0, 0, 1u, 1u));

  EXPECT_THAT(alphaAt(*pixmap, 0, 0), Eq(161u));
}

TEST(PipelineBlitterTest, CreateBlitVPartialCoverageSetsColorAndComposesAlpha) {
  auto pixmap = tiny_skia::Pixmap::fromSize(1, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 200u);
  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::create(color, &sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitV(0, 0, 1u, 128u);
  blitter->blitV(0, 0, 1u, 128u);

  EXPECT_THAT(channelAt(*pixmap, 0, 0, 0), Eq(8u));
  EXPECT_THAT(channelAt(*pixmap, 0, 0, 1), Eq(16u));
  EXPECT_THAT(channelAt(*pixmap, 0, 0, 2), Eq(24u));
  EXPECT_THAT(alphaAt(*pixmap, 0, 0), Eq(161u));
}

TEST(PipelineBlitterTest, CreateBlitVOpaqueCoverageComposesAcrossPasses) {
  auto pixmap = tiny_skia::Pixmap::fromSize(1, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 200u);
  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::create(color, &sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitV(0, 0, 1u, 255u);
  blitter->blitV(0, 0, 1u, 255u);

  EXPECT_THAT(alphaAt(*pixmap, 0, 0), Eq(243u));
}

TEST(PipelineBlitterTest, CreateBlitAntiH2PartialCoverageSetsColorAndAlpha) {
  auto pixmap = tiny_skia::Pixmap::fromSize(2, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 200u);
  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::create(color, &sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitAntiH2(0, 0, 64u, 200u);

  EXPECT_THAT(channelAt(*pixmap, 0, 0, 0), Eq(3u));
  EXPECT_THAT(channelAt(*pixmap, 1, 0, 0), Eq(8u));
  EXPECT_THAT(alphaAt(*pixmap, 0, 0), Eq(50u));
  EXPECT_THAT(alphaAt(*pixmap, 1, 0), Eq(157u));
}

TEST(PipelineBlitterTest, CreateBlitAntiH2OpaqueCoverageComposesAcrossPasses) {
  auto pixmap = tiny_skia::Pixmap::fromSize(2, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 200u);
  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::create(color, &sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitAntiH2(0, 0, 255u, 255u);
  blitter->blitAntiH2(0, 0, 255u, 255u);

  const auto alpha = std::vector<std::uint8_t>{alphaAt(*pixmap, 0, 0), alphaAt(*pixmap, 1, 0)};
  EXPECT_THAT(alpha, ElementsAre(243u, 243u));
}

TEST(PipelineBlitterTest, CreateBlitAntiV2OpaqueCoverageComposesAcrossPasses) {
  auto pixmap = tiny_skia::Pixmap::fromSize(1, 2);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  const auto color = tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10u, 20u, 30u, 200u);
  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::create(color, &sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitAntiV2(0, 0, 255u, 255u);
  blitter->blitAntiV2(0, 0, 255u, 255u);

  const auto alpha = std::vector<std::uint8_t>{alphaAt(*pixmap, 0, 0), alphaAt(*pixmap, 0, 1)};
  EXPECT_THAT(alpha, ElementsAre(243u, 243u));
}
TEST(PipelineBlitterTest, CreateMaskAndBlitRectWritesOpaqueAlphaInRegion) {
  auto pixmap = tiny_skia::Pixmap::fromSize(4, 3);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());
  EXPECT_TRUE(blitter->isMaskOnly());

  const auto rect = tiny_skia::ScreenIntRect::fromXYWH(1, 1, 2, 1);
  ASSERT_TRUE(rect.has_value());
  blitter->blitRect(*rect);

  EXPECT_THAT(alphaAt(*pixmap, 0, 0), Eq(0u));
  EXPECT_THAT(alphaAt(*pixmap, 1, 1), Eq(255u));
  EXPECT_THAT(alphaAt(*pixmap, 2, 1), Eq(255u));
  EXPECT_THAT(alphaAt(*pixmap, 3, 2), Eq(0u));
}

TEST(PipelineBlitterTest, BlitAntiHRespectsRunsAndCoverageKinds) {
  auto pixmap = tiny_skia::Pixmap::fromSize(5, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());

  std::array<std::uint8_t, 4> alpha = {0u, 128u, 255u, 0u};
  std::array<tiny_skia::AlphaRun, 5> runs = {
      tiny_skia::AlphaRun{1}, tiny_skia::AlphaRun{1}, tiny_skia::AlphaRun{1},
      tiny_skia::AlphaRun{1}, std::nullopt,
  };

  blitter->blitAntiH(0, 0, alpha, runs);

  const auto antiHAlpha = std::vector<std::uint8_t>{alphaAt(*pixmap, 0, 0), alphaAt(*pixmap, 1, 0),
                                                    alphaAt(*pixmap, 2, 0), alphaAt(*pixmap, 3, 0),
                                                    alphaAt(*pixmap, 4, 0)};
  EXPECT_THAT(antiHAlpha, ElementsAre(0u, 128u, 255u, 0u, 0u));
}

TEST(PipelineBlitterTest, BlitVHandlesTransparentPartialAndOpaqueCoverage) {
  auto pixmap = tiny_skia::Pixmap::fromSize(1, 3);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitV(0, 0, 1u, 0u);
  blitter->blitV(0, 1, 1u, 128u);
  blitter->blitV(0, 2, 1u, 255u);

  const auto alpha = std::vector<std::uint8_t>{alphaAt(*pixmap, 0, 0), alphaAt(*pixmap, 0, 1),
                                               alphaAt(*pixmap, 0, 2)};
  EXPECT_THAT(alpha, ElementsAre(0u, 128u, 255u));
}

TEST(PipelineBlitterTest, BlitAntiH2WritesPerPixelCoverage) {
  auto pixmap = tiny_skia::Pixmap::fromSize(2, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitAntiH2(0, 0, 64u, 200u);

  const auto alpha = std::vector<std::uint8_t>{alphaAt(*pixmap, 0, 0), alphaAt(*pixmap, 1, 0)};
  EXPECT_THAT(alpha, ElementsAre(64u, 200u));
}

TEST(PipelineBlitterTest, PartialCoverageComposesWithExistingDestinationAlpha) {
  auto pixmap = tiny_skia::Pixmap::fromSize(1, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitV(0, 0, 1u, 128u);
  blitter->blitV(0, 0, 1u, 128u);

  EXPECT_THAT(alphaAt(*pixmap, 0, 0), Eq(192u));
}

TEST(PipelineBlitterTest, BlitRectClipsToPixmapBounds) {
  auto pixmap = tiny_skia::Pixmap::fromSize(2, 2);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitRect(tiny_skia::ScreenIntRect::fromXYWHSafe(1, 1, 3u, 3u));

  const auto alpha = std::vector<std::uint8_t>{alphaAt(*pixmap, 0, 0), alphaAt(*pixmap, 1, 0),
                                               alphaAt(*pixmap, 0, 1), alphaAt(*pixmap, 1, 1)};
  EXPECT_THAT(alpha, ElementsAre(0u, 0u, 0u, 255u));
}

TEST(PipelineBlitterTest, BlitAntiHStopsAtRunSentinel) {
  auto pixmap = tiny_skia::Pixmap::fromSize(4, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());

  std::array<std::uint8_t, 4> alpha = {255u, 255u, 255u, 255u};
  std::array<tiny_skia::AlphaRun, 4> runs = {
      tiny_skia::AlphaRun{1},
      std::nullopt,
      tiny_skia::AlphaRun{1},
      std::nullopt,
  };

  blitter->blitAntiH(0, 0, alpha, runs);

  const auto result = std::vector<std::uint8_t>{alphaAt(*pixmap, 0, 0), alphaAt(*pixmap, 1, 0),
                                                alphaAt(*pixmap, 2, 0), alphaAt(*pixmap, 3, 0)};
  EXPECT_THAT(result, ElementsAre(255u, 0u, 0u, 0u));
}

TEST(PipelineBlitterTest, BlitMaskClipsWhenClipExceedsMaskDimensions) {
  auto pixmap = tiny_skia::Pixmap::fromSize(3, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());

  auto mask = tiny_skia::Mask::fromVec(std::vector<std::uint8_t>{80u, 160u},
                                       tiny_skia::IntSize::fromWH(2, 1).value());
  ASSERT_TRUE(mask.has_value());

  const auto clip = tiny_skia::ScreenIntRect::fromXYWH(0, 0, 3, 1);
  ASSERT_TRUE(clip.has_value());
  blitter->blitMask(*mask, *clip);

  const auto alpha = std::vector<std::uint8_t>{alphaAt(*pixmap, 0, 0), alphaAt(*pixmap, 1, 0),
                                               alphaAt(*pixmap, 2, 0)};
  EXPECT_THAT(alpha, ElementsAre(80u, 160u, 0u));
}

TEST(PipelineBlitterTest, BlitMaskPartialCoverageComposesAcrossPasses) {
  auto pixmap = tiny_skia::Pixmap::fromSize(1, 1);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());

  auto mask = tiny_skia::Mask::fromVec(std::vector<std::uint8_t>{128u},
                                       tiny_skia::IntSize::fromWH(1, 1).value());
  ASSERT_TRUE(mask.has_value());

  const auto clip = tiny_skia::ScreenIntRect::fromXYWH(0, 0, 1, 1);
  ASSERT_TRUE(clip.has_value());
  blitter->blitMask(*mask, *clip);
  blitter->blitMask(*mask, *clip);

  EXPECT_THAT(alphaAt(*pixmap, 0, 0), Eq(192u));
}

TEST(PipelineBlitterTest, BlitMaskLerpsDestinationAlphaWithCoverageMap) {
  auto pixmap = tiny_skia::Pixmap::fromSize(2, 2);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());

  auto mask = tiny_skia::Mask::fromVec(std::vector<std::uint8_t>{0u, 64u, 128u, 255u},
                                       tiny_skia::IntSize::fromWH(2, 2).value());
  ASSERT_TRUE(mask.has_value());

  const auto clip = tiny_skia::ScreenIntRect::fromXYWH(0, 0, 2, 2);
  ASSERT_TRUE(clip.has_value());
  blitter->blitMask(*mask, *clip);

  const auto maskAlpha = std::vector<std::uint8_t>{alphaAt(*pixmap, 0, 0), alphaAt(*pixmap, 1, 0),
                                                   alphaAt(*pixmap, 0, 1), alphaAt(*pixmap, 1, 1)};
  EXPECT_THAT(maskAlpha, ElementsAre(0u, 64u, 128u, 255u));
}

TEST(PipelineBlitterTest, BlitAntiV2WritesSeparatePixelCoverages) {
  auto pixmap = tiny_skia::Pixmap::fromSize(1, 2);
  ASSERT_TRUE(pixmap.has_value());
  auto sub = pixmap->mutableView().subpixmap();

  auto blitter = tiny_skia::pipeline::RasterPipelineBlitter::createMask(&sub);
  ASSERT_TRUE(blitter.has_value());

  blitter->blitAntiV2(0, 0, 10u, 240u);

  const auto antiV2Alpha =
      std::vector<std::uint8_t>{alphaAt(*pixmap, 0, 0), alphaAt(*pixmap, 0, 1)};
  EXPECT_THAT(antiV2Alpha, ElementsAre(10u, 240u));
}

}  // namespace
