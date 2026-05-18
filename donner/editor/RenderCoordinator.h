#pragma once
/// @file

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/CompositedPresentation.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/SelectionAabb.h"
#include "donner/editor/ViewportInteractionController.h"

namespace donner::editor {

class SelectTool;

/// Owns the advanced editor's renderer-side orchestration: async rendering, overlay rasterization,
/// composited drag presentation, and selection-bounds cache promotion.
class RenderCoordinator {
public:
  RenderCoordinator() = default;

  [[nodiscard]] AsyncRenderer& asyncRenderer() { return asyncRenderer_; }
  [[nodiscard]] const AsyncRenderer& asyncRenderer() const { return asyncRenderer_; }
  [[nodiscard]] svg::Renderer& renderer() { return renderer_; }
  [[nodiscard]] const SelectionBoundsCache& selectionBoundsCache() const {
    return selectionBoundsCache_;
  }
  [[nodiscard]] CompositedPresentation& compositedPresentation() { return compositedPresentation_; }
  [[nodiscard]] const CompositedPresentation& compositedPresentation() const {
    return compositedPresentation_;
  }
  [[nodiscard]] std::uint64_t displayedDocVersion() const { return displayedDocVersion_; }

  void resetForLoadedDocument();
  void refreshSelectionBoundsCache(EditorApp& app);
  void promoteSelectionBoundsIfReady();
  /// Rasterize the editor chrome (path outlines, selection AABBs,
  /// marquee) into the overlay texture. `marqueeRectDoc` is the active
  /// marquee rectangle in document space (nullopt when the user isn't
  /// marquee-dragging). All chrome is baked into this single overlay
  /// texture — Geode will later own the whole layer end-to-end.
  bool rasterizeOverlayForCurrentSelection(EditorApp& app, const ViewportState& viewport,
                                           GlTextureCache& textures,
                                           const std::optional<Box2d>& marqueeRectDoc);
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

  // `asyncRenderer_` is declared last so it is destroyed first. Its destructor
  // joins the worker thread while `renderer_`, which the compositor references
  // during active renders, is still alive.
  svg::Renderer renderer_;
  svg::Renderer overlayRenderer_;
  CompositedPresentation compositedPresentation_;
  SelectionBoundsCache selectionBoundsCache_;

  std::optional<svg::RendererBitmap> pendingOverlayBitmap_;
  std::uint64_t pendingOverlayVersion_ = 0;
  std::uint64_t displayedDocVersion_ = 0;

  std::vector<svg::SVGElement> lastOverlaySelectionVec_;
  Vector2i lastOverlayCanvasSize_ = Vector2i::Zero();
  std::uint64_t lastOverlayVersion_ = std::numeric_limits<std::uint64_t>::max();
  /// Last marquee rect baked into the overlay texture, or nullopt if
  /// the last overlay rasterize didn't include one. Used to invalidate
  /// the cached overlay when the marquee geometry changes.
  std::optional<Box2d> lastOverlayMarqueeRectDoc_;

  std::uint64_t lastRenderedVersion_ = 0;
  Vector2i lastRenderedCanvasSize_ = Vector2i::Zero();
  /// Most recent desired canvas size requested by `maybeRequestRender`.
  /// Used to debounce continuous pinch-zoom before committing through
  /// `SVGDocument::setCanvasSize`.
  Vector2i pendingCanvasSize_ = Vector2i::Zero();
  std::chrono::steady_clock::time_point pendingCanvasSizeSince_{};

  // Declared last so it is destroyed first; see the `renderer_` comment.
  AsyncRenderer asyncRenderer_;
};

}  // namespace donner::editor
