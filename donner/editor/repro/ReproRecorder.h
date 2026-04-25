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
///
/// Since v2 of the file format, the recorder also captures per-frame
/// viewport state + mouse-down hit-test checkpoints so replay can use
/// authoritative document-space coords rather than reconstructing
/// screen→doc math from hand-tuned pane-layout constants. See
/// `ReproFile.h` for the full rationale.

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "donner/editor/repro/ReproFile.h"

namespace donner::editor::repro {

/// Callbacks the recorder uses to ground its frame snapshots against
/// live editor state. Supplied by the owner (EditorShell) right after
/// `ImGui::NewFrame` and before any widget consumes input — the same
/// seam `snapshotFrame()` already runs at.
struct FrameContext {
  /// Current viewport snapshot, or `nullopt` if the editor hasn't
  /// laid out the render pane yet (e.g. first few frames). When
  /// absent, the recorder omits `vp` and `mdx`/`mdy` for this frame;
  /// the replay can still use the prior frame's viewport.
  std::optional<ReproViewport> viewport;
  /// Mouse position in SVG-document coords for the current frame, as
  /// computed by the live editor from `viewport`. `nullopt` when the
  /// viewport isn't available OR when the mouse is outside the render
  /// pane. The live editor always uses this coord for tool dispatch
  /// so recording it is the authoritative replay value.
  std::optional<std::pair<double, double>> mouseDoc;
  /// Invoked at mouse-down time with the recorded mouse-document
  /// coord; returns the element the editor hit-tested to, or an
  /// "empty" ReproHit for clicks on empty space. Replay uses this as
  /// a checkpoint to catch silent click-lands-wrong-element
  /// divergence. May be `nullptr` (tests / early init).
  std::function<ReproHit(double docX, double docY)> hitTester;
};

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
  /// `context` supplies the live viewport + hit-tester. It's allowed
  /// to be default-constructed on frames before the viewport has been
  /// laid out; the recorder degrades gracefully (window-coord-only
  /// capture for those frames, which the replay tolerates).
  ///
  /// Cheap (a few dozen ImGui field reads + a small vector append),
  /// but the buffer grows linearly with recording duration. A
  /// 10-minute session at 60 fps produces ~36k frames; back-of-envelope
  /// ~6 MB serialized with viewport deltas. Not streamed to disk;
  /// held entirely in memory until `flush()`.
  void snapshotFrame(const FrameContext& context = {});

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
  // Last viewport written to the file — we delta-encode so only
  // actual changes consume bytes.
  std::optional<ReproViewport> lastEmittedViewport_;
};

}  // namespace donner::editor::repro
