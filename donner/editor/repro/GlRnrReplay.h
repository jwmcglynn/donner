#pragma once
/// @file

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/editor/AsyncRenderer.h"
#include "donner/editor/FrameCostBreakdown.h"
#include "donner/editor/LayerInspectorDiagnostics.h"

namespace donner::editor::repro {

/// Worker scheduling mode for deterministic GL replay tests.
enum class GlRnrReplayWorkerScheduling {
  Realtime,          //!< Preserve normal wall-clock worker scheduling.
  DrainEachFrame,    //!< Wait for active worker renders before each frame poll.
  HoldFramesBehind,  //!< Hold completed results for a fixed number of frame polls.
};

/// Output crop mode for GL framebuffer replay captures.
enum class GlRnrReplayCropMode {
  Full,            //!< Capture the entire editor framebuffer.
  RenderPane,      //!< Capture the render-pane rectangle.
  DocumentCanvas,  //!< Capture the visible document canvas clipped to the render pane.
};

/// Options for replaying an `.rnr` file through the real OpenGL editor shell.
struct GlRnrReplayOptions {
  /// Path to the `.rnr` recording.
  std::filesystem::path rnrPath;
  /// Optional SVG path override. When empty, the SVG path is resolved from the `.rnr` metadata.
  std::optional<std::filesystem::path> svgPathOverride;
  /// Directory where captured PNGs will be written.
  std::filesystem::path outputDir = "/tmp";
  /// Explicit frame indices to capture.
  std::set<std::uint64_t> captureFrames;
  /// Capture the frame containing the Nth left mouse-down.
  std::optional<int> captureLeftMouseDownOrdinal;
  /// Stop replay after this frame index.
  std::optional<std::uint64_t> maxFrame;
  /// Crop applied before writing captured PNGs.
  GlRnrReplayCropMode cropMode = GlRnrReplayCropMode::Full;
  /// Pace replay by recorded timestamps.
  bool pace = true;
  /// Replay-only worker scheduling control.
  GlRnrReplayWorkerScheduling workerScheduling = GlRnrReplayWorkerScheduling::Realtime;
  /// Number of frame polls to hold each completed worker result in HoldFramesBehind mode.
  int holdFramesBehind = 0;
  /// Replay-only fixed render delay injected into the async worker.
  int workerRenderDelayMsForTesting = 0;
  /// Suppress non-document render-pane chrome when writing captures.
  bool contentOnlyCapture = false;
  /// Show the native editor window while replaying.
  bool visible = false;
};

/// One PNG written by \ref RunGlRnrReplay.
struct GlRnrReplayCapture {
  /// Repro frame index captured.
  std::uint64_t frameIndex = 0;
  /// Human-readable capture reason.
  std::string reason;
  /// Absolute path to the written PNG.
  std::filesystem::path path;
};

/// Per-frame texture tile diagnostics captured during GL replay.
struct GlRnrReplayTileDiagnostics {
  /// Stable texture-cache tile id.
  std::string id;
  /// Segment/layer tile kind.
  RenderResult::CompositedTile::Kind kind = RenderResult::CompositedTile::Kind::Segment;
  /// Raster payload generation.
  std::uint64_t generation = 0;
  /// Texture dimensions in pixels.
  Vector2i bitmapDimsPx = Vector2i::Zero();
  /// Raster canvas size that produced the texture payload.
  Vector2i rasterCanvasSize = Vector2i::Zero();
  /// Tile origin in document/canvas coordinates.
  Vector2d canvasOffsetDoc = Vector2d::Zero();
  /// Tile dimensions in document/canvas units.
  Vector2d bitmapDimsDoc = Vector2d::Zero();
  /// Drag translation represented by the presented tile.
  Vector2d dragTranslationDoc = Vector2d::Zero();
  /// Effective drag translation used by the render-pane presenter this frame.
  Vector2d presentedDragTranslationDoc = Vector2d::Zero();
  /// Backend texture/view handle, represented as an integer for diagnostics.
  std::uint64_t textureHandle = 0;
  /// True when the tile reused an existing texture with metadata-only geometry.
  bool metadataOnly = false;
  /// True when this tile represents the active drag target.
  bool isDragTarget = false;
};

/// Per-frame diagnostics captured during GL replay.
struct GlRnrReplayFrameDiagnostics {
  /// Repro frame index.
  std::uint64_t frameIndex = 0;
  /// Canvas freshness classification used by the layer inspector.
  CanvasFreshness canvasFreshness = CanvasFreshness::Current;
  /// Status suffix rendered beside document canvas diagnostics.
  std::string statusSuffix;
  /// Canvas size implied by the current viewport.
  Vector2i viewportDesiredCanvas = Vector2i::Zero();
  /// Canvas size committed to the document path used by the editor shell.
  Vector2i documentCanvas = Vector2i::Zero();
  /// Canvas size last rasterized by the compositor.
  Vector2i compositorCanvas = Vector2i::Zero();
  /// Metadata-only composited tiles skipped during the last upload.
  int metadataOnlyMissCount = 0;
  /// Duplicate live texture handles found across different tile ids.
  int duplicateLiveTextureCount = 0;
  /// Overlay texture dimensions in pixels.
  Vector2i overlayDimsPx = Vector2i::Zero();
  /// Backend overlay texture/view handle, represented as an integer for diagnostics.
  std::uint64_t overlayTextureHandle = 0;
  /// Latest editor rendering cost counters.
  FrameCostBreakdown frameCost;
  /// Replay worker scheduling mode used for this frame.
  GlRnrReplayWorkerScheduling replayWorkerScheduling = GlRnrReplayWorkerScheduling::Realtime;
  /// Replay-only worker render delay in milliseconds.
  int replayWorkerRenderDelayMsForTesting = 0;
  /// Configured number of frame polls to hold each completed worker result.
  int replayHoldFramesBehind = 0;
  /// Number of completed worker results intentionally withheld during this frame.
  std::uint64_t replayResultHoldPollsThisFrame = 0;
  /// True when replay scheduling intentionally withheld a completed worker result this frame.
  bool replayResultWithheld = false;
  /// Paint-order texture state currently visible to the presenter.
  std::vector<GlRnrReplayTileDiagnostics> tiles;
};

/// Result of a GL replay run.
struct GlRnrReplayResult {
  /// Captures written during replay.
  std::vector<GlRnrReplayCapture> captures;
  /// Per-frame diagnostics gathered after each editor frame runs.
  std::vector<GlRnrReplayFrameDiagnostics> frameDiagnostics;
  /// Selection label after the last replayed frame.
  std::optional<std::string> finalSelectedElementLabel;
};

/**
 * Parse a CLI/MCP crop-mode string.
 *
 * @param value String value such as `full`, `render-pane`, or `document-canvas`.
 */
[[nodiscard]] std::optional<GlRnrReplayCropMode> ParseGlRnrReplayCropMode(std::string_view value);

/**
 * File suffix for a crop mode.
 *
 * @param cropMode Output crop mode.
 */
[[nodiscard]] std::string_view GlRnrReplayCropModeSuffix(GlRnrReplayCropMode cropMode);

/**
 * Replay an `.rnr` recording through `EditorShell`, capture the OpenGL
 * framebuffer, and write requested frames as PNGs.
 *
 * @param options Replay and capture options.
 * @param result Output capture list.
 * @param error Human-readable error on failure.
 */
[[nodiscard]] bool RunGlRnrReplay(const GlRnrReplayOptions& options, GlRnrReplayResult* result,
                                  std::string* error);

}  // namespace donner::editor::repro
