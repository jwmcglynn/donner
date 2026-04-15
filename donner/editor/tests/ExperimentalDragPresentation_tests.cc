#include "donner/editor/ExperimentalDragPresentation.h"

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(ExperimentalDragPresentationTest, SelectionTriggersPrewarmWhenCacheMissing) {
  ExperimentalDragPresentation state;
  EXPECT_TRUE(state.shouldPrewarm(Entity(7), /*currentVersion=*/3, Vector2i(100, 100),
                                  /*dragActive=*/false));
}

TEST(ExperimentalDragPresentationTest, UpToDateCacheSuppressesPrewarm) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  EXPECT_FALSE(state.shouldPrewarm(Entity(7), /*currentVersion=*/3, Vector2i(100, 100),
                                   /*dragActive=*/false));
}

TEST(ExperimentalDragPresentationTest, CachedTexturesDisplayZeroTranslationWithoutActiveDrag) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 0.0);
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));
}

TEST(ExperimentalDragPresentationTest, MouseUpKeepsSettlingPreviewUntilFullRenderLands) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  SelectTool::ActiveDragPreview preview{.entity = Entity(7), .translation = Vector2d(12.0, 5.0)};
  state.beginSettling(preview, /*targetVersion=*/4);

  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 12.0);

  state.noteFullRenderLanded(/*landedVersion=*/3);
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));

  state.noteFullRenderLanded(/*landedVersion=*/4);
  EXPECT_FALSE(state.shouldDisplayCompositedLayers(std::nullopt));
  EXPECT_FALSE(state.presentationPreview(std::nullopt).has_value());
}

TEST(ExperimentalDragPresentationTest, SelectionChangeClearsSettlingState) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(3.0, 2.0),
      },
      /*targetVersion=*/4);
  state.noteFullRenderLanded(/*landedVersion=*/4);

  state.clearSettlingIfSelectionChanged(Entity(8), /*dragActive=*/false);
  EXPECT_FALSE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_FALSE(state.waitingForFullRender);
}

TEST(ExperimentalDragPresentationTest,
     SelectionChangeDoesNotClearSettlingWhileWaitingForFullRender) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(3.0, 2.0),
      },
      /*targetVersion=*/4);

  state.clearSettlingIfSelectionChanged(Entity(8), /*dragActive=*/false);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_TRUE(state.waitingForFullRender);
}

TEST(ExperimentalDragPresentationTest, FullRenderLandedClearsCachedTextures) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));

  state.noteFullRenderLanded(/*landedVersion=*/3);
  EXPECT_FALSE(state.hasCachedTextures);
  EXPECT_TRUE(state.cachedEntity == entt::null);
  EXPECT_FALSE(state.shouldDisplayCompositedLayers(std::nullopt));
  EXPECT_FALSE(state.presentationPreview(std::nullopt).has_value());
}

TEST(ExperimentalDragPresentationTest, SettlingCompletionTriggersPrewarmOnNextSelection) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(5.0, 0.0),
      },
      /*targetVersion=*/4);

  // After settling completes, hasCachedTextures is cleared so shouldPrewarm returns true.
  state.noteFullRenderLanded(/*landedVersion=*/4);
  EXPECT_TRUE(state.shouldPrewarm(Entity(7), /*currentVersion=*/4, Vector2i(100, 100),
                                  /*dragActive=*/false));
}

TEST(ExperimentalDragPresentationTest, SettlingViaCompositedRenderKeepsCachedTextures) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(5.0, 0.0),
      },
      /*targetVersion=*/4);

  // Settling is resolved by a composited render (not a flat one). Keep the drag offset alive until
  // the selection chrome catches up, so stale overlay/AABB state doesn't pop back to pre-drag
  // position for a frame.
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100));
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_FALSE(state.waitingForFullRender);
  EXPECT_TRUE(state.waitingForChromeRefresh);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 5.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 0.0);
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));

  state.noteChromeRefreshCompleted(/*refreshedVersion=*/4);
  EXPECT_FALSE(state.waitingForChromeRefresh);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 0.0);
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));
}

TEST(ExperimentalDragPresentationTest, CompositedSettleKeepsOffsetUntilChromeRefreshCompletes) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(12.0, 4.0),
      },
      /*targetVersion=*/4);

  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100));

  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 12.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 4.0);
  EXPECT_TRUE(state.waitingForChromeRefresh);

  state.noteChromeRefreshCompleted(/*refreshedVersion=*/3);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 12.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 4.0);

  state.noteChromeRefreshCompleted(/*refreshedVersion=*/4);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 0.0);
}

TEST(ExperimentalDragPresentationTest, EntityChangeAfterSettlingClearsCachedTextures) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(5.0, 0.0),
      },
      /*targetVersion=*/4);

  // Settling completes via flat render — clears all cached state.
  state.noteFullRenderLanded(/*landedVersion=*/4);

  // A new composited render lands for Entity(9) — represents the replacement entity
  // after a ReplaceDocumentCommand from source writeback.
  state.noteCachedTextures(Entity(9), /*version=*/5, Vector2i(100, 100));
  EXPECT_EQ(state.cachedEntity, Entity(9));
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));
}

// Regression test: After settling resolves via composited render, if the entity changes (e.g., from
// ReplaceDocument), clearSettlingIfSelectionChanged clears the settling preview but keeps cached
// textures alive.  The composited textures at zero offset are visually correct, and the prewarm
// render for the new entity will atomically update them — no pop to flat.
TEST(ExperimentalDragPresentationTest,
     ClearSettlingIfSelectionChangedKeepsTexturesAfterComposedSettle) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(5.0, 0.0),
      },
      /*targetVersion=*/4);

  // Settling resolved via composited render — hasCachedTextures stays true.
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100));
  state.noteChromeRefreshCompleted(/*refreshedVersion=*/4);
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_FALSE(state.waitingForFullRender);

  // Entity changes (ReplaceDocument): selection is remapped to Entity(9).
  state.clearSettlingIfSelectionChanged(Entity(9), /*dragActive=*/false);

  // Settling preview is cleared, but cached textures are kept alive for seamless display.
  EXPECT_FALSE(state.settlingPreview.has_value());
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_EQ(state.cachedEntity, Entity(7));
  // presentationPreview falls to third branch: cached textures with zero offset.
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0);
}

// Verify that cached textures are kept after entity handle change (ReplaceDocument scenario).
// The prewarm render for the new entity will atomically update them.
TEST(ExperimentalDragPresentationTest, ClearSettlingIfSelectionChangedKeepsTexturesWithoutSettle) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));

  // Entity changes without any settling phase (e.g., ReplaceDocument after writeback).
  state.clearSettlingIfSelectionChanged(Entity(9), /*dragActive=*/false);

  // Cached textures stay alive — shouldPrewarm will trigger for the new entity.
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_EQ(state.cachedEntity, Entity(7));
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));

  // Prewarm is triggered for the new entity (entity mismatch).
  EXPECT_TRUE(state.shouldPrewarm(Entity(9), /*currentVersion=*/3, Vector2i(100, 100),
                                  /*dragActive=*/false));
}

// Verify deselection (null entity) clears cached textures.
TEST(ExperimentalDragPresentationTest, DeselectionClearsCachedTextures) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));

  state.clearSettlingIfSelectionChanged(entt::null, /*dragActive=*/false);

  EXPECT_FALSE(state.hasCachedTextures);
  EXPECT_FALSE(state.shouldDisplayCompositedLayers(std::nullopt));
}

// Verify that during an active drag, clearSettlingIfSelectionChanged does NOT clear textures.
TEST(ExperimentalDragPresentationTest, ActiveDragPreventsTextureClearing) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  state.clearSettlingIfSelectionChanged(Entity(9), /*dragActive=*/true);

  // Cached textures preserved because drag is active.
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_EQ(state.cachedEntity, Entity(7));
}

}  // namespace
}  // namespace donner::editor
