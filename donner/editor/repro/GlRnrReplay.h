#pragma once
/// @file

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/LayerInspectorDiagnostics.h"

namespace donner::editor::repro {

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

/// Per-frame diagnostics captured during GL replay.
struct GlRnrReplayFrameDiagnostics {
  /// Repro frame index.
  std::uint64_t frameIndex = 0;
  /// Canvas freshness classification used by the layer inspector.
  CanvasFreshness canvasFreshness = CanvasFreshness::Current;
  /// Status suffix rendered beside document canvas diagnostics.
  std::string statusSuffix;
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
