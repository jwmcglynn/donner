/// @file
/// Cached sprites on the Geode backend under conditions only a Geode renderer can set up: tight
/// surface budgets, abandoned frames, and the renderer's own capture entry points.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/css/Color.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/tests/ExternalUseSpriteScene.h"

namespace donner::svg {
namespace {

constexpr std::string_view kPlainSheet = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">
    <g id="sprite"><rect x="2" y="2" width="20" height="20" fill="#2080f0"/></g>
  </svg>)svg";

constexpr std::string_view kThreeUses = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="64" height="32">
    <use href="sprites.svg#sprite" transform="translate(2 2)"/>
    <use href="sprites.svg#sprite" transform="translate(22 2)"/>
    <use href="sprites.svg#sprite" transform="translate(42 2)"/>
  </svg>)svg";

std::uint8_t AlphaAt(const RendererBitmap& bitmap, int x, int y) {
  return bitmap
      .pixels[static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4 + 3];
}

// When the frame's surface budget cannot hold a sprite's capture, the occurrence is drawn without
// recording rather than dropped: every frame paints every occurrence, exactly as a renderer
// that does not cache sprites does under the same budget.
TEST(ExternalUseSpriteCacheGeodeTest, RefusedCaptureStillDrawsTheSprite) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_TRUE(device);
  for (const std::uint64_t budgetBytes : {16u << 10, 18u << 10, 20u << 10}) {
    SCOPED_TRACE(budgetBytes);
    std::optional<tests::ExternalUseDocument> scene =
        tests::MakeExternalUseDocument(kThreeUses, std::string(kPlainSheet));
    ASSERT_TRUE(scene.has_value());
    RendererGeode cached(device);
    RendererGeode plain(device);
    plain.setSpriteCacheEnabled(false);
    for (RendererGeode* renderer : {&cached, &plain}) {
      renderer->setSurfaceBudgetForTesting(/*maximumSurfaces=*/8u, budgetBytes);
    }
    for (int frame = 0; frame < 3; ++frame) {
      cached.draw(scene->document);
      plain.draw(scene->document);
      const RendererBitmap cachedPixels = cached.takeSnapshot();
      const RendererBitmap plainPixels = plain.takeSnapshot();
      ASSERT_FALSE(cachedPixels.empty());
      ASSERT_FALSE(plainPixels.empty());
      for (const int x : {12, 32, 52}) {
        EXPECT_EQ(AlphaAt(cachedPixels, x, 12), AlphaAt(plainPixels, x, 12))
            << "frame " << frame << ", occurrence at x=" << x;
      }
    }
  }
}

// A sprite whose recording frame is abandoned before it submits is never drawn from the cache:
// the next frame draws it again rather than the unwritten texture.
TEST(ExternalUseSpriteCacheGeodeTest, AbandonedRecordingIsNotReused) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_TRUE(device);
  RendererGeode renderer(device);
  auto owner = std::make_shared<int>(0);
  FilterResultContentKey key;
  key.document = owner;
  key.sprite = true;
  const Box2d bounds({0, 0}, {24, 24});
  key.filterRegion = bounds;
  RenderViewport viewport;
  viewport.size = Vector2d(64, 64);
  viewport.devicePixelRatio = 1.0;
  const auto drawSprite = [&] {
    renderer.setTransform(Transform2d::Translate(Vector2d(8, 8)));
    const CachedSpriteResult result = renderer.drawCachedSprite(key, bounds);
    if (result != CachedSpriteResult::Drawn) {
      const bool recording =
          result == CachedSpriteResult::Record && renderer.pushSpriteRecording(key, bounds);
      PaintParams paint;
      paint.fill = PaintServer::Solid(css::Color(css::RGBA(255, 0, 0, 255)));
      renderer.setPaint(paint);
      renderer.drawRect(Box2d({4, 4}, {20, 20}), StrokeParams{});
      if (recording) {
        renderer.popSpriteRecording();
      }
    }
    renderer.setTransform(Transform2d());
    return result;
  };

  renderer.beginFrame(viewport);
  EXPECT_NE(drawSprite(), CachedSpriteResult::Drawn);
  EXPECT_EQ(drawSprite(), CachedSpriteResult::Record);
  ASSERT_EQ(renderer.resourceStats().spriteCacheRecords, 1u);
  renderer.injectActiveArenaWriteFailureForTesting();
  PaintParams paint;
  paint.fill = PaintServer::Solid(css::Color(css::RGBA(0, 0, 255, 255)));
  renderer.setPaint(paint);
  renderer.drawRect(Box2d({56, 56}, {60, 60}), StrokeParams{});
  renderer.endFrame();
  ASSERT_FALSE(renderer.deviceLost());

  renderer.beginFrame(viewport);
  EXPECT_NE(drawSprite(), CachedSpriteResult::Drawn);
  renderer.endFrame();
  const RendererBitmap bitmap = renderer.takeSnapshot();
  ASSERT_FALSE(bitmap.empty());
  EXPECT_GT(AlphaAt(bitmap, 20, 20), 128);
}

// A sprite capture is not a filter: it neither charges nor latches the frame's filter budget, so
// the filters drawn after many recordings still run.
TEST(ExternalUseSpriteCacheGeodeTest, CapturesDoNotSpendTheFilterBudget) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_TRUE(device);
  RendererGeode renderer(device);
  RenderViewport viewport;
  viewport.size = Vector2d(64, 64);
  viewport.devicePixelRatio = 1.0;
  renderer.beginFrame(viewport);
  for (int i = 0; i < 6; ++i) {
    auto owner = std::make_shared<int>(i);
    FilterResultContentKey key;
    key.document = owner;
    key.sprite = true;
    const Box2d bounds({0, 0}, {8, 8});
    key.filterRegion = bounds;
    renderer.setTransform(Transform2d::Translate(Vector2d(8 * i, 0)));
    (void)renderer.drawCachedSprite(key, bounds);
    if (renderer.drawCachedSprite(key, bounds) == CachedSpriteResult::Record &&
        renderer.pushSpriteRecording(key, bounds)) {
      renderer.popSpriteRecording();
    }
  }
  renderer.endFrame();
  EXPECT_EQ(renderer.resourceStats().filterExecutions, 0u);
  EXPECT_FALSE(renderer.resourceStats().filterBudgetRejected);
}

// A sprite whose capture can never be begun (it is larger than any surface may be) is refused
// once and never admitted for recording again, so it cannot take capture budget every frame.
TEST(ExternalUseSpriteCacheGeodeTest, ImpossibleCaptureIsNotRetried) {
  std::shared_ptr<geode::GeodeDevice> device(geode::GeodeDevice::CreateHeadless());
  ASSERT_TRUE(device);
  RendererGeode renderer(device);
  auto owner = std::make_shared<int>(0);
  FilterResultContentKey key;
  key.document = owner;
  key.sprite = true;
  const Box2d bounds({0, 0}, {6000, 20});
  key.filterRegion = bounds;
  RenderViewport viewport;
  viewport.size = Vector2d(64, 64);
  viewport.devicePixelRatio = 1.0;
  int records = 0;
  for (int frame = 0; frame < 4; ++frame) {
    renderer.beginFrame(viewport);
    for (int draw = 0; draw < 3; ++draw) {
      if (renderer.drawCachedSprite(key, bounds) == CachedSpriteResult::Record) {
        ++records;
        EXPECT_FALSE(renderer.pushSpriteRecording(key, bounds));
      }
    }
    renderer.endFrame();
  }
  EXPECT_EQ(records, 1) << "Admitted once, refused, and never admitted again.";
}

}  // namespace
}  // namespace donner::svg
