#include "donner/editor/PresentationRenderScheduler.h"

#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace donner::editor {
namespace {

const Vector2i kCanvasSize(100, 100);

EditorRasterViewport RasterViewport(const Vector2d& documentTopLeft = Vector2d::Zero()) {
  EditorRasterViewport viewport;
  viewport.documentRect = Box2d(documentTopLeft, documentTopLeft + Vector2d(100.0, 100.0));
  viewport.outputSizePx = kCanvasSize;
  viewport.semanticCanvasSizePx = kCanvasSize;
  viewport.outputFromDocument = Transform2d::Translate(-documentTopLeft);
  return viewport;
}

PresentationRenderScheduleInput Input(
    Entity selectedEntity, std::uint64_t version = 1,
    std::optional<SelectTool::ActiveDragPreview> activeDragPreview = std::nullopt,
    EditorRasterViewport rasterViewport = RasterViewport(),
    const Vector2i& currentCanvasSize = kCanvasSize) {
  return PresentationRenderScheduleInput{
      .selectedEntity = selectedEntity,
      .selectedExtraEntities = std::move(selectedExtraEntities),
      .activeDragPreview = activeDragPreview,
      .currentVersion = version,
      .currentCanvasSize = currentCanvasSize,
      .currentRasterViewport = rasterViewport,
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

TEST(PresentationRenderSchedulerTest, SelectionPrewarmCarriesGroupedSelectionEntities) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision decision = scheduler.evaluate(
      presentation,
      Input(Entity(7), /*version=*/1, std::nullopt, RasterViewport(), kCanvasSize, {Entity(8)}));

  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_EQ(decision.dragPreview->entity, Entity(7));
  EXPECT_EQ(decision.dragPreview->extraEntities, std::vector<Entity>{Entity(8)});
  EXPECT_EQ(decision.dragPreview->interactionKind, svg::compositor::InteractionHint::Selection);
}

TEST(PresentationRenderSchedulerTest, ChangedGroupedSelectionRewarmsSamePrimaryEntity) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision warm = scheduler.evaluate(
      presentation,
      Input(Entity(7), /*version=*/1, std::nullopt, RasterViewport(), kCanvasSize, {Entity(8)}));
  ASSERT_TRUE(warm.dragPreview.has_value());
  scheduler.noteRenderCompleted(warm.currentVersion, warm.currentCanvasSize,
                                warm.currentRasterViewport);
  presentation.noteCachedTextures(Entity(7), /*version=*/1, kCanvasSize,
                                  SelectTool::ActiveDragPreview{
                                      .entity = Entity(7),
                                      .extraEntities = {Entity(8)},
                                  });

  const PresentationRenderScheduleDecision changed = scheduler.evaluate(
      presentation,
      Input(Entity(7), /*version=*/1, std::nullopt, RasterViewport(), kCanvasSize, {Entity(9)}));

  EXPECT_TRUE(changed.needsCompositedPrewarm);
  ASSERT_TRUE(changed.dragPreview.has_value());
  EXPECT_EQ(changed.dragPreview->extraEntities, std::vector<Entity>{Entity(9)});
}

TEST(PresentationRenderSchedulerTest, RepeatedUpToDateSelectionDoesNotRequestRender) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision first =
      scheduler.evaluate(presentation, Input(Entity(7)));
  scheduler.noteRenderCompleted(first.currentVersion, first.currentCanvasSize,
                                first.currentRasterViewport);
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
  scheduler.noteRenderCompleted(warmOtherEntity.currentVersion, warmOtherEntity.currentCanvasSize,
                                warmOtherEntity.currentRasterViewport);
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
  scheduler.noteRenderCompleted(warm.currentVersion, warm.currentCanvasSize,
                                warm.currentRasterViewport);
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

TEST(PresentationRenderSchedulerTest, ActiveDragWithMatchingCacheSuppressesMovedRasterViewport) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision warm =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/1));
  scheduler.noteRenderCompleted(warm.currentVersion, warm.currentCanvasSize,
                                warm.currentRasterViewport);
  presentation.noteCachedTextures(Entity(7), /*version=*/1, kCanvasSize);

  const SelectTool::ActiveDragPreview activeDrag{
      .entity = Entity(7),
      .translation = Vector2d(9.0, 0.0),
      .dragGeneration = 14,
  };
  const PresentationRenderScheduleDecision decision = scheduler.evaluate(
      presentation,
      Input(Entity(7), /*version=*/8, activeDrag, RasterViewport(Vector2d(10.0, 0.0))));

  EXPECT_FALSE(decision.shouldRequestRender())
      << "Zoom/pan during an active drag must keep using the presenter-transformed cached content; "
         "requesting a regular render here re-rasterizes every cached span on zoom+drag frames.";
  EXPECT_FALSE(decision.needsCompositedLayerCapture);
  EXPECT_FALSE(decision.needsRegularRender);
}

TEST(PresentationRenderSchedulerTest, ActiveDragWithMatchingCacheSuppressesCanvasSizeChange) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision warm =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/1));
  scheduler.noteRenderCompleted(warm.currentVersion, warm.currentCanvasSize,
                                warm.currentRasterViewport);
  presentation.noteCachedTextures(Entity(7), /*version=*/1, kCanvasSize);

  const SelectTool::ActiveDragPreview activeDrag{
      .entity = Entity(7),
      .translation = Vector2d(9.0, 0.0),
      .dragGeneration = 14,
  };
  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation,
                         Input(Entity(7), /*version=*/8, activeDrag, RasterViewport(),
                               Vector2i(kCanvasSize.x + 20, kCanvasSize.y + 20)));

  EXPECT_FALSE(decision.shouldRequestRender())
      << "Continuous zoom changes the desired canvas size. During active drag the presenter should "
         "keep transforming the existing cached content and defer the crisp re-render until idle.";
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

  scheduler.noteRenderCompleted(first.currentVersion, first.currentCanvasSize,
                                first.currentRasterViewport);

  const PresentationRenderScheduleDecision completed =
      scheduler.evaluate(presentation, Input(entt::null));
  EXPECT_FALSE(completed.shouldRequestRender());
}

TEST(PresentationRenderSchedulerTest, SameCanvasMovedRasterViewportRequestsRegularRender) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision first =
      scheduler.evaluate(presentation, Input(entt::null));
  scheduler.noteRenderCompleted(first.currentVersion, first.currentCanvasSize,
                                first.currentRasterViewport);

  const PresentationRenderScheduleDecision movedViewport = scheduler.evaluate(
      presentation,
      Input(entt::null, /*version=*/1, std::nullopt, RasterViewport(Vector2d(10.0, 0.0))));

  EXPECT_TRUE(movedViewport.shouldRequestRender());
  EXPECT_TRUE(movedViewport.needsRegularRender)
      << "High-zoom panning keeps the same output size but changes the document window.";
}

TEST(PresentationRenderSchedulerTest, SelectedRasterViewportMoveKeepsSelectionPrewarmHint) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision first =
      scheduler.evaluate(presentation, Input(Entity(7)));
  scheduler.noteRenderCompleted(first.currentVersion, first.currentCanvasSize,
                                first.currentRasterViewport);
  presentation.noteCachedTextures(Entity(7), /*version=*/1, kCanvasSize);

  const PresentationRenderScheduleDecision movedViewport = scheduler.evaluate(
      presentation,
      Input(Entity(7), /*version=*/1, std::nullopt, RasterViewport(Vector2d(10.0, 0.0))));

  EXPECT_TRUE(movedViewport.shouldRequestRender());
  EXPECT_TRUE(movedViewport.needsRegularRender);
  EXPECT_TRUE(movedViewport.needsCompositedPrewarm)
      << "A selected regular render after zoom/pan must keep the selected layer promoted; "
         "otherwise a full-canvas fallback replaces the drag-target tile before the next drag.";
  ASSERT_TRUE(movedViewport.dragPreview.has_value());
  EXPECT_EQ(movedViewport.dragPreview->entity, Entity(7));
  EXPECT_EQ(movedViewport.dragPreview->interactionKind,
            svg::compositor::InteractionHint::Selection);
}

}  // namespace
}  // namespace donner::editor
