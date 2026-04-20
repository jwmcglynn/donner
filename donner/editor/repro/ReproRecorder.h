#pragma once
/// @file
///
/// Live UI-input recorder. Install one instance in `EditorShell` when
/// the user passes `--save-repro <path>`; call `snapshotFrame()` once
/// per editor frame (before any UI widgets have consumed input events
/// — right after `window_.beginFrame()` / `ImGui::NewFrame()`). On
/// process exit, call `flush()` to serialize the recording to the
/// destination path.
///
/// The recorder captures raw ImGui input state (mouse position, mouse
/// button mask, modifier state, wheel deltas, discrete key/char
/// events) plus enough metadata to re-instantiate the editor session.
/// That's the input boundary BELOW menu / tool / compositor dispatch,
/// so a replay exercises every stage of the stack: a recording of a
/// bug you can see in the editor always reproduces that bug in
/// playback, regardless of which layer owns it.

#include <chrono>
#include <filesystem>
#include <string>

#include "donner/editor/repro/ReproFile.h"

namespace donner::editor::repro {

/// Options passed at construction. All fields populated from editor
/// startup state; the recorder copies what it needs.
struct ReproRecorderOptions {
  /// Path where the `.donner-repro` file will be written.
  std::filesystem::path outputPath;
  /// SVG file the editor is editing. Stored verbatim in the metadata.
  std::string svgPath;
  /// Initial logical window dimensions at recording start.
  int windowWidth = 0;
  int windowHeight = 0;
  /// Initial HiDPI scale.
  double displayScale = 1.0;
  /// Whether the editor was started with `--experimental`.
  bool experimentalMode = false;
};

/// Records editor UI inputs frame-by-frame. Not thread-safe; must be
/// invoked from the UI thread (where ImGui context is active).
class ReproRecorder {
public:
  explicit ReproRecorder(ReproRecorderOptions options);

  /// Call ONCE per frame, after `ImGui::NewFrame` and before any widget
  /// code consumes input events. Reads the current ImGuiIO state,
  /// diffs against the previous frame's snapshot, appends a frame
  /// record + any discrete events to the in-memory recording buffer.
  ///
  /// Cheap (a few dozen ImGui field reads + a small vector append),
  /// but the buffer grows linearly with recording duration. A
  /// 10-minute session at 60 fps produces ~36k frames; back-of-envelope
  /// ~5 MB serialized. Not streamed to disk; held entirely in memory
  /// until `flush()`.
  void snapshotFrame();

  /// Serialize the in-memory recording to the configured output path.
  /// Called from the editor's shutdown path (or invoked manually from
  /// a signal handler); safe to call more than once (subsequent calls
  /// overwrite the file atomically).
  ///
  /// Returns `true` on success.
  [[nodiscard]] bool flush();

  /// Number of frames captured so far. Useful for diagnostics / debug
  /// logging.
  [[nodiscard]] std::size_t frameCount() const { return file_.frames.size(); }

private:
  ReproRecorderOptions options_;
  ReproFile file_;
  std::chrono::steady_clock::time_point startTime_;
  bool started_ = false;
  // Previous-frame state for diffing discrete events.
  double prevMouseX_ = 0.0;
  double prevMouseY_ = 0.0;
  int prevButtonMask_ = 0;
  int prevModifiers_ = 0;
  int prevWindowWidth_ = 0;
  int prevWindowHeight_ = 0;
  bool prevWindowFocused_ = true;
};

}  // namespace donner::editor::repro
