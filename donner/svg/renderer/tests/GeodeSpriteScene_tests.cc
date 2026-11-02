/// @file
/// Per-frame work of a sprite-driven scene through Geode: many occurrences of a few sprites drawn
/// with external `<use>` references into sprite sheets. Counters, not timings, so every assertion
/// is deterministic.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/FontResourceGraph.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/renderer/RendererDriver.h"
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

/// Draws \p document with a driver over \p renderer and returns the font collection work it did.
components::FontResourceGraphCache::Stats DrawCountingFontWork(RendererGeode& renderer,
                                                               SVGDocument& document) {
  RendererDriver::SecurityStats stats;
  RendererDriver driver(renderer, false, &stats);
  driver.draw(document);
  return stats.nestedFontResources;
}

/// A sprite sheet with no text renders nothing that depends on a font, so drawing its sprites
/// builds no font dependency graph and collects no dependencies, however many occurrences there
/// are. Before, every occurrence rebuilt the sheet's graph.
TEST_F(GeodeSpriteSceneTest, TextFreeSpritesSkipFontDependencyCollection) {
  RendererGeode renderer(sharedDevice());
  SpriteScene scene(SmallScene());
  (void)DrawCountingFontWork(renderer, scene.document());
  const components::FontResourceGraphCache::Stats work =
      DrawCountingFontWork(renderer, scene.document());
  EXPECT_EQ(work.graphBuilds, 0u);
  EXPECT_EQ(work.targetCollections, 0u);
  EXPECT_EQ(work.cacheHits, 0u);
  EXPECT_GT(work.fontIndependentCollections, 0u);
}

/// The shortcut answers exactly what the full collection would for a text-free sheet: no
/// dependencies, and the same rendered-frame coverage status, for drawn and undrawn sprites and
/// for both purposes.
TEST_F(GeodeSpriteSceneTest, FontIndependentCollectionMatchesTheFullGraph) {
  RendererGeode renderer(sharedDevice());
  SpriteScene scene(SmallScene());
  (void)DrawCountingFontWork(renderer, scene.document());
  const auto* external =
      scene.use(0).unsafeEntityHandle().try_get<components::ExternalUseComponent>();
  ASSERT_NE(external, nullptr);
  SVGDocument sheet = SVGDocument::CreateFromHandle(external->subDocument);
  const Registry& registry = sheet.registry();
  ASSERT_TRUE(components::RendersNothingFontDependent(registry));
  const components::FontResourceGraph graph(registry);
  for (const char* id : {"#s0", "#s1", "#s2", "#s0-glow", "#s2-glow"}) {
    const auto sprite = sheet.querySelector(id);
    ASSERT_TRUE(sprite.has_value()) << id;
    const Entity target = sprite->unsafeEntityHandle().entity();
    for (const auto purpose : {components::FontResourceGraph::Purpose::CompleteTarget,
                               components::FontResourceGraph::Purpose::RenderedFrame}) {
      SCOPED_TRACE(testing::Message()
                   << id
                   << (purpose == components::FontResourceGraph::Purpose::RenderedFrame
                           ? " rendered"
                           : " complete"));
      components::FontResourceGraphCache cache;
      const auto shortcut = cache.collect(external->subDocument, target, purpose);
      const auto full = graph.collect(registry, target, purpose);
      EXPECT_EQ(shortcut.dependencies, full.dependencies);
      EXPECT_EQ(shortcut.needsRender, full.needsRender);
      EXPECT_EQ(shortcut.resourceLimit, full.resourceLimit);
    }
  }
}

/// Skipping the collection for text-free sheets changes no pixels: the same sprites drawn from a
/// sheet that also carries an unreferenced text element, which keeps the collection running,
/// render identically.
TEST_F(GeodeSpriteSceneTest, SkippingFontCollectionLeavesTextFreeSpritesUnchanged) {
  RendererGeode skipped(sharedDevice());
  SpriteScene textFree(SmallScene());
  (void)DrawCountingFontWork(skipped, textFree.document());
  const RendererBitmap expectedSkip = skipped.takeSnapshot();
  ASSERT_FALSE(expectedSkip.empty());

  SpriteSceneOptions withText = SmallScene();
  withText.unreferencedText = true;
  RendererGeode collected(sharedDevice());
  SpriteScene withUnreferencedText(withText);
  const components::FontResourceGraphCache::Stats work =
      DrawCountingFontWork(collected, withUnreferencedText.document());
  EXPECT_EQ(work.fontIndependentCollections, 0u)
      << "a sheet with text must still collect its dependencies";
  EXPECT_GT(work.graphBuilds + work.cacheHits + work.targetCollections, 0u);
  editor::tests::CompareBitmapToBitmap(collected.takeSnapshot(), expectedSkip,
                                       "sprite_scene_font_skip_identity",
                                       editor::tests::PixelmatchIdentityParams());
}

/// Sprites from a sheet with text keep their font dependency bookkeeping, and a text-free sheet
/// that gains text starts collecting again on its next draw.
TEST_F(GeodeSpriteSceneTest, FontCollectionStillRunsForTextAndAfterTheSheetGainsText) {
  RendererGeode renderer(sharedDevice());
  SpriteSceneOptions options = SmallScene();
  options.includeText = true;
  SpriteScene labeled(options);
  const components::FontResourceGraphCache::Stats labeledWork =
      DrawCountingFontWork(renderer, labeled.document());
  EXPECT_GT(labeledWork.graphBuilds + labeledWork.targetCollections + labeledWork.cacheHits, 0u)
      << "the labeled sheet's sprites must collect their font dependencies";
  EXPECT_GT(labeledWork.fontIndependentCollections, 0u)
      << "the scene's text-free sheets still skip";

  SpriteScene scene(SmallScene());
  (void)DrawCountingFontWork(renderer, scene.document());
  const auto* external =
      scene.use(0).unsafeEntityHandle().try_get<components::ExternalUseComponent>();
  ASSERT_NE(external, nullptr);
  SVGDocument sheet = SVGDocument::CreateFromHandle(external->subDocument);
  auto sprite = sheet.querySelector("#" + std::string(external->fragment.str()));
  ASSERT_TRUE(sprite.has_value());
  sprite->appendChild(SVGTextElement::Create(sheet));

  const components::FontResourceGraphCache::Stats afterEdit =
      DrawCountingFontWork(renderer, scene.document());
  EXPECT_GT(afterEdit.graphBuilds + afterEdit.targetCollections + afterEdit.cacheHits, 0u)
      << "a sheet that gained text must collect its dependencies again";
}

}  // namespace
}  // namespace donner::svg
