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

  // Below-target render leaves settlingPreview in place — the settling
  // window hasn't closed yet, so the settling preview still drives the
  // displayed translation.
  state.noteFullRenderLanded(/*landedVersion=*/3);
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 12.0);

  // Target render lands — settling window closes; the settling
  // preview is cleared and `presentationPreview` falls back to the
  // cached-entity path (translation=0). The cached textures
  // themselves survive: `noteFullRenderLanded` no longer clears them,
  // since clearing on a stray flat-only render mid-drag was the
  // observed "snap back to start position" regression (see fix notes
  // on `noteFullRenderLanded`).
  state.noteFullRenderLanded(/*landedVersion=*/4);
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_TRUE(state.presentationPreview(std::nullopt)->entity == Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0)
      << "Post-settle preview should fall to the zero-offset cached path.";
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
  // Settling state itself is cleared (settling preview, waiting flag).
  EXPECT_FALSE(state.waitingForFullRender);
  // The cached textures stay alive — covers the M2C drag-target-swap
  // window where the editor keeps blitting the old entity's tiles
  // (at zero offset, drawn against the old entity's current DOM
  // transform) until the new entity's render lands. Pre-fix, the
  // cache was cleared here and the editor fell back to the stale
  // flat texture for one frame. See `presentationPreview`'s
  // "Drag-target swap" branch.
  EXPECT_TRUE(state.hasCachedTextures);
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_TRUE(state.presentationPreview(std::nullopt)->entity == Entity(7));
  EXPECT_DOUBLE_EQ(state.presentationPreview(std::nullopt)->translation.x, 0.0)
      << "Post-settling cached path drops the settling translation.";
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

// Pre-fix `noteFullRenderLanded` cleared `hasCachedTextures`. In
// production `beginSettling` is never called, so the only effect
// was clearing the cache on every non-composited render — which
// fired during transient drag-target-switch windows where the
// worker's `previewTiles` happened to be empty, snapping the
// editor's display to the pre-drag flat texture until the next
// composited render landed. The fix: `noteFullRenderLanded` only
// touches the settling-state fields; the cache lifecycle is driven
// by `noteCachedTextures` (refresh) and `clearSettlingIfSelectionChanged`
// (deselect).
TEST(ExperimentalDragPresentationTest, FullRenderLandedDoesNotClearCachedTextures) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));

  state.noteFullRenderLanded(/*landedVersion=*/3);
  EXPECT_TRUE(state.hasCachedTextures);
  EXPECT_TRUE(state.cachedEntity == Entity(7));
  EXPECT_TRUE(state.shouldDisplayCompositedLayers(std::nullopt));
  ASSERT_TRUE(state.presentationPreview(std::nullopt).has_value());
  EXPECT_TRUE(state.presentationPreview(std::nullopt)->entity == Entity(7));
}

// Selection-clear is the ONE production path that drops the cached
// textures; covers the "user deselected" UX where the editor should
// switch back to the flat texture.
TEST(ExperimentalDragPresentationTest, SelectionClearDropsCachedTextures) {
  ExperimentalDragPresentation state;
  state.noteCachedTextures(Entity(7), /*version=*/3, Vector2i(100, 100));
  ASSERT_TRUE(state.hasCachedTextures);

  state.clearSettlingIfSelectionChanged(/*selectedEntity=*/entt::null,
                                        /*dragActive=*/false);
  EXPECT_FALSE(state.hasCachedTextures);
  EXPECT_TRUE(state.cachedEntity == entt::null);
  EXPECT_FALSE(state.shouldDisplayCompositedLayers(std::nullopt));
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

  // `noteFullRenderLanded` no longer clears `hasCachedTextures` —
  // `shouldPrewarm` is driven by the version mismatch instead
  // (cachedVersion=3 vs currentVersion=4 after settling). That's the
  // production-path signal anyway: a fresh render bumps the version,
  // the cache lags, prewarm catches up.
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

// Regression for the prewarm-dispatch loop. When the selected entity
// has a compositing-breaking ancestor, `promoteEntity` refuses, the
// render produces no composited preview, `noteCachedTextures` never
// runs, and `hasCachedTextures` stays false forever. The old
// `shouldPrewarm` would therefore re-dispatch every frame, keeping
// the worker continuously busy and blocking the editor's click
// handler (which is gated on `!isBusy()`) from ever processing a
// new selection. `notePrewarmAttempted` closes the loop by remembering
// the `(entity, version, canvasSize)` combination the editor tried,
// regardless of whether the render produced a composited preview.
TEST(ExperimentalDragPresentationTest, NotePrewarmAttemptedSuppressesRedispatchEvenWithoutCache) {
  ExperimentalDragPresentation state;
  // Initial state: no cache, prewarm should fire.
  ASSERT_TRUE(state.shouldPrewarm(Entity(7), /*currentVersion=*/3, Vector2i(100, 100),
                                  /*dragActive=*/false));

  // Worker rendered but produced no compositedPreview (entity refused
  // by `HasCompositingBreakingAncestor`). Editor records the attempt
  // even though `hasCachedTextures` stays false.
  state.notePrewarmAttempted(Entity(7), /*version=*/3, Vector2i(100, 100));
  EXPECT_FALSE(state.hasCachedTextures);

  // Critical: subsequent `shouldPrewarm` for the same combination must
  // return false. Without this guard, the dispatch loop runs forever.
  EXPECT_FALSE(state.shouldPrewarm(Entity(7), /*currentVersion=*/3, Vector2i(100, 100),
                                   /*dragActive=*/false));
}

// A change in any of the three keys (entity, version, canvasSize) must
// unblock the next prewarm — otherwise the editor wouldn't retry after
// the user selects something different, makes an edit, or zooms.
TEST(ExperimentalDragPresentationTest, NotePrewarmAttemptedAllowsRetryAfterStateChange) {
  ExperimentalDragPresentation state;
  state.notePrewarmAttempted(Entity(7), /*version=*/3, Vector2i(100, 100));
  ASSERT_FALSE(state.shouldPrewarm(Entity(7), /*currentVersion=*/3, Vector2i(100, 100),
                                   /*dragActive=*/false));

  // Different entity → retry.
  EXPECT_TRUE(state.shouldPrewarm(Entity(8), /*currentVersion=*/3, Vector2i(100, 100),
                                  /*dragActive=*/false));
  // Bumped version → retry.
  EXPECT_TRUE(state.shouldPrewarm(Entity(7), /*currentVersion=*/4, Vector2i(100, 100),
                                  /*dragActive=*/false));
  // Canvas resized → retry.
  EXPECT_TRUE(state.shouldPrewarm(Entity(7), /*currentVersion=*/3, Vector2i(200, 100),
                                  /*dragActive=*/false));
}

}  // namespace
}  // namespace donner::editor
