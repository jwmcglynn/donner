#pragma once
/// @file

#include <cstdint>
#include <optional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/editor/SelectTool.h"

namespace donner::editor {

/// Tracks experimental drag-compositing presentation state across selection, drag, and release.
struct ExperimentalDragPresentation {
  bool hasCachedTextures = false;
  Entity cachedEntity = entt::null;
  std::uint64_t cachedVersion = 0;
  Vector2i cachedCanvasSize = Vector2i::Zero();
  std::optional<SelectTool::ActiveDragPreview> settlingPreview;
  bool waitingForFullRender = false;
  std::uint64_t settlingTargetVersion = 0;

  /// Returns true when selection should trigger an async prewarm capture.
  [[nodiscard]] bool shouldPrewarm(Entity selectedEntity, std::uint64_t currentVersion,
                                   const Vector2i& currentCanvasSize, bool dragActive) const {
    if (selectedEntity == entt::null || dragActive || waitingForFullRender) {
      return false;
    }

    return !hasCachedTextures || cachedEntity != selectedEntity || cachedVersion != currentVersion ||
           cachedCanvasSize != currentCanvasSize;
  }

  /// Mark cached composited textures as available for the given entity/version/canvas size.
  void noteCachedTextures(Entity entity, std::uint64_t version, const Vector2i& canvasSize) {
    hasCachedTextures = true;
    cachedEntity = entity;
    cachedVersion = version;
    cachedCanvasSize = canvasSize;
  }

  /// Begin the post-release settling phase, keeping the last composited presentation alive.
  void beginSettling(const std::optional<SelectTool::ActiveDragPreview>& preview,
                     std::uint64_t targetVersion) {
    settlingPreview = preview;
    waitingForFullRender = preview.has_value();
    settlingTargetVersion = preview.has_value() ? targetVersion : 0;
  }

  /// End settling once a fresh full render has landed.
  void noteFullRenderLanded(std::uint64_t landedVersion) {
    if (waitingForFullRender && landedVersion < settlingTargetVersion) {
      return;
    }

    settlingPreview.reset();
    waitingForFullRender = false;
    settlingTargetVersion = 0;
  }

  /// Returns the drag preview that should currently be displayed, if any.
  [[nodiscard]] std::optional<SelectTool::ActiveDragPreview> presentationPreview(
      const std::optional<SelectTool::ActiveDragPreview>& activePreview) const {
    if (activePreview.has_value()) {
      return activePreview;
    }
    return settlingPreview;
  }

  /// Returns true if the UI should draw the composited drag presentation right now.
  [[nodiscard]] bool shouldDisplayCompositedLayers(
      const std::optional<SelectTool::ActiveDragPreview>& activePreview) const {
    const auto preview = presentationPreview(activePreview);
    return hasCachedTextures && preview.has_value() && preview->entity == cachedEntity;
  }

  /// Drop stale settling state when the selected entity changes away from the drag target.
  void clearSettlingIfSelectionChanged(Entity selectedEntity, bool dragActive) {
    if (!dragActive && settlingPreview.has_value() && settlingPreview->entity != selectedEntity) {
      settlingPreview.reset();
      waitingForFullRender = false;
      settlingTargetVersion = 0;
    }
  }
};

}  // namespace donner::editor
