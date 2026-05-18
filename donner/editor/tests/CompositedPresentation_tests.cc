#include "donner/editor/CompositedPresentation.h"

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

TEST(CompositedPresentationTest, SelectionTriggersPrewarmWhenCacheMissing) {
  CompositedPresentation state;
  EXPECT_TRUE(state.shouldPrewarm(Entity(7), /*currentVersion=*/3, Vector2i(100, 100),
                                  /*dragActive=*/false));
}

TEST(CompositedPresentationTest, UpToDateCacheSuppressesPrewarm) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  EXPECT_FALSE(state.shouldPrewarm(Entity(7), /*currentVersion=*/3, Vector2i(100, 100),
                                   /*dragActive=*/false));
}

TEST(CompositedPresentationTest, CachedTexturesDisplayZeroTranslationWithoutActiveDrag) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 0.0);
  EXPECT_TRUE(state.hasCachedTextures);
}

TEST(CompositedPresentationTest, CachedTilesRemainVisibleAtIdle) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_TRUE(state.hasCachedTextures);

  SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .translation = Vector2d(4.0, 0.0),
  };
  ASSERT_TRUE(state.presentationPreview(active).has_value());
  EXPECT_EQ(state.presentationPreview(active)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(active)->translation.x, 4.0);
  EXPECT_TRUE(state.hasCachedTextures);
}

TEST(CompositedPresentationTest, ActiveDragForDifferentEntityKeepsPreviousTilesVisible) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  SelectTool::ActiveDragPreview active{
      .entity = Entity(8),
      .translation = Vector2d(4.0, 0.0),
  };
  ASSERT_TRUE(state.presentationPreview(active).has_value());
  EXPECT_EQ(state.presentationPreview(active)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(active)->translation.x, 0.0);
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_EQ(state.cachedEntity, Entity(7));
}

TEST(CompositedPresentationTest, MouseUpKeepsSettlingPreviewUntilFullRenderLands) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  SelectTool::ActiveDragPreview preview{.entity = Entity(7), .translation = Vector2d(12.0, 5.0)};
  state.beginSettling(preview, /*targetVersion=*/4);

  EXPECT_TRUE(state.hasCachedTextures);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 12.0);

  // Below-target render leaves settlingPreview in place — the settling
  // window hasn't closed yet, so the settling preview still drives the
  // displayed translation.
  state.noteFullRenderLanded(/*landedVersion=*/3);
  EXPECT_TRUE(state.hasCachedTextures);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 12.0);

  // The target render closes the settling window, clears the preview,
  // and leaves cached textures available at zero display offset.
  state.noteFullRenderLanded(/*landedVersion=*/4);
  EXPECT_TRUE(state.hasCachedTextures);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0)
      << "Post-settle preview should fall to the zero-offset cached path.";
}

TEST(CompositedPresentationTest, SelectionChangeClearsSettlingState) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(3.0, 2.0),
      },
      /*targetVersion=*/4);
  state.noteFullRenderLanded(/*landedVersion=*/4);

  state.clearSettlingIfSelectionChanged(Entity(8), /*dragActive=*/false);
  EXPECT_FALSE(state.waitingForFullRender);
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_TRUE(state.presentationPreview(std::nullopt).has_value());
}

TEST(CompositedPresentationTest, SelectionChangeDoesNotClearSettlingWhileWaitingForFullRender) {
  CompositedPresentation state;
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

TEST(CompositedPresentationTest, FullRenderLandedDoesNotClearCachedTextures) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_TRUE(state.hasCachedTextures);

  state.noteFullRenderLanded(/*landedVersion=*/3);
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_EQ(state.cachedEntity, Entity(7));
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
}

// Selection-clear keeps the cached document image visible. It only removes
// selection/settling state; the next document render atomically replaces the tiles.
TEST(CompositedPresentationTest, SelectionClearKeepsCachedTexturesVisible) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  ASSERT_TRUE(state.hasCachedTextures);

  state.clearSettlingIfSelectionChanged(/*selectedEntity=*/entt::null,
                                        /*dragActive=*/false);
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_EQ(state.cachedEntity, Entity(7));
}

TEST(CompositedPresentationTest, SettlingCompletionTriggersPrewarmOnNextSelection) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(5.0, 0.0),
      },
      /*targetVersion=*/4);

  // The cache remains live after settling, but its older version still
  // triggers prewarm for the current document version.
  state.noteFullRenderLanded(/*landedVersion=*/4);
  EXPECT_TRUE(state.shouldPrewarm(Entity(7), /*currentVersion=*/4, Vector2i(100, 100),
                                  /*dragActive=*/false));
}

TEST(CompositedPresentationTest, SettlingViaCompositedRenderKeepsCachedTextures) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(5.0, 0.0),
      },
      /*targetVersion=*/4);

  // Composited settle keeps the drag offset alive until selection chrome
  // catches up, so overlay/AABB state and document pixels change together.
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100));
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_FALSE(state.waitingForFullRender);
  EXPECT_TRUE(state.waitingForChromeRefresh);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 5.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 0.0);
  EXPECT_TRUE(state.hasCachedTextures);

  state.noteChromeRefreshCompleted(/*refreshedVersion=*/4);
  EXPECT_FALSE(state.waitingForChromeRefresh);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 0.0);
  EXPECT_TRUE(state.hasCachedTextures);
}

TEST(CompositedPresentationTest, CompositedSettleKeepsOffsetUntilChromeRefreshCompletes) {
  CompositedPresentation state;
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

TEST(CompositedPresentationTest, EntityChangeAfterSettlingReplacesCachedTextures) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(5.0, 0.0),
      },
      /*targetVersion=*/4);

  state.noteFullRenderLanded(/*landedVersion=*/4);

  state.noteCachedTextures(Entity(9), /*version=*/5, Vector2i(100, 100));
  EXPECT_EQ(state.cachedEntity, Entity(9));
  EXPECT_TRUE(state.hasCachedTextures);
}

TEST(CompositedPresentationTest, ClearSettlingIfSelectionChangedKeepsTexturesAfterComposedSettle) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(5.0, 0.0),
      },
      /*targetVersion=*/4);

  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100));
  state.noteChromeRefreshCompleted(/*refreshedVersion=*/4);
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_FALSE(state.waitingForFullRender);

  state.clearSettlingIfSelectionChanged(Entity(9), /*dragActive=*/false);

  EXPECT_FALSE(state.settlingPreview.has_value());
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_EQ(state.cachedEntity, Entity(7));
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
}

TEST(CompositedPresentationTest, ClearSettlingIfSelectionChangedKeepsTexturesWithoutSettle) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_TRUE(state.hasCachedTextures);

  state.clearSettlingIfSelectionChanged(Entity(9), /*dragActive=*/false);

  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_EQ(state.cachedEntity, Entity(7));

  EXPECT_TRUE(state.shouldPrewarm(Entity(9), /*currentVersion=*/3, Vector2i(100, 100),
                                  /*dragActive=*/false));
}

TEST(CompositedPresentationTest, DeselectionKeepsCachedTexturesVisible) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_TRUE(state.hasCachedTextures);

  state.clearSettlingIfSelectionChanged(entt::null, /*dragActive=*/false);

  EXPECT_TRUE(state.hasCachedTextures);
}

TEST(CompositedPresentationTest, ActiveDragPreventsTextureClearing) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  state.clearSettlingIfSelectionChanged(Entity(9), /*dragActive=*/true);

  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_EQ(state.cachedEntity, Entity(7));
}

}  // namespace
}  // namespace donner::editor
