#pragma once
/// @file
///
/// Read-only diagnostic panel that exposes the live compositor's
/// per-layer state (size, dirty flag, rasterize wall-clock, fallback
/// reasons, downsampled thumbnail) plus the global fast-path counters.
/// Lands as Milestone 1 of design doc 0033 — the observability surface
/// every subsequent responsiveness milestone needs to know what's
/// actually happening.

#include <cstdint>
#include <span>
#include <unordered_map>

#ifdef __EMSCRIPTEN__
#define GLFW_INCLUDE_ES3
#include <GLES3/gl3.h>
#else
#include "glad/glad.h"
#endif

#include "donner/base/EcsRegistry.h"
#include "donner/svg/compositor/CompositorController.h"

namespace donner::editor {

/// Stateful — owns one GL texture per active compositor layer for the
/// thumbnail column, and frees textures when their layer leaves the
/// snapshot. Construct on the GL thread (textures are lazily created in
/// `render`); destroy on the GL thread (the dtor calls `glDeleteTextures`).
class LayerInspectorPanel {
public:
  LayerInspectorPanel() = default;
  ~LayerInspectorPanel();

  LayerInspectorPanel(const LayerInspectorPanel&) = delete;
  LayerInspectorPanel& operator=(const LayerInspectorPanel&) = delete;
  LayerInspectorPanel(LayerInspectorPanel&&) = delete;
  LayerInspectorPanel& operator=(LayerInspectorPanel&&) = delete;

  /// Render the panel into the current ImGui window. Must be called
  /// inside a `ImGui::Begin(...) / End()` pair AND on the GL thread —
  /// thumbnails are uploaded via `glTexImage2D` on first sight of a new
  /// `(entity, generation)` pair.
  ///
  /// @param layerRows Latest snapshot from `AsyncRenderer::compositorLayerInspectorRows`.
  ///   Empty span renders a "no compositor layers" placeholder.
  /// @param segmentRows Latest snapshot from `AsyncRenderer::compositorSegmentInspectorRows`.
  ///   Rendered as a secondary table below the layer table.
  /// @param fastPath Latest fast-path counters from `AsyncRenderer::
  ///   compositorFastPathCountersForTesting`. Rendered as a summary line.
  void render(
      std::span<const svg::compositor::CompositorController::LayerInspectorRow> layerRows,
      std::span<const svg::compositor::CompositorController::SegmentInspectorRow> segmentRows,
      const svg::compositor::CompositorController::FastPathCounters& fastPath);

private:
  struct ThumbnailTexture {
    GLuint texture = 0;
    std::uint64_t uploadedGeneration = 0;
    int width = 0;
    int height = 0;
  };

  /// Upload (or refresh) the thumbnail texture for a single row. Returns
  /// the GL texture name, or 0 if the row has no valid thumbnail.
  GLuint uploadThumbnail(const svg::compositor::CompositorController::LayerInspectorRow& row);

  /// Free textures for entities that aren't in the current snapshot —
  /// keeps the cache size bounded as the user demotes/promotes layers.
  void evictAbsentEntities(
      std::span<const svg::compositor::CompositorController::LayerInspectorRow> rows);

  std::unordered_map<Entity, ThumbnailTexture> textures_;
};

}  // namespace donner::editor
