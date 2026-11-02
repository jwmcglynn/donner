/// @file
/// CPU-invariant counters for a game-like sprite field: warm frames that only move sprites or flip
/// them to another sprite of an already loaded sheet keep the document's shadow trees,
/// sub-document admissions and styles, and a full rebuild admits each sheet once.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <optional>

#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/resources/SubDocumentCache.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/renderer/Renderer.h"
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

/// Renders the warm-up frames of a field and returns it, or records a failure.
std::optional<SpriteFieldScene> WarmField(Renderer& renderer) {
  std::optional<SpriteFieldScene> scene = MakeSpriteFieldScene(kField);
  if (!scene) {
    return std::nullopt;
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
  ASSERT_TRUE(scene.has_value());

  const WorkCounters before = ReadCounters(scene->document);
  RestyleCounter restyles(scene->document);
  for (int frame = kWarmFrames; frame < kWarmFrames + kMeasuredFrames; ++frame) {
    AdvanceSpriteField(*scene, frame, SpriteFieldChange::Move);
    renderer.draw(scene->document);
  }
  ExpectOnlyRetainedUpdates(before, ReadCounters(scene->document), kMeasuredFrames);
  EXPECT_EQ(restyles.restyles(), 0u);
}

TEST(SpriteFieldPrepareCorrectnessTest, OpacityFramesAdmitEachSheetOncePerRebuild) {
  Renderer renderer;
  std::optional<SpriteFieldScene> scene = WarmField(renderer);
  ASSERT_TRUE(scene.has_value());

  // An opacity write still takes the full rebuild, which admits each sheet once, not once per use.
  const WorkCounters before = ReadCounters(scene->document);
  for (int frame = kWarmFrames; frame < kWarmFrames + kMeasuredFrames; ++frame) {
    AdvanceSpriteField(*scene, frame, SpriteFieldChange::Opacity);
    renderer.draw(scene->document);
  }
  const WorkCounters after = ReadCounters(scene->document);
  EXPECT_EQ(after.renderTree.fullRebuilds, before.renderTree.fullRebuilds + kMeasuredFrames);
  EXPECT_EQ(after.cachedAdmissions,
            before.cachedAdmissions + static_cast<std::size_t>(kMeasuredFrames) * kField.sheets);
}

TEST(SpriteFieldPrepareCorrectnessTest, RetargetFramesAttachOnlyTheRetargetedUses) {
  Renderer renderer;
  std::optional<SpriteFieldScene> scene = WarmField(renderer);
  ASSERT_TRUE(scene.has_value());

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

TEST(SpriteFieldPrepareCorrectnessTest, GameFramesBetweenFadesKeepTheRenderTree) {
  Renderer renderer;
  std::optional<SpriteFieldScene> scene = WarmField(renderer);
  ASSERT_TRUE(scene.has_value());

  // The measured frames move and retarget sprites; none of them is a fade frame.
  const WorkCounters before = ReadCounters(scene->document);
  RestyleCounter restyles(scene->document);
  for (int frame = kWarmFrames; frame < kWarmFrames + kMeasuredFrames; ++frame) {
    AdvanceSpriteField(*scene, frame, SpriteFieldChange::All);
    renderer.draw(scene->document);
  }
  ExpectOnlyRetainedUpdates(before, ReadCounters(scene->document), kMeasuredFrames);
  EXPECT_EQ(restyles.restyles(), 0u);
}

TEST(SpriteFieldPrepareCorrectnessTest, FullRebuildAdmitsEachSheetOnce) {
  Renderer renderer;
  std::optional<SpriteFieldScene> scene = WarmField(renderer);
  ASSERT_TRUE(scene.has_value());

  scene->document.registry().ctx().get<RenderTreeState>().retainedUpdatesEnabled = false;
  const WorkCounters before = ReadCounters(scene->document);
  AdvanceSpriteField(*scene, kWarmFrames, SpriteFieldChange::Move);
  renderer.draw(scene->document);
  const WorkCounters after = ReadCounters(scene->document);
  EXPECT_EQ(after.renderTree.fullRebuilds, before.renderTree.fullRebuilds + 1);
  EXPECT_EQ(after.cachedAdmissions, before.cachedAdmissions + kField.sheets);
}

}  // namespace
}  // namespace donner::svg::tests
