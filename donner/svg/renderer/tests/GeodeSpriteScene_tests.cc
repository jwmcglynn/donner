/// @file
/// Per-frame work of a sprite-driven scene through Geode: many occurrences of a few sprites drawn
/// with external `<use>` references into sprite sheets. Counters, not timings, so every assertion
/// is deterministic.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/tests/SpriteSceneFixture.h"

namespace donner::svg {
namespace {

using tests::SpriteScene;
using tests::SpriteSceneOptions;

/// A scene small enough for a correctness lane, with repeats of every sprite.
SpriteSceneOptions SmallScene() {
  SpriteSceneOptions options;
  options.width = 400;
  options.height = 300;
  options.sprites = 24;
  options.glowingEvery = 5;
  return options;
}

class GeodeSpriteSceneTest : public ::testing::Test {
protected:
  static std::shared_ptr<geode::GeodeDevice> sharedDevice() {
    static auto device = [] {
      return std::shared_ptr<geode::GeodeDevice>(geode::GeodeDevice::CreateHeadless());
    }();
    return device;
  }

  void SetUp() override { ASSERT_NE(sharedDevice(), nullptr); }
};

/// Every occurrence of a sprite draws the same encoded paths, so once the scene is warm a frame
/// re-uploads none of that geometry: what remains is one uniform block per draw. Before geometry
/// stayed resident for repeated occurrences, every occurrence after a sprite's first re-uploaded
/// all of its geometry, several KiB per draw.
TEST_F(GeodeSpriteSceneTest, WarmStaticFrameUploadsOnlyPerDrawUniforms) {
  RendererGeode renderer(sharedDevice());
  SpriteScene scene(SmallScene());
  renderer.draw(scene.document());
  const RendererBitmap cold = renderer.takeSnapshot();
  ASSERT_FALSE(cold.empty());

  renderer.draw(scene.document());
  renderer.draw(scene.document());
  const geode::GeodeCounters warm = renderer.lastFrameTimings().counters;
  EXPECT_EQ(warm.pathEncodes, 0u);
  EXPECT_EQ(warm.bufferCreates, 0u);
  ASSERT_GT(warm.drawCalls, 100u) << "the scene must draw every occurrence";
  // The largest per-draw block is a gradient uniform, well under 1 KiB.
  constexpr uint64_t kPerDrawUniformBound = 1024;
  EXPECT_LE(warm.bufferWriteBytes, warm.drawCalls * kPerDrawUniformBound)
      << "a warm frame re-uploaded geometry: " << warm.bufferWriteBytes << " bytes for "
      << warm.drawCalls << " draws";

  const RendererBitmap warmBitmap = renderer.takeSnapshot();
  editor::tests::CompareBitmapToBitmap(warmBitmap, cold, "sprite_scene_warm_vs_cold",
                                       editor::tests::PixelmatchIdentityParams());
}

}  // namespace
}  // namespace donner::svg
