#pragma once
/// @file

#include <cstdint>
#include <optional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/editor/SelectTool.h"

namespace donner::editor {

/// Tracks composited presentation state across selection, drag, and release.
struct CompositedPresentation {
  bool hasCachedTextures = false;
  Entity cachedEntity = entt::null;
  std::uint64_t cachedVersion = 0;
  Vector2i cachedCanvasSize = Vector2i::Zero();
  std::optional<SelectTool::ActiveDragPreview> settlingPreview;
  bool waitingForFullRender = false;
  std::uint64_t settlingTargetVersion = 0;
  bool waitingForChromeRefresh = false;
  std::uint64_t chromeRefreshTargetVersion = 0;

  /// Returns true when selection should trigger an async prewarm capture.
  [[nodiscard]] bool shouldPrewarm(Entity selectedEntity, std::uint64_t currentVersion,
                                   const Vector2i& currentCanvasSize, bool dragActive) const {
    if (selectedEntity == entt::null || dragActive || waitingForFullRender ||
        waitingForChromeRefresh) {
      return false;
    }

    return !hasCachedTextures || cachedEntity != selectedEntity ||
           cachedVersion != currentVersion || cachedCanvasSize != currentCanvasSize;
  }

  /// Mark cached composited textures as available for the given entity/version/canvas size.
  void noteCachedTextures(Entity entity, std::uint64_t version, const Vector2i& canvasSize) {
    hasCachedTextures = true;
    cachedEntity = entity;
    cachedVersion = version;
    cachedCanvasSize = canvasSize;

    if (waitingForFullRender && settlingPreview.has_value() && settlingPreview->entity == entity &&
        version >= settlingTargetVersion) {
      waitingForFullRender = false;
      settlingTargetVersion = 0;
      waitingForChromeRefresh = true;
      chromeRefreshTargetVersion = version;
    }
  }

  /// Finish the settle handoff once overlay chrome and cached AABBs have refreshed to match the
  /// settled document version. Only then is it safe to drop the old drag offset.
  void noteChromeRefreshCompleted(std::uint64_t refreshedVersion) {
    if (!waitingForChromeRefresh || refreshedVersion < chromeRefreshTargetVersion ||
        !settlingPreview.has_value()) {
      return;
    }

    settlingPreview = SelectTool::ActiveDragPreview{
        .entity = settlingPreview->entity,
        .translation = Vector2d::Zero(),
    };
    waitingForChromeRefresh = false;
    chromeRefreshTargetVersion = 0;
  }

  /// Begin the post-release settling phase, keeping the last composited presentation alive.
  void beginSettling(const std::optional<SelectTool::ActiveDragPreview>& preview,
                     std::uint64_t targetVersion) {
    settlingPreview = preview;
    waitingForFullRender = preview.has_value();
    settlingTargetVersion = preview.has_value() ? targetVersion : 0;
    waitingForChromeRefresh = false;
    chromeRefreshTargetVersion = 0;
  }

  /// End the settling phase once a fresh full render has landed.
  ///
  /// This function only handles settling-state bookkeeping; cached tiles remain live until a fresh
  /// upload replaces them or the document is reset.
  void noteFullRenderLanded(std::uint64_t landedVersion) {
    if (waitingForFullRender && landedVersion < settlingTargetVersion) {
      return;
    }

    settlingPreview.reset();
    waitingForFullRender = false;
    settlingTargetVersion = 0;
    waitingForChromeRefresh = false;
    chromeRefreshTargetVersion = 0;
  }

  /// Returns the drag preview that should currently be displayed, if any.
  [[nodiscard]] std::optional<SelectTool::ActiveDragPreview> presentationPreview(
      const std::optional<SelectTool::ActiveDragPreview>& activePreview) const {
    // Prefer an active preview whose entity matches our cached textures. If
    // the active drag has moved to a different entity while a new render is
    // in flight, keep displaying the last cached document image without
    // applying the new entity's live drag offset to stale drag-target tiles.
    if (activePreview.has_value() && hasCachedTextures && activePreview->entity == cachedEntity) {
      return activePreview;
    }
    if (settlingPreview.has_value() && hasCachedTextures &&
        settlingPreview->entity == cachedEntity) {
      return settlingPreview;
    }
    if (hasCachedTextures && cachedEntity != entt::null) {
      return SelectTool::ActiveDragPreview{
          .entity = cachedEntity,
          .translation = Vector2d::Zero(),
      };
    }
    return std::nullopt;
  }

  /// Drop stale settling state when the selected entity changes away from the drag target.
  ///
  /// After ReplaceDocument, entity handles are invalidated and the selection is remapped to new
  /// entities. This detects the mismatch and clears the settling preview. Cached composited
  /// textures are deliberately kept alive as the visible document image until the next render
  /// atomically replaces them via noteCachedTextures().
  void clearSettlingIfSelectionChanged(Entity selectedEntity, bool dragActive) {
    if (waitingForFullRender || waitingForChromeRefresh) {
      return;
    }

    if (!dragActive && settlingPreview.has_value() && settlingPreview->entity != selectedEntity) {
      settlingPreview.reset();
      waitingForFullRender = false;
      settlingTargetVersion = 0;
    }
  }
};

}  // namespace donner::editor
