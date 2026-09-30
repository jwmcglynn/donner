#include "donner/editor/CompositedPresentation.h"

#include <sstream>

#include "donner/editor/EditorApp.h"
#include "donner/editor/tests/FramePresentationTestAccess.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {
using Phase = CompositedPresentation::Phase;

TEST(CompositedPresentationTest, EmptyStateHasNoRasterOrReleasedIntent) {
  CompositedPresentation state;
  EXPECT_EQ(state.diagnostics().phase, Phase::NoCache);
  EXPECT_FALSE(state.hasCachedTextures());
  EXPECT_EQ(state.resources(), nullptr);
  EXPECT_FALSE(state.activePreviewForPresentation(std::nullopt).has_value());
}

TEST(CompositedPresentationTest, MissingPoseProvenanceRequestsCapture) {
  CompositedPresentation state;
  const SelectTool::ActiveDragPreview desired{.entity = Entity(7), .dragGeneration = 8};
  EXPECT_TRUE(state.needsCompositedLayerCapture(desired, 3, Vector2i(100, 100)));
  state.noteCachedTextures(Entity(7), 3, Vector2i(100, 100));
  EXPECT_TRUE(state.needsCompositedLayerCapture(desired, 3, Vector2i(100, 100)));
  EXPECT_FALSE(state.represents(desired));
}

TEST(CompositedPresentationTest, PureTranslationRecapturesBeforeOverdrawIsExhausted) {
  CompositedPresentation state;
  const SelectTool::ActiveDragPreview represented{
      .entity = Entity(7),
      .translation = Vector2d(40.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(40.0, 0.0)),
      .dragGeneration = 8,
  };
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100), represented);

  SelectTool::ActiveDragPreview active = represented;
  active.translation = Vector2d(150.0, 0.0);
  active.documentFromCachedDocument = Transform2d::Translate(active.translation);
  EXPECT_FALSE(state.needsCompositedLayerCapture(active, /*currentVersion=*/4, Vector2i(100, 100),
                                                 /*translationRecaptureDistanceDoc=*/128.0));

  active.translation = Vector2d(169.0, 0.0);
  active.documentFromCachedDocument = Transform2d::Translate(active.translation);
  EXPECT_TRUE(state.needsCompositedLayerCapture(active, /*currentVersion=*/5, Vector2i(100, 100),
                                                /*translationRecaptureDistanceDoc=*/128.0));
}

// A large affine scale drift past the threshold re-captures a crisp bitmap - the
// intentional anti-blur re-capture. The worker bakes the live transform into the
// fresh bitmap and `represented` is updated to that baked transform, so the
// presentation (`effective = represented^-1 * active`) compensates for the
// swapped-in image and the shape stays continuous (no pop). The seamless-swap
// behavior itself is pinned by the .rnr replay test.
TEST(CompositedPresentationTest, LargeAffineScaleDriftRequestsCrispRecapture) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  const SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .translation = Vector2d(6.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(6.0, 2.0)) * Transform2d::Scale(2.0),
      .dragGeneration = 8,
  };

  EXPECT_TRUE(state.needsCompositedLayerCapture(active, /*currentVersion=*/4, Vector2i(100, 100)))
      << "Scaling 2x past the crisp-recapture threshold should request a fresh, sharper bitmap.";
}

// A small affine change below the scale threshold must NOT re-capture - the
// presentation quad tracks it against the cached bitmap with no worker round-trip.
TEST(CompositedPresentationTest, SmallAffineScaleDriftTracksWithoutRecapture) {
  const SelectTool::ActiveDragPreview represented{
      .entity = Entity(7),
      .translation = Vector2d(6.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(6.0, 2.0)) * Transform2d::Scale(1.25),
      .dragGeneration = 8,
  };
  const SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .translation = Vector2d(7.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(7.0, 2.0)) * Transform2d::Scale(1.45),
      .dragGeneration = 8,
  };
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100), represented);

  EXPECT_FALSE(state.needsCompositedLayerCapture(active, /*currentVersion=*/5, Vector2i(100, 100)))
      << "Continuing an affine manipulation must not re-capture; the presentation quad tracks it.";
}

// Pure rotation likewise tracks via the presentation quad with no re-capture.
TEST(CompositedPresentationTest, PureRotationDoesNotRecapture) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100),
                           SelectTool::ActiveDragPreview{.entity = Entity(7), .dragGeneration = 8});

  const SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .documentFromCachedDocument = Transform2d::Rotate(0.6),
      .dragGeneration = 8,
  };

  EXPECT_FALSE(state.needsCompositedLayerCapture(active, /*currentVersion=*/5, Vector2i(100, 100)))
      << "Rotation (and any affine) tracks purely through the presentation quad.";
}

TEST(CompositedPresentationTest, MatchingAffineRepresentedPreviewSuppressesCaptureLoop) {
  const SelectTool::ActiveDragPreview represented{
      .entity = Entity(7),
      .translation = Vector2d(6.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(6.0, 2.0)) * Transform2d::Scale(1.25),
      .dragGeneration = 8,
  };
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100), represented);

  EXPECT_FALSE(
      state.needsCompositedLayerCapture(represented, /*currentVersion=*/4, Vector2i(100, 100)))
      << "Once an affine drag bitmap has landed for the current transform, the scheduler must not "
         "spin on another identical opportunistic capture.";
}

TEST(CompositedPresentationTest, ZeroScaleRepresentedPreviewRequestsFreshCapture) {
  const SelectTool::ActiveDragPreview represented{
      .entity = Entity(7),
      .documentFromCachedDocument = Transform2d::Scale(0.0),
      .dragGeneration = 8,
  };
  const SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .documentFromCachedDocument = Transform2d::Scale(1.1),
      .dragGeneration = 8,
  };
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100), represented);

  EXPECT_TRUE(state.needsCompositedLayerCapture(active, /*currentVersion=*/5, Vector2i(100, 100)))
      << "A degenerate represented affine cannot produce a stable scale-drift ratio.";
}

TEST(CompositedPresentationTest, ChangedAffineActiveDragRequestsNextCapture) {
  const SelectTool::ActiveDragPreview represented{
      .entity = Entity(7),
      .translation = Vector2d(6.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(6.0, 2.0)) * Transform2d::Scale(1.25),
      .dragGeneration = 8,
  };
  const SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .translation = Vector2d(7.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(7.0, 2.0)) * Transform2d::Scale(2.0),
      .dragGeneration = 8,
  };
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100), represented);

  EXPECT_TRUE(state.needsCompositedLayerCapture(active, /*currentVersion=*/5, Vector2i(100, 100)))
      << "Continuing an affine manipulation past the scale-drift threshold (1.25x -> 2.0x) should "
         "request the next sharper bitmap when the worker is free.";
}

TEST(CompositedPresentationTest, SmallReturnFromAffineCaptureDoesNotRecapture) {
  const SelectTool::ActiveDragPreview represented{
      .entity = Entity(7),
      .translation = Vector2d(6.0, 2.0),
      .documentFromCachedDocument =
          Transform2d::Translate(Vector2d(6.0, 2.0)) * Transform2d::Scale(1.25),
      .dragGeneration = 8,
  };
  const SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .translation = Vector2d(8.0, 2.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(8.0, 2.0)),
      .dragGeneration = 8,
  };
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100), represented);

  EXPECT_FALSE(state.needsCompositedLayerCapture(active, /*currentVersion=*/5, Vector2i(100, 100)));
}

TEST(CompositedPresentationTest, SelectionTriggersPrewarmWhenCacheMissing) {
  CompositedPresentation state;
  EXPECT_TRUE(state.shouldPrewarm(Entity(7), {}, /*currentVersion=*/3, Vector2i(100, 100),
                                  /*dragActive=*/false));
}

TEST(CompositedPresentationTest, UpToDateCacheSuppressesPrewarm) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  EXPECT_FALSE(state.shouldPrewarm(Entity(7), {}, /*currentVersion=*/3, Vector2i(100, 100),
                                   /*dragActive=*/false));
}

TEST(CompositedPresentationTest, CapturedMetadataIsNeverRelabeledForAnotherGesture) {
  const SelectTool::ActiveDragPreview captured{
      .entity = Entity(7),
      .translation = Vector2d(20.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(20.0, 0.0),
      .dragGeneration = 1};
  const SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .translation = Vector2d(5.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(5.0, 0.0),
      .dragGeneration = 2};
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), 3, Vector2i(100, 100), captured);
  for (const auto& intent :
       {std::optional(active), std::optional<SelectTool::ActiveDragPreview>()}) {
    const auto actual = state.presentationPreview(intent);
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(actual->dragGeneration, 1u);
    EXPECT_EQ(actual->translation, Vector2d(20.0, 0.0));
    EXPECT_TRUE(SamePresentationTransform(actual->documentFromCachedDocument,
                                          captured.documentFromCachedDocument));
  }
  EXPECT_FALSE(state.represents(active));
  EXPECT_TRUE(state.needsCompositedLayerCapture(active, 4, Vector2i(100, 100)));
}

TEST(CompositedPresentationTest, ReleaseRetainsIntentUntilCompleteRasterAcceptance) {
  CompositedPresentation state;
  const SelectTool::ActiveDragPreview captured{.entity = Entity(7), .dragGeneration = 1};
  const SelectTool::ActiveDragPreview released{
      .entity = Entity(7), .translation = Vector2d(12.0, 5.0), .dragGeneration = 1};
  state.noteCachedTextures(Entity(7), 3, Vector2i(100, 100), captured);
  state.beginSettling(released, 4);
  EXPECT_EQ(state.diagnostics().phase, Phase::SettlingForRender);
  state.noteCachedTextures(Entity(7), 3, Vector2i(100, 100), captured);
  ASSERT_TRUE(state.activePreviewForPresentation(std::nullopt).has_value());
  EXPECT_EQ(state.activePreviewForPresentation(std::nullopt)->translation, released.translation);
  EXPECT_EQ(state.presentationPreview(std::nullopt)->translation, captured.translation);
  state.noteCachedTextures(Entity(7), 4, Vector2i(100, 100), released);
  EXPECT_EQ(state.diagnostics().phase, Phase::Cached);
  EXPECT_FALSE(state.activePreviewForPresentation(std::nullopt).has_value());
  EXPECT_TRUE(state.hasCachedTextures());
}

TEST(CompositedPresentationTest, VersionAloneCannotFinishSettlingAtTheWrongPose) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect x="10" y="10" width="20" height="20"/></svg>)"));
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15.0, 15.0), MouseModifiers{});
  const auto identity = [&] {
    return PresentationIdentity{.captureId = 1,
                                .documentGeneration = app.document().documentGeneration(),
                                .version = app.document().currentFrameVersion(),
                                .geometryRevision = app.document().nonTransformRevision()};
  };
  const auto capture =
      CapturedPresentation::Capture(app.document().document(), identity(), app.selectedElements());
  tool.onMouseMove(app, Vector2d(45.0, 15.0), true);
  const auto desired = tool.activeDragPreview();
  ASSERT_TRUE(desired.has_value());
  CompositedPresentation state;
  state.beginSettling(desired, app.document().currentFrameVersion());
  state.notePreparedResources(FramePresentationTestAccess::resources(capture, {}), desired->entity,
                              std::nullopt);
  EXPECT_EQ(state.diagnostics().phase, Phase::SettlingForRender);
  ASSERT_TRUE(app.flushFrame());
  const auto rendered =
      CapturedPresentation::Capture(app.document().document(), identity(), app.selectedElements());
  state.notePreparedResources(FramePresentationTestAccess::resources(rendered, {}), desired->entity,
                              desired);
  EXPECT_EQ(state.diagnostics().phase, Phase::Cached);
  EXPECT_EQ(state.capturedPresentation(), rendered);
  EXPECT_FALSE(state.activePreviewForPresentation(std::nullopt).has_value());
}

TEST(CompositedPresentationTest, NewActiveGestureOverridesReleasedIntentWithoutChangingCapture) {
  CompositedPresentation state;
  const SelectTool::ActiveDragPreview captured{.entity = Entity(7), .dragGeneration = 1};
  state.noteCachedTextures(Entity(7), 3, Vector2i(100, 100), captured);
  state.beginSettling(captured, 4);
  const SelectTool::ActiveDragPreview active{.entity = Entity(7), .dragGeneration = 2};
  ASSERT_TRUE(state.activePreviewForPresentation(active).has_value());
  EXPECT_EQ(state.activePreviewForPresentation(active)->dragGeneration, 2u);
  EXPECT_EQ(state.presentationPreview(active)->dragGeneration, 1u);
}

TEST(CompositedPresentationTest, SelectionChangeClearsReleasedIntentAndPreservesRaster) {
  for (const Entity next : {Entity(8), Entity(entt::null)}) {
    CompositedPresentation state;
    state.noteCachedTextures(Entity(7), 3, Vector2i(100, 100));
    state.beginSettling(SelectTool::ActiveDragPreview{.entity = Entity(7)}, 4);
    state.clearSettlingIfSelectionChanged(next, true);
    EXPECT_TRUE(state.isWaitingForFullRender());
    state.clearSettlingIfSelectionChanged(next, false);
    EXPECT_FALSE(state.isWaitingForFullRender());
    EXPECT_TRUE(state.hasCachedTexturesForEntity(Entity(7)));
  }
}

TEST(CompositedPresentationTest, DiscardOnlyMatchingCache) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), 3, Vector2i(100, 100));
  state.beginSettling(SelectTool::ActiveDragPreview{.entity = Entity(7)}, 4);
  EXPECT_FALSE(state.discardCachedTexturesForEntity(entt::null));
  EXPECT_FALSE(state.discardCachedTexturesForEntity(Entity(8)));
  EXPECT_TRUE(state.hasCachedTexturesForEntity(Entity(7)));
  EXPECT_TRUE(state.discardCachedTexturesForEntity(Entity(7)));
  EXPECT_EQ(state.diagnostics().phase, Phase::NoCache);
}

TEST(CompositedPresentationTest, ClearingReleasedIntentPreservesOptionalCache) {
  CompositedPresentation state;
  state.beginSettling(std::nullopt, 4);
  EXPECT_EQ(state.diagnostics().phase, Phase::NoCache);
  state.noteCachedTextures(Entity(7), 3, Vector2i(100, 100));
  state.beginSettling(SelectTool::ActiveDragPreview{.entity = Entity(7)}, 4);
  state.beginSettling(std::nullopt, 4);
  EXPECT_EQ(state.diagnostics().phase, Phase::Cached);
  EXPECT_TRUE(state.hasCachedTexturesForEntity(Entity(7)));
}

TEST(CompositedPresentationTest, InvalidatingSelectedLayerRetainsCompleteSceneForReplacement) {
  EditorApp app;
  ASSERT_TRUE(
      app.loadFromString(R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
    <rect id="selected" width="20" height="20"/><rect x="50" width="20" height="20"/></svg>)"));
  app.setSelection(*app.document().document().querySelector("#selected"));
  const auto capture = CapturedPresentation::Capture(
      app.document().document(),
      PresentationIdentity{.captureId = 1,
                           .documentGeneration = app.document().documentGeneration()},
      app.selectedElements());
  const auto resources = FramePresentationTestAccess::resources(capture, {});
  const auto entity = capture->selection().front();
  CompositedPresentation state;
  state.notePreparedResources(resources, entity, std::nullopt);
  ASSERT_TRUE(state.discardCachedTexturesForEntity(entity));
  EXPECT_FALSE(state.hasCachedTexturesForEntity(entity));
  EXPECT_EQ(state.resources(), resources);
  EXPECT_EQ(state.capturedPresentation(), capture);
}

TEST(CompositedPresentationTest, InvalidResourceAdmissionDoesNotReplaceCurrentCache) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), 3, Vector2i(100, 100));
  state.notePreparedResources(nullptr, Entity(8), std::nullopt);
  state.notePreparedResources(FramePresentationTestAccess::resources(nullptr, {}), Entity(8),
                              std::nullopt);
  EXPECT_TRUE(state.hasCachedTexturesForEntity(Entity(7)));
}

TEST(CompositedPresentationTest, DiagnosticsAreDetachedAndPhasesHaveStableNames) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), 3, Vector2i(100, 100));
  auto copy = state.diagnostics();
  copy.cachedVersion = 90;
  EXPECT_EQ(state.diagnostics().cachedVersion, 3u);
  std::ostringstream output;
  output << Phase::NoCache << ',' << Phase::Cached << ',' << Phase::SettlingForRender;
  EXPECT_EQ(output.str(), "NoCache,Cached,SettlingForRender");
}

}  // namespace
TEST(CompositedPresentationTest, NewerCommittedTransformSupersedesReleasedDragIntent) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect id="r" x="10" y="10" width="20" height="20"/></svg>)"));
  SelectTool tool;
  tool.onMouseDown(app, Vector2d(15, 15), MouseModifiers{});
  tool.onMouseMove(app, Vector2d(35, 15), true);
  ASSERT_TRUE(app.flushFrame());
  const auto released = tool.activeDragPreview();
  ASSERT_TRUE(released.has_value());
  CompositedPresentation state;
  state.beginSettling(released, app.document().currentFrameVersion());
  tool.onMouseUp(app, Vector2d(35, 15));
  app.applyMutation(EditorCommand::SetTransformCommand(app.selectedElements().front(),
                                                       Transform2d::Translate(50, 0)));
  ASSERT_TRUE(app.flushFrame());
  const auto capture = CapturedPresentation::Capture(
      app.document().document(),
      PresentationIdentity{.captureId = 2,
                           .documentGeneration = app.document().documentGeneration(),
                           .version = app.document().currentFrameVersion(),
                           .geometryRevision = app.document().nonTransformRevision()},
      app.selectedElements());
  ASSERT_NE(capture, nullptr);
  state.notePreparedResources(FramePresentationTestAccess::resources(capture, {}), released->entity,
                              std::nullopt);
  EXPECT_EQ(state.diagnostics().phase, Phase::Cached);
  EXPECT_FALSE(state.activePreviewForPresentation(std::nullopt).has_value());
}

}  // namespace donner::editor
