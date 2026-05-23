#include "donner/editor/PresentationRenderScheduler.h"

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

const Vector2i kCanvasSize(100, 100);

PresentationRenderScheduleInput Input(
    Entity selectedEntity, std::uint64_t version = 1,
    std::optional<SelectTool::ActiveDragPreview> activeDragPreview = std::nullopt) {
  return PresentationRenderScheduleInput{
      .selectedEntity = selectedEntity,
      .activeDragPreview = activeDragPreview,
      .currentVersion = version,
      .currentCanvasSize = kCanvasSize,
  };
}

TEST(PresentationRenderSchedulerTest, FirstRenderRequestsRegularAndPrewarm) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7)));

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_FALSE(decision.needsCompositedLayerCapture);
  EXPECT_TRUE(decision.needsCompositedPrewarm);
  EXPECT_TRUE(decision.needsRegularRender);
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_EQ(decision.dragPreview->entity, Entity(7));
  EXPECT_EQ(decision.dragPreview->interactionKind, svg::compositor::InteractionHint::Selection);
}

TEST(PresentationRenderSchedulerTest, RepeatedUpToDateSelectionDoesNotRequestRender) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision first =
      scheduler.evaluate(presentation, Input(Entity(7)));
  scheduler.noteRenderCompleted(first.currentVersion, first.currentCanvasSize);
  presentation.noteCachedTextures(Entity(7), /*version=*/1, kCanvasSize);

  const PresentationRenderScheduleDecision second =
      scheduler.evaluate(presentation, Input(Entity(7)));

  EXPECT_FALSE(second.shouldRequestRender());
  EXPECT_FALSE(second.dragPreview.has_value());
}

TEST(PresentationRenderSchedulerTest, ActiveDragWithStaleCacheRequestsDragCapture) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision warmOtherEntity =
      scheduler.evaluate(presentation, Input(Entity(8)));
  scheduler.noteRenderCompleted(warmOtherEntity.currentVersion, warmOtherEntity.currentCanvasSize);
  presentation.noteCachedTextures(Entity(8), /*version=*/1, kCanvasSize);

  const SelectTool::ActiveDragPreview activeDrag{
      .entity = Entity(7),
      .translation = Vector2d(4.0, 0.0),
      .dragGeneration = 9,
  };
  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/1, activeDrag));

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsCompositedLayerCapture);
  EXPECT_FALSE(decision.needsRegularRender);
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_EQ(decision.dragPreview->entity, Entity(7));
  EXPECT_EQ(decision.dragPreview->interactionKind, svg::compositor::InteractionHint::ActiveDrag);
  EXPECT_EQ(decision.dragPreview->translation, Vector2d(4.0, 0.0));
  EXPECT_EQ(decision.dragPreview->dragGeneration, 9u);
}

TEST(PresentationRenderSchedulerTest, ActiveDragWithMatchingCacheDoesNotUploadAgain) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision warm =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/1));
  scheduler.noteRenderCompleted(warm.currentVersion, warm.currentCanvasSize);
  presentation.noteCachedTextures(Entity(7), /*version=*/1, kCanvasSize);

  const SelectTool::ActiveDragPreview activeDrag{
      .entity = Entity(7),
      .translation = Vector2d(9.0, 0.0),
      .dragGeneration = 14,
  };
  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/8, activeDrag));

  EXPECT_FALSE(decision.shouldRequestRender())
      << "Active drag should transform cached promoted textures in the presenter; the DOM "
         "version changes every mouse move and must not trigger a new bitmap upload.";
  EXPECT_FALSE(decision.needsCompositedLayerCapture);
  EXPECT_FALSE(decision.needsRegularRender);
}

TEST(PresentationRenderSchedulerTest, SettledSelectionRefreshRequestsSelectionHint) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;
  presentation.noteCachedTextures(Entity(7), /*version=*/3, kCanvasSize);
  presentation.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(5.0, 0.0),
      },
      /*targetVersion=*/4);

  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/4));

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsCompositedPrewarm);
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_EQ(decision.dragPreview->entity, Entity(7));
  EXPECT_EQ(decision.dragPreview->interactionKind, svg::compositor::InteractionHint::Selection);
}

TEST(PresentationRenderSchedulerTest, RegularRenderIsSuppressedOnlyAfterCompletion) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision first =
      scheduler.evaluate(presentation, Input(entt::null));
  ASSERT_TRUE(first.shouldRequestRender());
  EXPECT_TRUE(first.needsRegularRender);

  const PresentationRenderScheduleDecision retry =
      scheduler.evaluate(presentation, Input(entt::null));
  EXPECT_TRUE(retry.shouldRequestRender())
      << "A posted-but-cancelled render must not make the canvas look completed";
  EXPECT_TRUE(retry.needsRegularRender);

  scheduler.noteRenderCompleted(first.currentVersion, first.currentCanvasSize);

  const PresentationRenderScheduleDecision completed =
      scheduler.evaluate(presentation, Input(entt::null));
  EXPECT_FALSE(completed.shouldRequestRender());
}

}  // namespace
}  // namespace donner::editor
