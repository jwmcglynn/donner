#include "donner/editor/CompositedPresentation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::editor {
namespace {

using ::testing::ElementsAre;

using Phase = CompositedPresentation::Phase;

CompositedPresentation::DiagnosticsSnapshot Snapshot(const CompositedPresentation& state) {
  return state.diagnostics();
}

TEST(CompositedPresentationTest, DefaultDiagnosticsDescribeNoCache) {
  CompositedPresentation state;

  const auto snapshot = Snapshot(state);
  EXPECT_EQ(snapshot.phase, Phase::NoCache);
  EXPECT_FALSE(snapshot.hasCachedTextures);
  EXPECT_TRUE(snapshot.cachedEntity == entt::null);
  EXPECT_EQ(snapshot.cachedVersion, 0u);
  EXPECT_EQ(snapshot.cachedCanvasSize, Vector2i::Zero());
  EXPECT_FALSE(snapshot.settlingPreview.has_value());
  EXPECT_FALSE(snapshot.waitingForFullRender);
  EXPECT_FALSE(snapshot.waitingForChromeRefresh);
}

TEST(CompositedPresentationTest, DiagnosticsSnapshotIsDetachedFromState) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  auto snapshot = Snapshot(state);
  snapshot.hasCachedTextures = false;
  snapshot.cachedEntity = Entity(9);
  snapshot.cachedVersion = 99;
  snapshot.cachedCanvasSize = Vector2i(8, 8);

  const auto nextSnapshot = Snapshot(state);
  EXPECT_TRUE(nextSnapshot.hasCachedTextures);
  EXPECT_EQ(nextSnapshot.cachedEntity, Entity(7));
  EXPECT_EQ(nextSnapshot.cachedVersion, 3u);
  EXPECT_EQ(nextSnapshot.cachedCanvasSize, Vector2i(100, 100));
}

TEST(CompositedPresentationTest, SettlingWithoutCacheHasClosedDiagnostics) {
  CompositedPresentation state;
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(3.0, 2.0),
      },
      /*targetVersion=*/4);

  const auto snapshot = Snapshot(state);
  EXPECT_EQ(snapshot.phase, Phase::SettlingForRender);
  EXPECT_FALSE(snapshot.hasCachedTextures);
  ASSERT_TRUE(snapshot.settlingPreview.has_value());
  EXPECT_EQ(snapshot.settlingPreview->entity, Entity(7));
  EXPECT_TRUE(snapshot.waitingForFullRender);
  EXPECT_EQ(snapshot.settlingTargetVersion, 4u);
  EXPECT_FALSE(snapshot.waitingForChromeRefresh);
  EXPECT_FALSE(state.presentationPreview(std::nullopt).has_value());
}

TEST(CompositedPresentationTest, WaitingPhasesAreMutuallyExclusive) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(5.0, 0.0),
      },
      /*targetVersion=*/4);

  auto snapshot = Snapshot(state);
  EXPECT_EQ(snapshot.phase, Phase::SettlingForRender);
  EXPECT_TRUE(snapshot.waitingForFullRender);
  EXPECT_FALSE(snapshot.waitingForChromeRefresh);

  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100));
  snapshot = Snapshot(state);
  EXPECT_EQ(snapshot.phase, Phase::WaitingForChromeRefresh);
  EXPECT_FALSE(snapshot.waitingForFullRender);
  EXPECT_TRUE(snapshot.waitingForChromeRefresh);

  state.noteChromeRefreshCompleted(/*refreshedVersion=*/4);
  snapshot = Snapshot(state);
  EXPECT_EQ(snapshot.phase, Phase::Cached);
  EXPECT_FALSE(snapshot.waitingForFullRender);
  EXPECT_FALSE(snapshot.waitingForChromeRefresh);
}

TEST(CompositedPresentationTest, PureTranslationActiveDragWithMatchingCacheSuppressesCapture) {
  CompositedPresentation state;
  const SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .translation = Vector2d(4.0, 0.0),
      .documentFromCachedDocument = Transform2d::Translate(Vector2d(4.0, 0.0)),
  };

  EXPECT_TRUE(state.needsCompositedLayerCapture(active, /*currentVersion=*/3, Vector2i(100, 100)));

  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_FALSE(state.needsCompositedLayerCapture(active, /*currentVersion=*/3, Vector2i(100, 100)));
  EXPECT_FALSE(state.needsCompositedLayerCapture(active, /*currentVersion=*/4, Vector2i(100, 100)))
      << "DOM version changes during drag are presented as affine texture placement.";
  EXPECT_FALSE(state.needsCompositedLayerCapture(active, /*currentVersion=*/3, Vector2i(120, 100)))
      << "Canvas-size changes during active drag are presented from the existing cache; the crisp "
         "canvas refresh happens after drag settles.";
}

// A large affine scale drift past the threshold re-captures a crisp bitmap — the
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

// A small affine change below the scale threshold must NOT re-capture — the
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
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100));

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

TEST(CompositedPresentationTest, PureTranslationAfterAffineCaptureRequestsCrispReset) {
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

  EXPECT_TRUE(state.needsCompositedLayerCapture(active, /*currentVersion=*/5, Vector2i(100, 100)))
      << "Returning from affine resize/rotate to a pure translation should not keep transforming "
         "the last affine bitmap; capture a crisp translated layer again.";
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

TEST(CompositedPresentationTest, CachedTexturesDisplayZeroTranslationWithoutActiveDrag) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 0.0);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
}

TEST(CompositedPresentationTest, CachedTilesRemainVisibleAtIdle) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);

  SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .translation = Vector2d(4.0, 0.0),
  };
  ASSERT_TRUE(state.presentationPreview(active).has_value());
  EXPECT_EQ(state.presentationPreview(active)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(active)->translation.x, 0.0);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
}

TEST(CompositedPresentationTest, ActiveDragUsesCachedRepresentedTranslation) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .translation = Vector2d(4.0, 0.0),
  };
  ASSERT_TRUE(state.presentationPreview(active).has_value());
  EXPECT_EQ(state.presentationPreview(active)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(active)->translation.x, 0.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(active)->translation.y, 0.0);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
}

TEST(CompositedPresentationTest, ActiveDragKeepsGroupedSelectionPrewarmEntities) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100),
                           SelectTool::ActiveDragPreview{
                               .entity = Entity(7),
                               .extraEntities = {Entity(8)},
                           });

  SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .extraEntities = {Entity(8)},
      .translation = Vector2d(4.0, 0.0),
      .dragGeneration = 9,
  };
  const std::optional<SelectTool::ActiveDragPreview> displayed = state.presentationPreview(active);

  ASSERT_TRUE(displayed.has_value());
  EXPECT_EQ(displayed->entity, Entity(7));
  EXPECT_THAT(displayed->extraEntities, ElementsAre(Entity(8)))
      << "Selection-prewarmed grouped tiles must remain tied to the active drag group so the "
         "first live drag frames translate every unbundled component.";
  EXPECT_EQ(displayed->dragGeneration, active.dragGeneration);
  EXPECT_EQ(displayed->translation, Vector2d::Zero());
}

TEST(CompositedPresentationTest, ActiveDragUsesLandedRepresentedTranslation) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100),
                           SelectTool::ActiveDragPreview{
                               .entity = Entity(7),
                               .translation = Vector2d(3.0, 1.0),
                               .dragGeneration = 4,
                           });

  SelectTool::ActiveDragPreview active{
      .entity = Entity(7),
      .translation = Vector2d(5.0, 2.0),
      .dragGeneration = 4,
  };
  ASSERT_TRUE(state.presentationPreview(active).has_value());
  EXPECT_EQ(state.presentationPreview(active)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(active)->translation.x, 3.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(active)->translation.y, 1.0);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
}

TEST(CompositedPresentationTest, ActiveRedragIgnoresPreviousGestureRepresentedTranslation) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/4, Vector2i(100, 100),
                           SelectTool::ActiveDragPreview{
                               .entity = Entity(7),
                               .translation = Vector2d(-122.0, -86.0),
                               .dragGeneration = 4,
                           });

  SelectTool::ActiveDragPreview redrag{
      .entity = Entity(7),
      .translation = Vector2d::Zero(),
      .dragGeneration = 5,
  };
  const std::optional<SelectTool::ActiveDragPreview> displayed = state.presentationPreview(redrag);

  ASSERT_TRUE(displayed.has_value());
  EXPECT_EQ(displayed->entity, Entity(7));
  EXPECT_EQ(displayed->translation, Vector2d::Zero())
      << "A new same-entity drag must not subtract the previous gesture's settled offset.";
  EXPECT_EQ(displayed->dragGeneration, redrag.dragGeneration);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
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
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  EXPECT_EQ(Snapshot(state).cachedEntity, Entity(7));
}

TEST(CompositedPresentationTest, MouseUpKeepsSettlingPreviewUntilFullRenderLands) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  SelectTool::ActiveDragPreview preview{.entity = Entity(7), .translation = Vector2d(12.0, 5.0)};
  state.beginSettling(preview, /*targetVersion=*/4);

  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 12.0);

  // Below-target render leaves settlingPreview in place — the settling
  // window hasn't closed yet, so the settling preview still drives the
  // displayed translation.
  state.noteFullRenderLanded(/*landedVersion=*/3);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 12.0);

  // The target render closes the settling window, clears the preview,
  // and leaves cached textures available at zero display offset.
  state.noteFullRenderLanded(/*landedVersion=*/4);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0)
      << "Post-settle preview should fall to the zero-offset cached path.";
}

TEST(CompositedPresentationTest, MouseUpPreviewContinuesToDrivePresenterBaseline) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  SelectTool::ActiveDragPreview preview{
      .entity = Entity(7),
      .translation = Vector2d(12.0, 5.0),
      .dragGeneration = 9,
  };
  state.beginSettling(preview, /*targetVersion=*/4);

  const std::optional<SelectTool::ActiveDragPreview> presentationActivePreview =
      state.activePreviewForPresentation(std::nullopt);
  ASSERT_TRUE(presentationActivePreview.has_value());
  EXPECT_EQ(presentationActivePreview->entity, Entity(7));
  EXPECT_DOUBLE_EQ(presentationActivePreview->translation.x, 12.0);
  EXPECT_EQ(presentationActivePreview->dragGeneration, 9u);

  const std::optional<SelectTool::ActiveDragPreview> representedPreview =
      state.presentationPreview(presentationActivePreview);
  ASSERT_TRUE(representedPreview.has_value());
  EXPECT_EQ(representedPreview->entity, Entity(7));
  EXPECT_DOUBLE_EQ(representedPreview->translation.x, 0.0)
      << "The presenter receives active=settling and represented=cached, so the final mouse-up "
         "delta remains applied instead of popping back to the cached position.";
  EXPECT_EQ(representedPreview->dragGeneration, presentationActivePreview->dragGeneration);
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
  EXPECT_FALSE(Snapshot(state).waitingForFullRender);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
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
  EXPECT_TRUE(Snapshot(state).waitingForFullRender);
}

TEST(CompositedPresentationTest, FullRenderLandedDoesNotClearCachedTextures) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);

  state.noteFullRenderLanded(/*landedVersion=*/3);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  EXPECT_EQ(Snapshot(state).cachedEntity, Entity(7));
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
}

TEST(CompositedPresentationTest, DiscardCachedTexturesForEntityClearsMatchingCache) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  EXPECT_TRUE(state.discardCachedTexturesForEntity(Entity(7)));

  EXPECT_FALSE(Snapshot(state).hasCachedTextures);
  EXPECT_FALSE(state.presentationPreview(std::nullopt).has_value());
}

TEST(CompositedPresentationTest, DiscardCachedTexturesForEntityKeepsUnrelatedCache) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  EXPECT_FALSE(state.discardCachedTexturesForEntity(Entity(9)));

  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  EXPECT_EQ(Snapshot(state).cachedEntity, Entity(7));
}

TEST(CompositedPresentationTest, DiscardCachedTexturesForEntityClearsSettlingState) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  state.beginSettling(
      SelectTool::ActiveDragPreview{
          .entity = Entity(7),
          .translation = Vector2d(3.0, 2.0),
      },
      /*targetVersion=*/4);

  EXPECT_TRUE(state.discardCachedTexturesForEntity(Entity(7)));

  const auto snapshot = Snapshot(state);
  EXPECT_FALSE(snapshot.hasCachedTextures);
  EXPECT_FALSE(snapshot.waitingForFullRender);
  EXPECT_FALSE(snapshot.waitingForChromeRefresh);
  EXPECT_FALSE(snapshot.settlingPreview.has_value());
}

// Selection-clear keeps the cached document image visible. It only removes
// selection/settling state; the next document render atomically replaces the tiles.
TEST(CompositedPresentationTest, SelectionClearKeepsCachedTexturesVisible) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  ASSERT_TRUE(Snapshot(state).hasCachedTextures);

  state.clearSettlingIfSelectionChanged(/*selectedEntity=*/entt::null,
                                        /*dragActive=*/false);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  EXPECT_EQ(Snapshot(state).cachedEntity, Entity(7));
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
  EXPECT_TRUE(state.shouldPrewarm(Entity(7), {}, /*currentVersion=*/4, Vector2i(100, 100),
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
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  EXPECT_FALSE(Snapshot(state).waitingForFullRender);
  EXPECT_TRUE(Snapshot(state).waitingForChromeRefresh);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 5.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 0.0);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);

  state.noteChromeRefreshCompleted(/*refreshedVersion=*/4);
  EXPECT_FALSE(Snapshot(state).waitingForChromeRefresh);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0);
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.y, 0.0);
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
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
  EXPECT_TRUE(Snapshot(state).waitingForChromeRefresh);

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
  EXPECT_EQ(Snapshot(state).cachedEntity, Entity(9));
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
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
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  EXPECT_FALSE(Snapshot(state).waitingForFullRender);

  state.clearSettlingIfSelectionChanged(Entity(9), /*dragActive=*/false);

  EXPECT_FALSE(Snapshot(state).settlingPreview.has_value());
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  EXPECT_EQ(Snapshot(state).cachedEntity, Entity(7));
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_EQ(state.presentationPreview(std::nullopt)->entity, Entity(7));
}

TEST(CompositedPresentationTest, ClearSettlingIfSelectionChangedKeepsTexturesWithoutSettle) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);

  state.clearSettlingIfSelectionChanged(Entity(9), /*dragActive=*/false);

  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  EXPECT_EQ(Snapshot(state).cachedEntity, Entity(7));

  EXPECT_TRUE(state.shouldPrewarm(Entity(9), {}, /*currentVersion=*/3, Vector2i(100, 100),
                                  /*dragActive=*/false));
}

TEST(CompositedPresentationTest, DeselectionKeepsCachedTexturesVisible) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_TRUE(Snapshot(state).hasCachedTextures);

  state.clearSettlingIfSelectionChanged(entt::null, /*dragActive=*/false);

  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
}

TEST(CompositedPresentationTest, ActiveDragPreventsTextureClearing) {
  CompositedPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));

  state.clearSettlingIfSelectionChanged(Entity(9), /*dragActive=*/true);

  EXPECT_TRUE(Snapshot(state).hasCachedTextures);
  EXPECT_EQ(Snapshot(state).cachedEntity, Entity(7));
}

}  // namespace
}  // namespace donner::editor
