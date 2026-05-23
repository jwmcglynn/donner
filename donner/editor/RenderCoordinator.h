#pragma once
/// @file

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/CompositedPresentation.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/PresentationRenderScheduler.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/ViewportInteractionController.h"

namespace donner::geode {
class GeodeDevice;
}

namespace donner::editor {

class SelectTool;

/**
 * Return true when a composited preview may be presented against the current viewport.
 *
 * Full-canvas previews may stretch across transient canvas-size changes. Split composited previews
 * carry per-tile raster bounds, so presenting them against a different canvas epoch can flash stale
 * high-resolution tiles in the wrong place during zoom/drag races.
 *
 * @param preview Worker-produced composited preview.
 * @param viewportDesiredCanvas Canvas size implied by the current UI-frame viewport.
 */
[[nodiscard]] bool ShouldPresentCompositedPreviewForViewport(
    const RenderResult::CompositedPreview& preview, const Vector2i& viewportDesiredCanvas);

/**
 * Return true when an active-drag overlay may bypass the displayed-version gate.
 *
 * Immediate overlay uploads keep drag chrome responsive, but they must not mix a
 * current-canvas overlay with stale split-tile content from a previous zoom epoch.
 * Full-canvas content is allowed to stretch across zoom/canvas changes; split
 * content must already match the current document canvas.
 *
 * @param tiles Currently presented content tiles.
 * @param currentCanvasSize Canvas size used to rasterize the pending overlay.
 */
[[nodiscard]] bool ShouldUploadImmediateOverlayForPresentedTiles(
    std::span<const GlTextureCache::TileView> tiles, const Vector2i& currentCanvasSize);

/// Owns the advanced editor's renderer-side orchestration: async rendering, overlay rasterization,
/// composited drag presentation, and selection-bounds cache promotion.
class RenderCoordinator {
public:
  /// Overlay upload policy for the freshly rasterized editor chrome.
  enum class OverlayUploadMode {
    /// Hold the chrome until it matches the displayed async render version.
    MatchDisplayedVersion,
    /// Upload immediately from the current UI-frame DOM state.
    Immediate,
  };

  explicit RenderCoordinator(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice = nullptr);

  [[nodiscard]] AsyncRenderer& asyncRenderer() { return renderWorker_.asyncRenderer; }
  [[nodiscard]] const AsyncRenderer& asyncRenderer() const { return renderWorker_.asyncRenderer; }
  [[nodiscard]] svg::Renderer& renderer() { return renderWorker_.renderer; }
  [[nodiscard]] const SelectionBoundsCache& selectionBoundsCache() const {
    return selectionBoundsCache_;
  }
  [[nodiscard]] CompositedPresentation& compositedPresentation() { return compositedPresentation_; }
  [[nodiscard]] const CompositedPresentation& compositedPresentation() const {
    return compositedPresentation_;
  }
  /// Drag preview state represented by the currently uploaded overlay texture.
  [[nodiscard]] const std::optional<SelectTool::ActiveDragPreview>& presentedOverlayDragPreview()
      const {
    return presentedOverlayDragPreview_;
  }
  [[nodiscard]] std::uint64_t displayedDocVersion() const { return displayedDocVersion_; }
  /// Return true when freshly rasterized overlay chrome is waiting for a
  /// matching async-render result before upload.
  [[nodiscard]] bool hasPendingOverlayForTesting() const { return pendingOverlayVersion_ != 0; }

  void resetForLoadedDocument();
  void refreshSelectionBoundsCache(EditorApp& app);
  void promoteSelectionBoundsIfReady();
  /// Rasterize the editor chrome (path outlines, selection AABBs,
  /// marquee) into the overlay texture. `marqueeRectDoc` is the active
  /// marquee rectangle in document space (nullopt when the user isn't
  /// marquee-dragging). All chrome is baked into this single overlay
  /// texture — Geode will later own the whole layer end-to-end.
  bool rasterizeOverlayForCurrentSelection(
      EditorApp& app, const ViewportState& viewport, GlTextureCache& textures,
      const std::optional<Box2d>& marqueeRectDoc,
      OverlayUploadMode uploadMode = OverlayUploadMode::MatchDisplayedVersion,
      std::optional<SelectTool::ActiveDragPreview> representedDragPreview = std::nullopt,
      std::optional<SelectTool::ActiveTransformBoundsPreview> activeBoundsPreview = std::nullopt);
  /// Drain the latest async-render result into the editor's UI state.
  /// If a `frameHistory` is supplied, its latest slot is stamped with
  /// the backend (worker) ms reported by `AsyncRenderer` so the frame
  /// graph can plot backend time alongside ImGui frame time. `nullptr`
  /// is allowed for callers that don't care about backend timing.
  void pollRenderResult(EditorApp& app, const ViewportState& viewport, GlTextureCache& textures,
                        FrameHistory* frameHistory = nullptr);
  void maybeRequestRender(EditorApp& app, SelectTool& selectTool, const ViewportState& viewport,
                          GlTextureCache& textures);

private:
  [[nodiscard]] Entity selectedCompositedEntity(EditorApp& app) const;

  struct RenderWorkerBundle {
    explicit RenderWorkerBundle(
        std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice = nullptr);

    // Members are destroyed in reverse declaration order. The async worker
    // joins before the renderer it references is destroyed.
    svg::Renderer renderer;
    AsyncRenderer asyncRenderer;
  };

  RenderWorkerBundle renderWorker_;
  svg::Renderer overlayRenderer_;
  CompositedPresentation compositedPresentation_;
  SelectionBoundsCache selectionBoundsCache_;

  std::optional<svg::RendererBitmap> pendingOverlayBitmap_;
  std::shared_ptr<const svg::RendererTextureSnapshot> pendingOverlayTexture_;
  std::optional<SelectTool::ActiveDragPreview> pendingOverlayDragPreview_;
  std::optional<SelectTool::ActiveDragPreview> presentedOverlayDragPreview_;
  std::uint64_t pendingOverlayVersion_ = 0;
  std::uint64_t displayedDocVersion_ = 0;

  std::vector<svg::SVGElement> lastOverlaySelectionVec_;
  Vector2i lastOverlayCanvasSize_ = Vector2i::Zero();
  std::uint64_t lastOverlayVersion_ = std::numeric_limits<std::uint64_t>::max();
  /// Last marquee rect baked into the overlay texture, or nullopt if
  /// the last overlay rasterize didn't include one. Used to invalidate
  /// the cached overlay when the marquee geometry changes.
  std::optional<Box2d> lastOverlayMarqueeRectDoc_;
  /// Active rotation bounds baked into the overlay texture, or nullopt
  /// when the last overlay used normal axis-aligned selection bounds.
  std::optional<SelectTool::ActiveTransformBoundsPreview> lastOverlayActiveBoundsPreview_;

  PresentationRenderScheduler renderScheduler_;
  /// Most recent desired canvas size requested by `maybeRequestRender`.
  /// Used to debounce continuous pinch-zoom before committing through
  /// `SVGDocument::setCanvasSize`.
  Vector2i pendingCanvasSize_ = Vector2i::Zero();
  std::chrono::steady_clock::time_point pendingCanvasSizeSince_{};
};

}  // namespace donner::editor
