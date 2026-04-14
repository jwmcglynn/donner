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
  state.beginSettling(SelectTool::ActiveDragPreview{
      .entity = Entity(7),
      .translation = Vector2d(3.0, 2.0),
  }, /*targetVersion=*/4);

  state.clearSettlingIfSelectionChanged(Entity(8), /*dragActive=*/false);
  EXPECT_FALSE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_FALSE(state.waitingForFullRender);
}

}  // namespace
}  // namespace donner::editor
