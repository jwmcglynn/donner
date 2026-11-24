/// @file
/// CPU-invariant counters for a game-like sprite field: warm frames that only move sprites, fade
/// them or flip them to another sprite of an already loaded sheet keep the document's shadow trees,
/// sub-document admissions and styles, and do work proportional to what changed; sheets without
/// text answer their font dependency collections without building a dependency graph.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>

#include "donner/svg/SVGTextElement.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/FontResourceGraph.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/SubDocumentCache.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererDriver.h"
#include "donner/svg/renderer/tests/MockRendererInterface.h"
#include "donner/svg/renderer/tests/SpriteFieldScene.h"

namespace donner::svg::tests {
namespace {

using components::RenderTreeState;

constexpr SpriteFieldOptions kField{.sprites = 24, .sheets = 3, .spritesPerSheet = 6};
constexpr int kWarmFrames = 2;
constexpr int kMeasuredFrames = 6;

/// Counts element restyles in a document: each restyle constructs a computed style.
class RestyleCounter {
public:
  explicit RestyleCounter(SVGDocument document) : document_(std::move(document)) {
    document_.registry()
        .on_construct<components::ComputedStyleComponent>()
        .connect<&RestyleCounter::onRestyle>(*this);
  }

  ~RestyleCounter() {
    document_.registry()
        .on_construct<components::ComputedStyleComponent>()
        .disconnect<&RestyleCounter::onRestyle>(*this);
  }

  RestyleCounter(const RestyleCounter&) = delete;
  RestyleCounter& operator=(const RestyleCounter&) = delete;

  std::size_t restyles() const { return restyles_; }

private:
  void onRestyle(Registry&, Entity) { ++restyles_; }

  SVGDocument document_;
  std::size_t restyles_ = 0;
};

/// A snapshot of the work counters a frame can move.
struct WorkCounters {
  RenderTreeState::Counters renderTree;
  std::size_t cachedAdmissions = 0;
};

WorkCounters ReadCounters(SVGDocument& document) {
  Registry& registry = document.registry();
  WorkCounters counters;
  counters.renderTree = registry.ctx().get<RenderTreeState>().counters;
  if (const auto* cache = registry.ctx().find<components::SubDocumentCache>()) {
    counters.cachedAdmissions = cache->cachedDocumentAdmissions();
  }
  return counters;
}

/// Uses \ref AdvanceSpriteField retargets (or fades) on \p frame.
std::size_t EditedUses(int frame) {
  std::size_t edited = 0;
  for (int i = frame % 7; i < kField.sprites; i += 13) {
    ++edited;
  }
  return edited;
}

/**
 * Renders the warm-up frames of a field and returns it, or records a failure. Every sprite of
 * every sheet is drawn once first: a sheet grows the first time one of its sprites is drawn, and a
 * grown sheet is rightly admitted again on its next retarget.
 */
std::optional<SpriteFieldScene> WarmField(Renderer& renderer) {
  std::optional<SpriteFieldScene> scene = MakeSpriteFieldScene(kField);
  if (!scene) {
    return std::nullopt;
  }
  for (int sprite = 0; sprite < kField.spritesPerSheet; ++sprite) {
    for (int i = 0; i < kField.sprites; ++i) {
      scene->uses[i].setHref(RcString("sheet-" + std::to_string(i % kField.sheets) +
                                      ".svg#sprite-" + std::to_string(sprite)));
    }
    renderer.draw(scene->document);
  }
  for (int frame = 0; frame < kWarmFrames; ++frame) {
    AdvanceSpriteField(*scene, frame);
    renderer.draw(scene->document);
  }
  return scene;
}

/// Asserts that \p after differs from \p before only by \p frames retained updates.
void ExpectOnlyRetainedUpdates(const WorkCounters& before, const WorkCounters& after, int frames) {
  EXPECT_EQ(after.renderTree.fullRebuilds, before.renderTree.fullRebuilds);
  EXPECT_EQ(after.renderTree.retainedUpdates, before.renderTree.retainedUpdates + frames);
  EXPECT_EQ(after.renderTree.shadowTreeTeardowns, before.renderTree.shadowTreeTeardowns);
  EXPECT_EQ(after.cachedAdmissions, before.cachedAdmissions);
}

TEST(SpriteFieldPrepareCorrectnessTest, MovingFramesKeepTheRenderTree) {
  Renderer renderer;
  std::optional<SpriteFieldScene> scene = WarmField(renderer);
  ASSERT_THAT(scene, testing::Optional(testing::_));

  const WorkCounters before = ReadCounters(scene->document);
  RestyleCounter restyles(scene->document);
  for (int frame = kWarmFrames; frame < kWarmFrames + kMeasuredFrames; ++frame) {
    AdvanceSpriteField(*scene, frame, SpriteFieldChange::Move);
    renderer.draw(scene->document);
  }
  ExpectOnlyRetainedUpdates(before, ReadCounters(scene->document), kMeasuredFrames);
  EXPECT_EQ(restyles.restyles(), 0u);
}

TEST(SpriteFieldPrepareCorrectnessTest, OpacityFramesRestyleOnlyTheFadedUses) {
  Renderer renderer;
  std::optional<SpriteFieldScene> scene = WarmField(renderer);
  ASSERT_THAT(scene, testing::Optional(testing::_));

  const WorkCounters before = ReadCounters(scene->document);
  RestyleCounter restyles(scene->document);
  std::size_t edited = 0;
  for (int frame = kWarmFrames; frame < kWarmFrames + kMeasuredFrames; ++frame) {
    AdvanceSpriteField(*scene, frame, SpriteFieldChange::Opacity);
    edited += EditedUses(frame);
    renderer.draw(scene->document);
  }
  ExpectOnlyRetainedUpdates(before, ReadCounters(scene->document), kMeasuredFrames);
  // An external use has no children in this document, so each fade restyles exactly one element.
  EXPECT_EQ(restyles.restyles(), edited);
}

TEST(SpriteFieldPrepareCorrectnessTest, RetargetFramesAttachOnlyTheRetargetedUses) {
  Renderer renderer;
  std::optional<SpriteFieldScene> scene = WarmField(renderer);
  ASSERT_THAT(scene, testing::Optional(testing::_));

  const WorkCounters before = ReadCounters(scene->document);
  RestyleCounter restyles(scene->document);
  std::size_t edited = 0;
  for (int frame = kWarmFrames; frame < kWarmFrames + kMeasuredFrames; ++frame) {
    AdvanceSpriteField(*scene, frame, SpriteFieldChange::Retarget);
    edited += EditedUses(frame);
    renderer.draw(scene->document);
  }
  const WorkCounters after = ReadCounters(scene->document);
  ExpectOnlyRetainedUpdates(before, after, kMeasuredFrames);
  EXPECT_EQ(after.renderTree.retargetedUses, before.renderTree.retargetedUses + edited);
  EXPECT_EQ(restyles.restyles(), 0u);
}

TEST(SpriteFieldPrepareCorrectnessTest, GameFramesCombineAllThreeChanges) {
  Renderer renderer;
  std::optional<SpriteFieldScene> scene = WarmField(renderer);
  ASSERT_THAT(scene, testing::Optional(testing::_));

  const WorkCounters before = ReadCounters(scene->document);
  RestyleCounter restyles(scene->document);
  std::size_t edited = 0;
  for (int frame = kWarmFrames; frame < kWarmFrames + kMeasuredFrames; ++frame) {
    AdvanceSpriteField(*scene, frame, SpriteFieldChange::All);
    edited += EditedUses(frame);
    renderer.draw(scene->document);
  }
  ExpectOnlyRetainedUpdates(before, ReadCounters(scene->document), kMeasuredFrames);
  EXPECT_EQ(restyles.restyles(), edited);
}

TEST(SpriteFieldPrepareCorrectnessTest, FullRebuildAdmitsEachSheetOnce) {
  Renderer renderer;
  std::optional<SpriteFieldScene> scene = WarmField(renderer);
  ASSERT_THAT(scene, testing::Optional(testing::_));

  scene->document.registry().ctx().get<RenderTreeState>().retainedUpdatesEnabled = false;
  const WorkCounters before = ReadCounters(scene->document);
  AdvanceSpriteField(*scene, kWarmFrames, SpriteFieldChange::Move);
  renderer.draw(scene->document);
  const WorkCounters after = ReadCounters(scene->document);
  EXPECT_EQ(after.renderTree.fullRebuilds, before.renderTree.fullRebuilds + 1);
  EXPECT_EQ(after.cachedAdmissions, before.cachedAdmissions + kField.sheets);
}

/// Font collection work summed over a run of frames.
struct FontCollectionWork {
  std::size_t graphBuilds = 0;
  std::size_t targetCollections = 0;
  std::size_t cacheHits = 0;
  std::size_t fontIndependentCollections = 0;

  void add(const components::FontResourceGraphCache::Stats& frame) {
    graphBuilds += frame.graphBuilds;
    targetCollections += frame.targetCollections;
    cacheHits += frame.cacheHits;
    fontIndependentCollections += frame.fontIndependentCollections;
  }
};

/// Draws game frames of a field through a driver that reports its font collection work.
class FontCollectionFieldTest : public testing::Test {
protected:
  void SetUp() override {
    scene_ = MakeSpriteFieldScene(kField);
    ASSERT_THAT(scene_, testing::Optional(testing::_));
    for (int frame = 0; frame < kWarmFrames; ++frame) {
      AdvanceSpriteField(*scene_, frame);
      driver_.draw(scene_->document);
    }
  }

  /// Draws \p frames game frames after the warm-up and returns their summed font work.
  FontCollectionWork drawFrames(int firstFrame, int frames) {
    FontCollectionWork work;
    for (int frame = firstFrame; frame < firstFrame + frames; ++frame) {
      AdvanceSpriteField(*scene_, frame);
      driver_.draw(scene_->document);
      work.add(stats_.nestedFontResources);
    }
    return work;
  }

  /// The sheet that use \p index draws from.
  SVGDocument sheetOf(int index) {
    const auto* external =
        scene_->uses[index].unsafeEntityHandle().try_get<components::ExternalUseComponent>();
    EXPECT_NE(external, nullptr);
    return SVGDocument::CreateFromHandle(external->subDocument);
  }

  testing::NiceMock<MockRendererInterface> backend_;
  RendererDriver::SecurityStats stats_;
  RendererDriver driver_{backend_, /*verbose=*/false, &stats_};
  std::optional<SpriteFieldScene> scene_;
};

/// The field's sheets hold no text, so drawing their sprites builds no font dependency graph, for
/// any number of uses: every collection is answered from the registry without one.
TEST_F(FontCollectionFieldTest, TextFreeSheetsBuildNoFontDependencyGraph) {
  const FontCollectionWork work = drawFrames(kWarmFrames, kMeasuredFrames);
  EXPECT_EQ(work.graphBuilds, 0u);
  EXPECT_EQ(work.targetCollections, 0u);
  EXPECT_EQ(work.cacheHits, 0u);
  // Each use collects its target twice a frame: complete and rendered-frame.
  EXPECT_EQ(work.fontIndependentCollections,
            static_cast<std::size_t>(2 * kField.sprites * kMeasuredFrames));
}

/// A sheet that gains text collects its dependencies again on the next frame, through the graph;
/// the other sheets keep the shortcut.
TEST_F(FontCollectionFieldTest, SheetThatGainsTextCollectsThroughTheGraphAgain) {
  (void)drawFrames(kWarmFrames, 1);
  SVGDocument sheet = sheetOf(0);
  std::optional<SVGElement> sprite = sheet.querySelector("#sprite-0");
  ASSERT_THAT(sprite, testing::Optional(testing::_));
  sprite->appendChild(SVGTextElement::Create(sheet));

  const FontCollectionWork work = drawFrames(kWarmFrames + 1, 1);
  EXPECT_GT(work.graphBuilds, 0u) << "the sheet with text must build its graph";
  EXPECT_GT(work.fontIndependentCollections, 0u) << "the text-free sheets keep the shortcut";
}

/// The shortcut gives exactly the full graph's answer for every sprite of every sheet, including
/// filtered and clipped sprites, and for both purposes: no dependencies, the same rendered-frame
/// coverage and no resource limit.
TEST_F(FontCollectionFieldTest, ShortcutMatchesTheFullGraph) {
  (void)drawFrames(kWarmFrames, kMeasuredFrames);
  for (int use = 0; use < kField.sheets; ++use) {
    SVGDocument sheet = sheetOf(use);
    const Registry& registry = sheet.registry();
    const components::FontResourceGraph graph(registry);
    for (int index = 0; index < kField.spritesPerSheet; ++index) {
      const std::string id = "#sprite-" + std::to_string(index);
      const std::optional<SVGElement> sprite = sheet.querySelector(id);
      ASSERT_THAT(sprite, testing::Optional(testing::_)) << id;
      const Entity target = sprite->unsafeEntityHandle().entity();
      for (const auto purpose : {components::FontResourceGraph::Purpose::CompleteTarget,
                                 components::FontResourceGraph::Purpose::RenderedFrame}) {
        SCOPED_TRACE(testing::Message()
                     << "sheet of use " << use << " " << id
                     << (purpose == components::FontResourceGraph::Purpose::RenderedFrame
                             ? " rendered"
                             : " complete"));
        components::FontResourceGraphCache::Stats cacheStats;
        components::FontResourceGraphCache cache(&cacheStats);
        const components::FontResourceGraph::Collection shortcut =
            cache.collect(sheet.handle(), target, purpose);
        const components::FontResourceGraph::Collection full =
            graph.collect(registry, target, purpose);
        EXPECT_EQ(cacheStats.fontIndependentCollections, 1u);
        EXPECT_THAT(shortcut.dependencies, testing::ElementsAreArray(full.dependencies));
        EXPECT_EQ(shortcut.needsRender, full.needsRender);
        EXPECT_EQ(shortcut.resourceLimit, full.resourceLimit);
      }
    }
  }
}

}  // namespace
}  // namespace donner::svg::tests
