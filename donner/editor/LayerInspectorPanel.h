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

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/editor/GlTextureCache.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/LayerInspectorDiagnostics.h"

#ifdef __EMSCRIPTEN__
#define GLFW_INCLUDE_ES3
#include <GLES3/gl3.h>
#elif !defined(DONNER_EDITOR_WGPU)
#include "glad/glad.h"
#endif

#include "donner/svg/compositor/CompositorController.h"

namespace donner::geode {
class GeodeDevice;
}  // namespace donner::geode

namespace donner::editor {

/// Stateful — owns one preview resource per active composite tile (keyed on
/// `CompositeTileSnapshot::id`). OpenGL builds upload CPU thumbnail pixels into
/// small GL textures. WebGPU builds keep backend texture snapshots alive and
/// pass their texture views directly to ImGui, avoiding thumbnail readback.
/// Construct and destroy on the presentation thread.
class LayerInspectorPanel {
public:
  explicit LayerInspectorPanel(std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice = nullptr);
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
  /// @param coverageDiagnostics Active bounded-raster and overview-infill presentation coverage.
  /// @param fastPath Fast-path counters rendered as a summary line
  ///   above the table.
  void render(std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles,
              const svg::compositor::CompositorController::StateSnapshot& state,
              Entity workerCompositorEntity, double viewportZoom, double viewportDpr,
              const Vector2i& viewportDesiredCanvas, const Vector2i& documentCanvas,
              const PresentationCoverageDiagnostics& coverageDiagnostics,
              const svg::compositor::CompositorController::FastPathCounters& fastPath,
              const svg::compositor::CompositorController::RenderFrameStats& renderStats);

  /// Advance one UI presentation frame for backend texture retirement.
  void advancePresentationFrame();

private:
#ifdef DONNER_EDITOR_WGPU
  using ThumbnailTextureHandle = ImTextureID;
  struct WgpuUploadedTexture;
#else
  using ThumbnailTextureHandle = GLuint;
#endif

  struct ThumbnailTexture {
    ThumbnailTextureHandle texture = 0;
    std::shared_ptr<const svg::RendererTextureSnapshot> textureSnapshot;
#ifdef DONNER_EDITOR_WGPU
    std::shared_ptr<WgpuUploadedTexture> uploadedTexture;
#endif
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

  /// Record newly-observed segment heuristic samples into the bounded telemetry history.
  void recordHeuristicTelemetrySamples(
      std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles,
      const CompositorHeuristicTelemetryContext& context);

  /// Return the current telemetry history as a JSONL payload.
  [[nodiscard]] std::string heuristicTelemetryHistoryJson() const;

  struct HeuristicTelemetryHistoryEntry {
    std::string key;
    std::string jsonLine;
  };

#ifdef DONNER_EDITOR_WGPU
  struct RetiredSnapshot {
    ThumbnailTextureHandle texture = 0;
    std::shared_ptr<const svg::RendererTextureSnapshot> snapshot;
    std::shared_ptr<WgpuUploadedTexture> uploadedTexture;
  };

  using RetiredSnapshotBatch = std::vector<RetiredSnapshot>;

  static ThumbnailTextureHandle ToImTextureId(const svg::RendererTextureSnapshot* textureSnapshot);
  std::shared_ptr<WgpuUploadedTexture> uploadThumbnailPixelsToWgpu(
      const std::vector<uint8_t>& pixels, const Vector2i& dimensions);
  static RetiredSnapshot RetireSnapshot(
      ThumbnailTextureHandle texture, std::shared_ptr<const svg::RendererTextureSnapshot> snapshot,
      std::shared_ptr<WgpuUploadedTexture> uploadedTexture);
  static void ReleaseImGuiTexture(ThumbnailTextureHandle texture);

  void retireSnapshots(RetiredSnapshotBatch snapshots);

  std::shared_ptr<::donner::geode::GeodeDevice> geodeDevice_;
#endif

  std::array<char, 4096> telemetryPathBuffer_{};
  std::string telemetryStatus_;
  std::deque<HeuristicTelemetryHistoryEntry> telemetryHistory_;
  std::unordered_set<std::string> telemetryHistoryKeys_;
  std::uint64_t telemetrySequence_ = 0;
  std::unordered_map<std::string, ThumbnailTexture> textures_;
#ifdef DONNER_EDITOR_WGPU
  RetiredSnapshotBatch pendingRetiredSnapshots_;
  std::deque<RetiredSnapshotBatch> retiredSnapshotFrames_;
#endif
};

}  // namespace donner::editor
