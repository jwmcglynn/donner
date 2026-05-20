#pragma once
/// @file
///
/// Read-only diagnostic panel that exposes the live compositor's
/// composite state — every tile (background, foreground, segments,
/// layers) the renderer blits to produce the final frame, displayed
/// in paint order with a thumbnail per tile.
///
/// Design doc 0033 §M1++. The "comprehensive composite" view replaces
/// the earlier separate per-layer / per-segment / split-bitmap tables.

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/editor/ImGuiIncludes.h"

#ifdef __EMSCRIPTEN__
#define GLFW_INCLUDE_ES3
#include <GLES3/gl3.h>
#elif !defined(DONNER_EDITOR_WGPU)
#include "glad/glad.h"
#endif

#include "donner/svg/compositor/CompositorController.h"

namespace donner::editor {

/// Stateful — owns one GL texture per active composite tile (keyed on
/// `CompositeTileSnapshot::id`), refreshes via `glTexImage2D` only when
/// the tile's `generation` advances, and frees textures when their
/// tile leaves the snapshot. Construct on the GL thread; destroy on
/// the GL thread (the dtor calls `glDeleteTextures`).
class LayerInspectorPanel {
public:
  LayerInspectorPanel() = default;
  ~LayerInspectorPanel();

  LayerInspectorPanel(const LayerInspectorPanel&) = delete;
  LayerInspectorPanel& operator=(const LayerInspectorPanel&) = delete;
  LayerInspectorPanel(LayerInspectorPanel&&) = delete;
  LayerInspectorPanel& operator=(LayerInspectorPanel&&) = delete;

  /// Render the panel into the current ImGui window. Must be called
  /// inside an `ImGui::Begin(...) / End()` pair AND on the GL thread.
  ///
  /// @param tiles Unified paint-order tile list from
  ///   `AsyncRenderer::compositorCompositeTiles`. Each entry produces
  ///   one row with an inline thumbnail.
  /// @param state Compositor-wide diagnostic state (active-hints
  ///   count, split-path active flag, drag-target entity, canvas
  ///   size). Rendered as a state header so the operator can spot
  ///   mismatches between expected and actual compositor state.
  /// @param workerCompositorEntity The worker's view of the
  ///   currently-promoted entity.
  /// @param viewportZoom Editor viewport's `zoom`.
  /// @param viewportDpr Editor viewport's `devicePixelRatio`.
  /// @param viewportDesiredCanvas What the viewport's
  ///   `desiredCanvasSize()` currently returns.
  /// @param documentCanvas What `SVGDocument::canvasSize()` currently
  ///   returns — the committed canvas size the worker rendered at.
  ///   Compare against `viewportDesiredCanvas` (commit pipeline) and
  ///   `state.canvasSize` (compositor rasterize freshness).
  /// @param fastPath Fast-path counters rendered as a summary line
  ///   above the table.
  void render(std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles,
              const svg::compositor::CompositorController::StateSnapshot& state,
              Entity workerCompositorEntity, double viewportZoom, double viewportDpr,
              const Vector2i& viewportDesiredCanvas, const Vector2i& documentCanvas,
              const svg::compositor::CompositorController::FastPathCounters& fastPath);

private:
#ifdef DONNER_EDITOR_WGPU
  using ThumbnailTextureHandle = ImTextureID;
#else
  using ThumbnailTextureHandle = GLuint;
#endif

  struct ThumbnailTexture {
    ThumbnailTextureHandle texture = 0;
    std::uint64_t uploadedGeneration = 0;
    int width = 0;
    int height = 0;
  };

  /// Upload (or refresh) the thumbnail texture for one tile. Returns
  /// the GL texture name (0 when the tile has no thumbnail).
  ThumbnailTextureHandle uploadThumbnail(
      const svg::compositor::CompositorController::CompositeTileSnapshot& tile);

  /// Free textures for tiles absent from the current snapshot.
  void evictAbsentTiles(
      std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles);

  std::unordered_map<std::string, ThumbnailTexture> textures_;
};

}  // namespace donner::editor
