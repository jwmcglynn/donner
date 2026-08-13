#pragma once
/// @file

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/CompositedPresentation.h"
#include "donner/editor/SelectTool.h"

namespace donner::editor {

/// Inputs for one editor-style render scheduling decision.
struct PresentationRenderScheduleInput {
  /// Current selected graphics entity, or entt::null when no compositable selection exists.
  Entity selectedEntity = entt::null;
  /// Additional selected graphics entities that should be prewarmed with \ref selectedEntity.
  std::vector<Entity> selectedExtraEntities;
  /// Current active drag preview, if a drag is in progress.
  std::optional<SelectTool::ActiveDragPreview> activeDragPreview;
  /// True when renderer presentation state changed independently of document and viewport state.
  bool forcePresentationRefresh = false;
  /// True when the selected layer's cached pixels are known stale.
  bool forceSelectedLayerRasterization = false;
  /// Current document frame version.
  std::uint64_t currentVersion = 0;
  /// Current output raster canvas size in pixels.
  Vector2i currentCanvasSize = Vector2i::Zero();
  /// Current raster viewport derived from the editor camera.
  EditorRasterViewport currentRasterViewport;
  /// Translation drift allowed before refreshing a cached drag tile.
  double dragTranslationRecaptureDistanceDoc = std::numeric_limits<double>::infinity();
  /// True when the presentation backend cannot apply a live drag transform to cached pixels and
  /// therefore needs a render request for each newly represented drag transform.
  bool requiresRenderedActiveDragPresentation = false;
  /// True when an identity drag started by mouse-down should wait for actual pointer movement
  /// before capturing its layer. The unchanged document remains presentable in the meantime.
  bool deferIdentityActiveDragCapture = false;
  /// True when an otherwise-current frame may be rendered solely to warm selected-layer textures.
  /// Cached-texture presenters use this to make the first drag frame immediate. Direct worker
  /// surfaces cannot consume that cache on the UI thread, so they disable selection-only renders.
  bool selectionOnlyPrewarmMayTriggerRender = true;
};

/// Result of one editor-style render scheduling decision.
struct PresentationRenderScheduleDecision {
  /// True when the caller should post a render request.
  [[nodiscard]] bool shouldRequestRender() const {
    return needsCompositedLayerCapture || needsCompositedPrewarm ||
           needsRenderedActiveDragPresentation || needsRegularRender;
  }

  /// A fresh drag-target layer is needed for active presentation.
  bool needsCompositedLayerCapture = false;
  /// A selected layer should be warmed or refreshed for presentation.
  bool needsCompositedPrewarm = false;
  /// The backend must render a changed active-drag transform even though cached pixels are valid.
  bool needsRenderedActiveDragPresentation = false;
  /// Document pixels changed and require a regular render.
  bool needsRegularRender = false;
  /// Compositor interaction hint to attach to the render request, if any.
  std::optional<RenderRequest::DragPreview> dragPreview;
  /// Version evaluated by the scheduler.
  std::uint64_t currentVersion = 0;
  /// Output raster canvas size evaluated by the scheduler.
  Vector2i currentCanvasSize = Vector2i::Zero();
  /// Raster viewport evaluated by the scheduler.
  EditorRasterViewport currentRasterViewport;
};

/// Shared editor/MCP scheduler for deciding when to post presentation renders.
class PresentationRenderScheduler {
public:
  /// Reset tracked render state after document load/replacement.
  void reset();

  /**
   * Evaluate whether a render should be posted.
   *
   * @param presentation Mutable presentation state to keep in sync with selection/drag changes.
   * @param input Current editor state snapshot.
   * @return Scheduling decision for the caller's next render request.
   */
  [[nodiscard]] PresentationRenderScheduleDecision evaluate(
      CompositedPresentation& presentation, const PresentationRenderScheduleInput& input) const;

  /**
   * Record that a final render result landed.
   *
   * @param completedVersion Document frame version in the completed result.
   * @param completedCanvasSize Output raster canvas size used by the completed result.
   * @param completedRasterViewport Raster viewport used by the completed result.
   */
  void noteRenderCompleted(std::uint64_t completedVersion, const Vector2i& completedCanvasSize,
                           const EditorRasterViewport& completedRasterViewport);

private:
  std::uint64_t lastRenderedVersion_ = 0;
  Vector2i lastRenderedCanvasSize_ = Vector2i::Zero();
  std::optional<EditorRasterViewport> lastRenderedRasterViewport_;
};

}  // namespace donner::editor
