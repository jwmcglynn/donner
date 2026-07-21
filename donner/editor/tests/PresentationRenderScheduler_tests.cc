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
    const Vector2i& currentCanvasSize = kCanvasSize, std::vector<Entity> selectedExtraEntities = {},
    bool forceSelectedLayerRasterization = false) {
  return PresentationRenderScheduleInput{
      .selectedEntity = selectedEntity,
      .selectedExtraEntities = std::move(selectedExtraEntities),
      .activeDragPreview = activeDragPreview,
      .forceSelectedLayerRasterization = forceSelectedLayerRasterization,
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

TEST(PresentationRenderSchedulerTest, PresentationSettingChangeForcesCurrentDocumentRender) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision first =
      scheduler.evaluate(presentation, Input(entt::null, /*version=*/1));
  scheduler.noteRenderCompleted(first.currentVersion, first.currentCanvasSize,
                                first.currentRasterViewport);

  PresentationRenderScheduleInput changedPresentation = Input(entt::null, /*version=*/1);
  changedPresentation.forcePresentationRefresh = true;
  const PresentationRenderScheduleDecision refresh =
      scheduler.evaluate(presentation, changedPresentation);

  EXPECT_TRUE(refresh.shouldRequestRender());
  EXPECT_TRUE(refresh.needsRegularRender);
}

TEST(PresentationRenderSchedulerTest, DirectSurfaceSelectionOnlyCacheMissDoesNotRequestRender) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision documentRender =
      scheduler.evaluate(presentation, Input(entt::null, /*version=*/1));
  ASSERT_TRUE(documentRender.needsRegularRender);
  scheduler.noteRenderCompleted(documentRender.currentVersion, documentRender.currentCanvasSize,
                                documentRender.currentRasterViewport);

  PresentationRenderScheduleInput selection = Input(Entity(7), /*version=*/1);
  selection.requiresRenderedActiveDragPresentation = true;
  selection.deferIdentityActiveDragCapture = true;
  selection.selectionOnlyPrewarmMayTriggerRender = false;
  const PresentationRenderScheduleDecision decision = scheduler.evaluate(presentation, selection);

  EXPECT_FALSE(decision.shouldRequestRender())
      << "A direct worker surface cannot consume a selected-layer prewarm on the UI thread. "
         "Selection chrome should update without posting an identical document frame.";
  EXPECT_FALSE(decision.needsCompositedPrewarm);
  EXPECT_FALSE(decision.dragPreview.has_value());
}

TEST(PresentationRenderSchedulerTest, CachedTexturePresenterStillPrewarmsSelectionOnlyCacheMiss) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision documentRender =
      scheduler.evaluate(presentation, Input(entt::null, /*version=*/1));
  scheduler.noteRenderCompleted(documentRender.currentVersion, documentRender.currentCanvasSize,
                                documentRender.currentRasterViewport);

  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/1));

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsCompositedPrewarm);
  EXPECT_FALSE(decision.needsRegularRender);
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_EQ(decision.dragPreview->interactionKind, svg::compositor::InteractionHint::Selection);
}

TEST(PresentationRenderSchedulerTest, DirectSurfaceSelectionPiggybacksOnDocumentInvalidation) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision documentRender =
      scheduler.evaluate(presentation, Input(entt::null, /*version=*/1));
  scheduler.noteRenderCompleted(documentRender.currentVersion, documentRender.currentCanvasSize,
                                documentRender.currentRasterViewport);

  PresentationRenderScheduleInput changedDocument = Input(Entity(7), /*version=*/2);
  changedDocument.selectionOnlyPrewarmMayTriggerRender = false;
  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, changedDocument);

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsRegularRender);
  EXPECT_TRUE(decision.needsCompositedPrewarm);
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_EQ(decision.dragPreview->entity, Entity(7));
  EXPECT_EQ(decision.dragPreview->interactionKind, svg::compositor::InteractionHint::Selection);
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
  EXPECT_TRUE(decision.dragPreview->forceLayerRasterization);
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

TEST(PresentationRenderSchedulerTest, DirectSurfaceRequestsChangedCachedDragTransform) {
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
  PresentationRenderScheduleInput input = Input(Entity(7), /*version=*/8, activeDrag);
  input.requiresRenderedActiveDragPresentation = true;
  input.selectionOnlyPrewarmMayTriggerRender = false;
  const PresentationRenderScheduleDecision changed = scheduler.evaluate(presentation, input);

  EXPECT_TRUE(changed.shouldRequestRender());
  EXPECT_FALSE(changed.needsCompositedLayerCapture)
      << "The cached layer remains valid; only its direct-surface composition changed.";
  EXPECT_TRUE(changed.needsRenderedActiveDragPresentation);
  ASSERT_TRUE(changed.dragPreview.has_value());
  EXPECT_FALSE(changed.dragPreview->forceLayerRasterization);

  scheduler.noteRenderCompleted(changed.currentVersion, changed.currentCanvasSize,
                                changed.currentRasterViewport);
  presentation.noteCachedTextures(Entity(7), /*version=*/8, kCanvasSize, activeDrag);
  const PresentationRenderScheduleDecision represented = scheduler.evaluate(presentation, input);
  EXPECT_FALSE(represented.shouldRequestRender())
      << "A direct surface must not redraw continuously after it represents the live transform.";
  EXPECT_FALSE(represented.needsRenderedActiveDragPresentation);
}

TEST(PresentationRenderSchedulerTest, DirectSurfaceMatchingDragStillRendersChangedVersion) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;
  const SelectTool::ActiveDragPreview representedDrag{
      .entity = Entity(7),
      .translation = Vector2d(9.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(9.0, 0.0)),
      .dragGeneration = 14,
  };
  scheduler.noteRenderCompleted(/*completedVersion=*/1, kCanvasSize, RasterViewport());
  presentation.noteCachedTextures(Entity(7), /*version=*/1, kCanvasSize, representedDrag);

  PresentationRenderScheduleInput input = Input(Entity(7), /*version=*/2, representedDrag);
  input.requiresRenderedActiveDragPresentation = true;
  input.selectionOnlyPrewarmMayTriggerRender = false;
  const PresentationRenderScheduleDecision decision = scheduler.evaluate(presentation, input);

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsRegularRender);
  EXPECT_FALSE(decision.needsCompositedLayerCapture);
  EXPECT_FALSE(decision.needsRenderedActiveDragPresentation)
      << "The direct surface already represents this drag transform; the document version is the "
         "only render trigger.";
}

TEST(PresentationRenderSchedulerTest, DirectSurfaceMatchingDragStillRendersChangedCanvasSize) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;
  const SelectTool::ActiveDragPreview representedDrag{
      .entity = Entity(7),
      .translation = Vector2d(9.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(9.0, 0.0)),
      .dragGeneration = 14,
  };
  scheduler.noteRenderCompleted(/*completedVersion=*/1, kCanvasSize, RasterViewport());
  presentation.noteCachedTextures(Entity(7), /*version=*/1, kCanvasSize, representedDrag);

  PresentationRenderScheduleInput input =
      Input(Entity(7), /*version=*/1, representedDrag, RasterViewport(),
            Vector2i(kCanvasSize.x + 20, kCanvasSize.y + 20));
  input.requiresRenderedActiveDragPresentation = true;
  input.selectionOnlyPrewarmMayTriggerRender = false;
  const PresentationRenderScheduleDecision decision = scheduler.evaluate(presentation, input);

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsRegularRender);
  EXPECT_FALSE(decision.needsCompositedLayerCapture);
  EXPECT_FALSE(decision.needsRenderedActiveDragPresentation)
      << "The direct surface already represents this drag transform; canvas size is the only "
         "render trigger.";
}

TEST(PresentationRenderSchedulerTest, DirectSurfaceMatchingDragStillRendersChangedRasterViewport) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;
  const SelectTool::ActiveDragPreview representedDrag{
      .entity = Entity(7),
      .translation = Vector2d(9.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(9.0, 0.0)),
      .dragGeneration = 14,
  };
  scheduler.noteRenderCompleted(/*completedVersion=*/1, kCanvasSize, RasterViewport());
  presentation.noteCachedTextures(Entity(7), /*version=*/1, kCanvasSize, representedDrag);

  PresentationRenderScheduleInput input =
      Input(Entity(7), /*version=*/1, representedDrag, RasterViewport(Vector2d(10.0, 0.0)));
  input.requiresRenderedActiveDragPresentation = true;
  input.selectionOnlyPrewarmMayTriggerRender = false;
  const PresentationRenderScheduleDecision decision = scheduler.evaluate(presentation, input);

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsRegularRender);
  EXPECT_FALSE(decision.needsCompositedLayerCapture);
  EXPECT_FALSE(decision.needsRenderedActiveDragPresentation)
      << "The direct surface already represents this drag transform; raster viewport is the only "
         "render trigger.";
}

TEST(PresentationRenderSchedulerTest, DirectSurfaceDefersIdentityCaptureUntilPointerMoves) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const SelectTool::ActiveDragPreview identityDrag{
      .entity = Entity(7),
      .dragGeneration = 14,
  };
  PresentationRenderScheduleInput input = Input(Entity(7), /*version=*/1, identityDrag);
  input.requiresRenderedActiveDragPresentation = true;
  input.deferIdentityActiveDragCapture = true;
  input.selectionOnlyPrewarmMayTriggerRender = false;
  scheduler.noteRenderCompleted(/*completedVersion=*/1, kCanvasSize, RasterViewport());

  const PresentationRenderScheduleDecision identity = scheduler.evaluate(presentation, input);
  EXPECT_FALSE(identity.shouldRequestRender())
      << "Mouse-down does not change document pixels and must not race the first move with a stale "
         "surface handoff.";
  EXPECT_FALSE(identity.needsCompositedLayerCapture);
  EXPECT_FALSE(identity.needsRenderedActiveDragPresentation);

  input.activeDragPreview->translation = Vector2d(9.0, 0.0);
  input.activeDragPreview->documentFromCachedDocument = Transform2d::Translate(Vector2d(9.0, 0.0));
  const PresentationRenderScheduleDecision moved = scheduler.evaluate(presentation, input);
  EXPECT_TRUE(moved.shouldRequestRender());
  EXPECT_TRUE(moved.needsCompositedLayerCapture);
}

TEST(PresentationRenderSchedulerTest, AffineActiveDragWithMatchingCacheRequestsLayerCapture) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision warm =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/1));
  scheduler.noteRenderCompleted(warm.currentVersion, warm.currentCanvasSize,
                                warm.currentRasterViewport);
  presentation.noteCachedTextures(Entity(7), /*version=*/1, kCanvasSize);

  const SelectTool::ActiveDragPreview activeDrag{
      .entity = Entity(7),
      .translation = Vector2d(9.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(9.0, 2.0)) * Transform2d::Scale(2.0),
      .dragGeneration = 14,
  };
  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/8, activeDrag));

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsCompositedLayerCapture);
  EXPECT_FALSE(decision.needsRegularRender)
      << "Affine drag refreshes should be drag-layer captures, not full regular canvas renders.";
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_EQ(decision.dragPreview->entity, Entity(7));
  EXPECT_FALSE(decision.dragPreview->documentFromCachedDocument.isTranslation());
  EXPECT_TRUE(decision.dragPreview->forceLayerRasterization);
}

TEST(PresentationRenderSchedulerTest, MatchingAffineDragCaptureDoesNotRequestAgain) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const SelectTool::ActiveDragPreview representedDrag{
      .entity = Entity(7),
      .translation = Vector2d(9.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(9.0, 2.0)) * Transform2d::Scale(1.5),
      .dragGeneration = 14,
  };
  scheduler.noteRenderCompleted(/*completedVersion=*/8, kCanvasSize, RasterViewport());
  presentation.noteCachedTextures(Entity(7), /*version=*/8, kCanvasSize, representedDrag);

  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/8, representedDrag));

  EXPECT_FALSE(decision.shouldRequestRender())
      << "An opportunistic resize capture should be expansion-only: once the exact represented "
         "affine transform has landed, idle frames must not post duplicate captures.";
  EXPECT_FALSE(decision.needsCompositedLayerCapture);
  EXPECT_FALSE(decision.needsRegularRender);
}

TEST(PresentationRenderSchedulerTest, ChangedAffineDragRequestsNextLayerCapture) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const SelectTool::ActiveDragPreview representedDrag{
      .entity = Entity(7),
      .translation = Vector2d(9.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(9.0, 2.0)) * Transform2d::Scale(1.5),
      .dragGeneration = 14,
  };
  const SelectTool::ActiveDragPreview activeDrag{
      .entity = Entity(7),
      .translation = Vector2d(11.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(11.0, 2.0)) * Transform2d::Scale(2.5),
      .dragGeneration = 14,
  };
  scheduler.noteRenderCompleted(/*completedVersion=*/8, kCanvasSize, RasterViewport());
  presentation.noteCachedTextures(Entity(7), /*version=*/8, kCanvasSize, representedDrag);

  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/9, activeDrag));

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsCompositedLayerCapture);
  EXPECT_FALSE(decision.needsRegularRender);
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_TRUE(decision.dragPreview->forceLayerRasterization);
}

TEST(PresentationRenderSchedulerTest, PureTranslationAfterAffineCaptureRequestsCrispLayerCapture) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const SelectTool::ActiveDragPreview representedDrag{
      .entity = Entity(7),
      .translation = Vector2d(9.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(9.0, 2.0)) * Transform2d::Scale(1.5),
      .dragGeneration = 14,
  };
  const SelectTool::ActiveDragPreview activeDrag{
      .entity = Entity(7),
      .translation = Vector2d(11.0, 2.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(11.0, 2.0)),
      .dragGeneration = 14,
  };
  scheduler.noteRenderCompleted(/*completedVersion=*/8, kCanvasSize, RasterViewport());
  presentation.noteCachedTextures(Entity(7), /*version=*/8, kCanvasSize, representedDrag);

  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/9, activeDrag));

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsCompositedLayerCapture);
  EXPECT_FALSE(decision.needsRegularRender);
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_TRUE(decision.dragPreview->forceLayerRasterization);
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
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/8, activeDrag, RasterViewport(),
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
  EXPECT_FALSE(decision.dragPreview->forceLayerRasterization);
}

TEST(PresentationRenderSchedulerTest, SettledAffineSelectionRefreshForcesLayerRasterization) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;
  presentation.noteCachedTextures(Entity(7), /*version=*/3, kCanvasSize);
  presentation.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .documentFromCachedDocument =
              Transform2d::Translate(Vector2d(15.0, -2.0)) * Transform2d::Rotate(0.8),
      },
      /*targetVersion=*/4);

  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/4));

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsCompositedPrewarm);
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_EQ(decision.dragPreview->entity, Entity(7));
  EXPECT_EQ(decision.dragPreview->interactionKind, svg::compositor::InteractionHint::Selection);
  EXPECT_TRUE(decision.dragPreview->forceLayerRasterization)
      << "After mouse-up, an affine drag target no longer has a live presenter baseline. The "
         "settled selection refresh must re-rasterize the selected layer instead of publishing "
         "the stale cached texture at identity.";
}

TEST(PresentationRenderSchedulerTest, MissingSelectedCacheForcesLayerRasterization) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;

  const PresentationRenderScheduleDecision decision =
      scheduler.evaluate(presentation, Input(Entity(7), /*version=*/4));

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_TRUE(decision.needsCompositedPrewarm);
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_EQ(decision.dragPreview->entity, Entity(7));
  EXPECT_EQ(decision.dragPreview->interactionKind, svg::compositor::InteractionHint::Selection);
  EXPECT_TRUE(decision.dragPreview->forceLayerRasterization)
      << "When a selected layer cache was discarded after a paint/style edit, the worker must "
         "rerasterize the promoted layer instead of reusing metadata for the old texture.";
}

TEST(PresentationRenderSchedulerTest, StyleInvalidationForcesRasterizationWithCurrentCache) {
  PresentationRenderScheduler scheduler;
  CompositedPresentation presentation;
  scheduler.noteRenderCompleted(/*completedVersion=*/4, kCanvasSize, RasterViewport());
  presentation.noteCachedTextures(Entity(7), /*version=*/4, kCanvasSize);

  const PresentationRenderScheduleDecision decision = scheduler.evaluate(
      presentation, Input(Entity(7), /*version=*/4, std::nullopt, RasterViewport(), kCanvasSize, {},
                          /*forceSelectedLayerRasterization=*/true));

  EXPECT_TRUE(decision.shouldRequestRender());
  EXPECT_FALSE(decision.needsRegularRender);
  EXPECT_TRUE(decision.needsCompositedPrewarm);
  ASSERT_TRUE(decision.dragPreview.has_value());
  EXPECT_EQ(decision.dragPreview->entity, Entity(7));
  EXPECT_EQ(decision.dragPreview->interactionKind, svg::compositor::InteractionHint::Selection);
  EXPECT_TRUE(decision.dragPreview->forceLayerRasterization)
      << "A paint/style edit invalidates the selected layer's pixels even when its presentation "
         "cache already matches the current document version and canvas size.";
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
