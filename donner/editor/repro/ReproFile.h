#pragma once
/// @file
///
/// Data model for a `.donner-repro` file: a recorded sequence of editor
/// UI inputs (mouse events, keyboard events, wheel events, window
/// resizes) plus enough session metadata to re-instantiate the editor
/// and replay the events deterministically.
///
/// The file records at the raw ImGui-input level (below menu action
/// dispatch, below tool dispatch, below the compositor) so every stage
/// of the stack — from Donner's DOM mutations down through the
/// RendererTinySkia pixel ops — is exercised during playback. That
/// breadth is the point: a recording made in the live editor
/// reproduces the bug regardless of which layer it lives in.
///
/// Format is one JSON object per line (NDJSON):
///
/// ```
/// {"v":2,"svg":"path.svg","wnd":[1600,900],"scale":2.0,"exp":false}
/// {"f":0,"t":0.0,"dt":16.6,"mx":100.5,"my":50.2,"btn":0,"mod":0,
///  "mdx":12.4,"mdy":33.1,
///  "vp":{"ox":560,"oy":22,"pw":1040,"ph":878,"dpr":2,
///        "z":1.0,"pdx":446,"pdy":256,"psx":1080,"psy":461,
///        "vbx":0,"vby":0,"vbw":892,"vbh":512}}
/// {"f":1,"t":16.6,"dt":16.7,"mx":120.0,"my":50.0,"btn":1,"mod":0,
///  "mdx":34.0,"mdy":33.0,
///  "e":[{"k":"mdown","b":0,"hit":{"id":"lightning","tag":"g"}}]}
/// ...
/// ```
///
/// Frame records carry the full mouse state plus any discrete events
/// that fired during that frame. Discrete events are keyed by short
/// type codes:
///
/// | code   | meaning                  | fields                        |
/// |--------|--------------------------|-------------------------------|
/// | mdown  | mouse button down        | b (button index 0-4), hit{}   |
/// | mup    | mouse button up          | b                             |
/// | kdown  | keyboard key down        | k (ImGui key enum), m (mods)  |
/// | kup    | keyboard key up          | k, m                          |
/// | chr    | character input          | c (UTF-32 code point)         |
/// | wheel  | mouse wheel              | dx, dy (float units)          |
/// | resize | window resize            | w, h                          |
/// | focus  | window focus change      | on (0/1)                      |
///
/// The "frame record" itself captures the continuous state (mouse
/// position + current button mask) so a dropped event in the discrete
/// list can't leave the player in an inconsistent state — the next
/// frame's state trumps.
///
/// Since v2 the frame record also carries:
///   - `mdx` / `mdy`: mouse position in SVG-document coordinates,
///     computed by the live editor from the current viewport. Replay
///     MUST use these as the authoritative doc-space coord rather than
///     re-deriving from `(mx, my)` + a reconstructed viewport. The
///     reconstruction was the exact failure mode v2 was designed to
///     eliminate: the replayer used approximate pane-layout constants
///     that drifted from the live editor and sent clicks to the wrong
///     elements.
///   - `vp`: a snapshot of the `ViewportState` the editor was using at
///     snapshot time (pane origin, pane size, zoom, pan, viewBox,
///     DPR). Emitted only when it differs from the previous frame's
///     `vp` to keep file size in check. Replay reconstructs the full
///     state by carrying forward the prior snapshot.
///   - `mdown.hit`: the element under the cursor at mouse-down time.
///     Replay does its own hit-test and fails loudly if the recorded
///     hit doesn't match, turning silent divergence into a hard error.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace donner::editor::repro {

/// File format version. Bump when format changes; loader rejects
/// unknown versions.
///
/// - v1: mouse in window coords only; no viewport, no doc coords, no
///   hit-test checkpoint. DEPRECATED — rejected by the loader.
/// - v2: adds per-frame doc-space mouse coord, per-frame viewport
///   snapshot (delta-encoded), and hit-test checkpoint on mouse-down.
constexpr int kReproFileVersion = 2;

/// Maximum number of mouse buttons recorded. Matches ImGui's
/// `ImGuiMouseButton_COUNT`.
constexpr int kMaxMouseButtons = 5;

/// Snapshot of the editor's `ViewportState` at record time. Fully
/// captures the window→document coordinate mapping (pane origin +
/// size, zoom, pan anchor, viewBox, DPR). Two snapshots that compare
/// equal produce the same `ViewportState::screenToDocument` mapping
/// up to floating-point reproducibility.
struct ReproViewport {
  double paneOriginX = 0.0;
  double paneOriginY = 0.0;
  double paneSizeW = 0.0;
  double paneSizeH = 0.0;
  double devicePixelRatio = 1.0;
  double zoom = 1.0;
  double panDocX = 0.0;
  double panDocY = 0.0;
  double panScreenX = 0.0;
  double panScreenY = 0.0;
  double viewBoxX = 0.0;
  double viewBoxY = 0.0;
  double viewBoxW = 0.0;
  double viewBoxH = 0.0;

  friend bool operator==(const ReproViewport&, const ReproViewport&) = default;
};

/// Result of a hit-test captured at mouse-down time.
///
/// **Diagnostic only — NOT a load-bearing replay checkpoint.** The
/// recorded element is the raw `EditorApp::hitTest` result. Downstream
/// the selection may legitimately diverge across code versions — e.g.
/// `SelectTool::ElevateToCompositingGroupAncestor` can promote a path
/// click to its enclosing `<g filter>`, and that elevation policy is
/// an internal detail that can change without breaking the recording.
/// A replayer that ASSERTed on hit equivalence would then turn every
/// internals refactor into a mass rerecord. Use this field for
/// human-readable diagnostics ("the recording clicked #lightning; the
/// live hit-test also lands on #lightning"), not for test assertions.
struct ReproHit {
  /// `id` attribute of the hit element, or empty if no hit / no id.
  std::string id;
  /// Tag name of the hit element, or empty if no hit.
  std::string tag;
  /// Document-order index of the hit element among all SVG elements
  /// (pre-order traversal). `-1` when no element was hit. Stable across
  /// record↔replay as long as the DOM structure hasn't been mutated
  /// before the mouse-down (the recorder captures BEFORE dispatching
  /// the event, so any prior mouse-down is already fully resolved).
  int docOrderIndex = -1;

  /// True when no element was under the cursor. Distinguishes an
  /// intentional "empty space" click from an unspecified hit.
  bool empty = false;

  friend bool operator==(const ReproHit&, const ReproHit&) = default;
};

/// One discrete event that fired within a frame. Frame-state
/// (mouse position, button mask) lives on the owning frame record;
/// this captures only events that can't be reconstructed from
/// continuous state (key presses, character input, wheel deltas,
/// resizes).
struct ReproEvent {
  enum class Kind {
    MouseDown,
    MouseUp,
    KeyDown,
    KeyUp,
    Char,
    Wheel,
    Resize,
    Focus,
  };
  Kind kind = Kind::MouseDown;
  // Mouse button index for MouseDown / MouseUp. 0 = left, 1 = right, 2 = middle.
  int mouseButton = 0;
  // ImGui key enum value for KeyDown / KeyUp. Preserved as int to avoid
  // importing imgui.h in this header.
  int key = 0;
  // Modifier flags (Ctrl/Shift/Alt/Super) packed as a bitmask matching
  // ImGui's `ImGuiKey_ModXxx` bits.
  int modifiers = 0;
  // UTF-32 code point for Char events.
  std::uint32_t codepoint = 0;
  // Wheel delta (x, y) for Wheel events.
  float wheelDeltaX = 0.0f;
  float wheelDeltaY = 0.0f;
  // Window width / height for Resize events (logical, pre-DPI).
  int width = 0;
  int height = 0;
  // Focus on/off for Focus events.
  bool focusOn = true;
  /// Hit-test checkpoint populated for `MouseDown` events. Empty for
  /// other event kinds.
  std::optional<ReproHit> hit;
};

/// One frame's snapshot: continuous input state + any discrete events
/// that fired during the frame.
struct ReproFrame {
  /// Monotonic frame index starting at 0.
  std::uint64_t index = 0;
  /// Seconds since the recording started.
  double timestampSeconds = 0.0;
  /// ImGui delta-time value that the frame advanced by, in
  /// milliseconds. Replayer uses this to set `ImGuiIO::DeltaTime`.
  double deltaMs = 0.0;
  /// Current mouse position in logical window coordinates.
  double mouseX = 0.0;
  double mouseY = 0.0;
  /// Current mouse position in SVG-document coordinates. Present when
  /// the editor fed a live `ViewportState` to the recorder (which it
  /// always does in v2). Replay uses this directly — no reconstruction
  /// of pane layout, no hand-tuned layout constants.
  std::optional<double> mouseDocX;
  std::optional<double> mouseDocY;
  /// Bitmask of currently-held mouse buttons. Bit N set means button N down.
  int mouseButtonMask = 0;
  /// Current modifier bitmask (see `ReproEvent::modifiers`).
  int modifiers = 0;
  /// Viewport snapshot for this frame. Emitted only when it differs
  /// from the previous frame's snapshot, so replay has to carry
  /// forward the prior value when this is absent.
  std::optional<ReproViewport> viewport;
  /// Discrete events that fired during this frame, in arrival order.
  std::vector<ReproEvent> events;
};

/// Session-level metadata captured at recording start.
struct ReproMetadata {
  /// Path to the SVG being edited when recording started. Relative or
  /// absolute — player resolves against its working directory.
  std::string svgPath;
  /// Logical window size at start. Replayer sets this on its mock window.
  int windowWidth = 0;
  int windowHeight = 0;
  /// HiDPI display scale at start (`io.DisplayFramebufferScale.x`).
  double displayScale = 1.0;
  /// Whether the editor was started with `--experimental`.
  bool experimentalMode = false;
  /// Absolute wall-clock timestamp when recording started, ISO-8601.
  /// Informational only; not used by the player.
  std::string startedAtIso8601;
};

/// In-memory form of a loaded or in-progress recording.
struct ReproFile {
  ReproMetadata metadata;
  std::vector<ReproFrame> frames;
};

/// Serialize `file` to the given path in NDJSON form. Returns `true`
/// on success; on failure writes an error message to `stderr` and
/// returns `false`. Atomic: writes to `path.tmp` then renames over
/// `path`, so a crash mid-write never truncates an existing file.
[[nodiscard]] bool WriteReproFile(const std::filesystem::path& path,
                                  const ReproFile& file);

/// Parse an NDJSON repro file. Returns `std::nullopt` on any error
/// (file missing, version mismatch, malformed line); writes details
/// to `stderr`. v1 files are rejected with a clear diagnostic telling
/// the user to rerecord.
[[nodiscard]] std::optional<ReproFile> ReadReproFile(const std::filesystem::path& path);

}  // namespace donner::editor::repro
