#pragma once
/// @file

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"
#include "donner/svg/compositor/CompositorController.h"

namespace donner::editor {

/// Canvas freshness state shown by the layer inspector.
enum class CanvasFreshness {
  Current,
  CommitStalled,
  CompositorBehind,
};

/// Classify desired, document, and compositor canvas-size agreement.
///
/// @param viewportDesiredCanvas Canvas size implied by the current viewport.
/// @param documentCanvas Canvas size committed to the SVG document.
/// @param compositorCanvas Canvas size last rasterized by the compositor.
[[nodiscard]] CanvasFreshness ClassifyCanvasFreshness(const Vector2i& viewportDesiredCanvas,
                                                      const Vector2i& documentCanvas,
                                                      const Vector2i& compositorCanvas);

/// User-visible status suffix for a canvas freshness state.
///
/// @param freshness State returned by \ref ClassifyCanvasFreshness.
[[nodiscard]] std::string_view CanvasFreshnessStatusSuffix(CanvasFreshness freshness);

/// Context fields written with a compositor heuristic telemetry snapshot.
struct CompositorHeuristicTelemetryContext {
  /// Editor viewport zoom at export time.
  double viewportZoom = 1.0;
  /// Editor viewport device-pixel ratio at export time.
  double viewportDpr = 1.0;
  /// Canvas size requested by the current viewport.
  Vector2i viewportDesiredCanvas = Vector2i::Zero();
  /// Canvas size currently committed to the document.
  Vector2i documentCanvas = Vector2i::Zero();
  /// Compositor-wide diagnostic state.
  svg::compositor::CompositorController::StateSnapshot state;
  /// Fast-path counters captured by the compositor.
  svg::compositor::CompositorController::FastPathCounters fastPath;
  /// Last completed compositor render costs.
  svg::compositor::CompositorController::RenderFrameStats renderStats;
};

/// Serialize the current compositor span heuristic state as one JSON object.
///
/// @param tiles Paint-order composite tile snapshot from the async renderer.
/// @param context Viewport, canvas, and aggregate compositor state.
/// @return JSON object ending with a trailing newline, suitable for JSONL append.
[[nodiscard]] std::string BuildCompositorHeuristicTelemetryJson(
    std::span<const svg::compositor::CompositorController::CompositeTileSnapshot> tiles,
    const CompositorHeuristicTelemetryContext& context);

/// Serialize one segment heuristic sample as one JSONL record.
///
/// @param tile Segment tile to serialize.
/// @param context Viewport, canvas, and aggregate compositor state.
/// @param sequence Monotonic sample sequence assigned by the editor.
/// @return JSON object ending with a trailing newline.
[[nodiscard]] std::string BuildCompositorHeuristicTelemetrySampleJson(
    const svg::compositor::CompositorController::CompositeTileSnapshot& tile,
    const CompositorHeuristicTelemetryContext& context, std::uint64_t sequence);

/// Append a compositor heuristic telemetry record to a JSONL file.
///
/// @param path Destination file path.
/// @param json One JSON object, normally returned by \ref BuildCompositorHeuristicTelemetryJson.
/// @param error Optional destination for a human-readable failure message.
/// @return True when the record was appended.
[[nodiscard]] bool AppendCompositorHeuristicTelemetry(std::string_view path, std::string_view json,
                                                      std::string* error);

/// Save compositor heuristic telemetry to a JSONL file, replacing any existing contents.
///
/// @param path Destination file path.
/// @param json JSONL payload to write.
/// @param error Optional destination for a human-readable failure message.
/// @return True when the payload was written.
[[nodiscard]] bool SaveCompositorHeuristicTelemetry(std::string_view path, std::string_view json,
                                                    std::string* error);

}  // namespace donner::editor
