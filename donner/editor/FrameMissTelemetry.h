#pragma once
/// @file

#include <cstdint>
#include <string>

#include "donner/editor/FrameCostBreakdown.h"

namespace donner::editor {

/// Frame-budget miss severity.
enum class FrameBudgetMiss {
  WithinBudget,
  Missed120Hz,
  Missed60Hz,
};

/// Presentation memory counters included in frame-miss telemetry.
struct FrameMissResourceTelemetry {
  /// Bytes retained by the current overlay texture, if the retained overlay fallback is active.
  std::uint64_t overlayBytes = 0;
  /// Bytes retained by active composited tile textures.
  std::uint64_t activeTileBytes = 0;
  /// Bytes retained by zoom-out overview tile textures.
  std::uint64_t overviewTileBytes = 0;
  /// Bytes retained by retired textures waiting for presentation-frame aging.
  std::uint64_t retiredBytes = 0;
  /// Total bytes directly tracked by the presentation cache.
  std::uint64_t totalTrackedBytes = 0;
  /// Peak tracked bytes observed by the presentation cache.
  std::uint64_t peakTrackedBytes = 0;
  /// Process-lifetime WebGPU texture-create count from the shared Geode device.
  std::uint64_t wgpuLifetimeTextureCreates = 0;
  /// Process-lifetime WebGPU buffer-create count from the shared Geode device.
  std::uint64_t wgpuLifetimeBufferCreates = 0;
};

/// Input used to build one frame-budget miss telemetry record.
struct FrameMissTelemetryInput {
  /// Monotonic editor UI-frame index.
  std::uint64_t frameIndex = 0;
  /// Current UI-frame duration in milliseconds.
  double frameMs = 0.0;
  /// Async worker result duration that landed on this UI frame, or zero when none landed.
  double backendMs = 0.0;
  /// Detailed frame-cost counters collected by the editor frame graph.
  FrameCostBreakdown frameCost;
  /// Presentation resource counters observed on the same frame.
  FrameMissResourceTelemetry resources;
};

/// Return the frame-budget miss severity for @p frameMs.
[[nodiscard]] FrameBudgetMiss ClassifyFrameBudgetMiss(double frameMs);

/// Return a stable lowercase string for @p miss.
[[nodiscard]] const char* FrameBudgetMissName(FrameBudgetMiss miss);

/// Sum known profiler regions that execute on, or directly block, the UI frame.
[[nodiscard]] double KnownUiFrameCostMs(const FrameCostBreakdown& cost);

/// Sum async compositor worker regions that may explain delayed presentation results.
[[nodiscard]] double KnownAsyncWorkerCostMs(const FrameCostBreakdown& cost);

/**
 * Serialize one frame-budget miss as a JSONL record.
 *
 * Returns an empty string when the frame stayed within the 120 Hz budget.
 *
 * @param input Frame and cost counters to serialize.
 * @return JSON object ending with a trailing newline, or an empty string.
 */
[[nodiscard]] std::string BuildFrameMissTelemetryJson(const FrameMissTelemetryInput& input);

}  // namespace donner::editor
