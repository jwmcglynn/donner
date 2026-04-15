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

    if (waitingForFullRender && settlingPreview.has_value() && settlingPreview->entity == entity &&
        version >= settlingTargetVersion) {
      settlingPreview = SelectTool::ActiveDragPreview{
          .entity = entity,
          .translation = Vector2d::Zero(),
      };
      waitingForFullRender = false;
      settlingTargetVersion = 0;
    }
  }

  /// Begin the post-release settling phase, keeping the last composited presentation alive.
  void beginSettling(const std::optional<SelectTool::ActiveDragPreview>& preview,
                     std::uint64_t targetVersion) {
    settlingPreview = preview;
    waitingForFullRender = preview.has_value();
    settlingTargetVersion = preview.has_value() ? targetVersion : 0;
  }

  /// End settling once a fresh full render has landed.  Also clears cached texture state so the
  /// display falls back to the just-uploaded flat texture instead of showing stale composited layers.
  void noteFullRenderLanded(std::uint64_t landedVersion) {
    if (waitingForFullRender && landedVersion < settlingTargetVersion) {
      return;
    }

    settlingPreview.reset();
    waitingForFullRender = false;
    settlingTargetVersion = 0;
    hasCachedTextures = false;
    cachedEntity = entt::null;
  }

  /// Returns the drag preview that should currently be displayed, if any.
  [[nodiscard]] std::optional<SelectTool::ActiveDragPreview> presentationPreview(
      const std::optional<SelectTool::ActiveDragPreview>& activePreview) const {
    if (activePreview.has_value()) {
      return activePreview;
    }
    if (settlingPreview.has_value()) {
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

  /// Returns true if the UI should draw the composited drag presentation right now.
  [[nodiscard]] bool shouldDisplayCompositedLayers(
      const std::optional<SelectTool::ActiveDragPreview>& activePreview) const {
    const auto preview = presentationPreview(activePreview);
    return hasCachedTextures && preview.has_value() && preview->entity == cachedEntity;
  }

  /// Drop stale settling state when the selected entity changes away from the drag target.
  ///
  /// After ReplaceDocument, entity handles are invalidated and the selection is remapped to new
  /// entities.  This detects the mismatch and clears the settling preview.  However, the cached
  /// composited textures are deliberately kept alive: at zero composition offset they are visually
  /// identical to the flat texture, so keeping them avoids a visible pop during the
  /// settling → prewarm transition.  The prewarm render for the new entity will atomically update
  /// the textures via noteCachedTextures().
  ///
  /// Cached textures are only cleared on explicit deselection (selectedEntity == entt::null).
  void clearSettlingIfSelectionChanged(Entity selectedEntity, bool dragActive) {
    if (waitingForFullRender) {
      return;
    }

    if (!dragActive && settlingPreview.has_value() && settlingPreview->entity != selectedEntity) {
      settlingPreview.reset();
      waitingForFullRender = false;
      settlingTargetVersion = 0;
    }

    // Only clear cached textures on explicit deselection.  When the entity handle changes
    // (e.g., after ReplaceDocument) but an element is still selected, the composited textures
    // remain valid at zero offset and shouldPrewarm() will dispatch a prewarm render for the
    // new entity.  Clearing here would cause a one-frame pop to the flat texture.
    if (!dragActive && hasCachedTextures && selectedEntity == entt::null) {
      hasCachedTextures = false;
      cachedEntity = entt::null;
    }
  }
};

}  // namespace donner::editor
