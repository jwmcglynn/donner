#pragma once
/// @file

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "donner/editor/FrameCostBreakdown.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/RenderPaneGesture.h"
#include "donner/editor/Tool.h"
#include "donner/editor/ViewportState.h"

namespace donner::editor {

constexpr std::size_t kFrameHistoryCapacity = 120;

/// Render-pane profiler costs aligned with one UI frame-history sample.
struct FrameProfilerSample {
  /// Time spent collecting overlay chrome geometry.
  float overlayCaptureMs = 0.0f;
  /// Time spent drawing overlay chrome into the overlay renderer.
  float overlayDrawMs = 0.0f;
  /// Time spent snapshotting overlay chrome from the renderer.
  float overlaySnapshotMs = 0.0f;
  /// Time spent uploading or registering overlay presentation payloads.
  float overlayUploadMs = 0.0f;
  /// Time spent uploading or registering composited content tiles.
  float compositedUploadMs = 0.0f;
  /// Worker time spent rendering transient immediate compositor spans.
  float compositedRenderImmediateMs = 0.0f;
  /// Worker time spent rendering retained cached compositor tiles.
  float compositedRenderCachedMs = 0.0f;
  /// Time spent laying out source-focus ropes.
  float sourceRopeLayoutMs = 0.0f;
  /// Time spent updating source-focus rope simulation/state.
  float sourceRopeUpdateMs = 0.0f;
  /// Time spent issuing source-focus rope draw commands.
  float sourceRopeDrawMs = 0.0f;

  /// Sum of known profiled costs.
  [[nodiscard]] float totalProfiledMs() const;
};

/// Presentation memory retained by the editor texture cache for one UI frame.
struct FrameMemorySample {
  /// Bytes retained by the current overlay texture.
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

struct FrameHistory {
  /// ImGui frame delta per UI-thread frame — populated from
  /// `ImGui::GetIO().DeltaTime` by `noteFrameDelta`.
  std::array<float, kFrameHistoryCapacity> deltaMs{};
  /// Async-renderer worker/presentation latency (ms) per UI frame, aligned 1:1
  /// with the matching `deltaMs[]` slot. Holds `0.0f` for frames where no
  /// render result landed (the worker was still busy or nothing was requested);
  /// consumers skip zero entries so the graph doesn't drop to zero between
  /// drags.
  std::array<float, kFrameHistoryCapacity> backendMs{};
  /// Known frame-cost divisions drawn as the color-bar profiler.
  std::array<FrameProfilerSample, kFrameHistoryCapacity> profiler{};
  /// Presentation memory retained by the editor texture cache.
  std::array<FrameMemorySample, kFrameHistoryCapacity> memory{};
  std::size_t writeIndex = 0;
  std::size_t samples = 0;
  /// Most recent non-zero worker sample, so latched-worker-latency
  /// readers (the numeric readout, sticky-line rendering) have something
  /// to show between render-result landings.
  float lastBackendMs = 0.0f;

  /// Append a new frame sample. The matching `backendMs[]` slot is
  /// reset to 0 ("no worker result this frame"); `setLatestBackendMs`
  /// fills it in if a render result lands during the same UI frame.
  void push(float ms);
  /// Record an async-renderer worker timing for the most
  /// recently pushed frame. Called by `RenderCoordinator::pollRenderResult`
  /// when a new `RenderResult` arrives. No-op if no frame has been pushed
  /// yet. Also updates `lastBackendMs` so UI elements that want to show the
  /// latest measured worker latency have a persistent value.
  void setLatestBackendMs(float ms);
  /// Record the editor render-cost breakdown for the most recently pushed
  /// frame. No-op if no frame has been pushed yet.
  void setLatestFrameCost(const FrameCostBreakdown& cost);
  /// Record presentation-memory counters for the most recently pushed frame.
  /// No-op if no frame has been pushed yet.
  void setLatestMemorySample(const FrameMemorySample& sample);
  /// Return the newest non-zero presentation-memory sample, or zeroes if none exist.
  [[nodiscard]] FrameMemorySample latestNonZeroMemorySample() const;
  [[nodiscard]] float latest() const;
  [[nodiscard]] float max() const;
};

struct PendingClick {
  Vector2d documentPoint;
  MouseModifiers modifiers;
};

/**
 * Return whether the render pane should show the pan-mode cursor.
 *
 * @param canvasHovered True when the mouse is over the interactive canvas area.
 * @param spaceHeld True while the spacebar pan modifier is held.
 * @param panning True while an active mouse-pan drag is in progress.
 */
[[nodiscard]] bool ShouldShowRenderPanePanCursor(bool canvasHovered, bool spaceHeld, bool panning);

/// Owns viewport/input state for the render pane: zoom/pan, pending click dispatch, and frame
/// timing history.
class ViewportInteractionController {
public:
  [[nodiscard]] ViewportState& viewport() { return viewport_; }
  [[nodiscard]] const ViewportState& viewport() const { return viewport_; }

  [[nodiscard]] FrameHistory& frameHistory() { return frameHistory_; }
  [[nodiscard]] const FrameHistory& frameHistory() const { return frameHistory_; }

  void noteFrameDelta(float deltaMs) { frameHistory_.push(deltaMs); }

  void updatePaneLayout(const Vector2d& paneOrigin, const Vector2d& paneSize,
                        const std::optional<Box2d>& documentViewBox);
  void updateDevicePixelRatio(double devicePixelRatio);
  void resetToActualSize();
  void applyZoom(double factor, const Vector2d& focalScreen);

  void updatePanState(bool paneHovered, bool spaceHeld, bool middleDown, bool leftDown,
                      const ImVec2& mousePosition);
  [[nodiscard]] bool panning() const { return panning_; }

  void consumeScrollEvents(std::vector<RenderPaneScrollEvent>& events, const Box2d& paneRect,
                           bool modalCapturingInput, double wheelZoomStep,
                           double panPixelsPerScrollUnit);

  void bufferPendingClick(const Vector2d& documentPoint, MouseModifiers modifiers);
  [[nodiscard]] const std::optional<PendingClick>& pendingClick() const { return pendingClick_; }
  void clearPendingClick() { pendingClick_.reset(); }

private:
  ViewportState viewport_;
  FrameHistory frameHistory_;
  bool panning_ = false;
  ImVec2 lastPanMouse_ = ImVec2(0.0f, 0.0f);
  std::optional<PendingClick> pendingClick_;
};

}  // namespace donner::editor
