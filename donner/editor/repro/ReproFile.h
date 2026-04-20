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
/// {"v":1,"svg":"path.svg","wnd":[1600,900],"scale":2.0,"exp":false}
/// {"f":0,"t":0.0,"dt":16.6,"mx":100.5,"my":50.2,"btn":0,"mod":0}
/// {"f":1,"t":16.6,"dt":16.7,"mx":120.0,"my":50.0,"btn":0,"mod":0,
///  "e":[{"k":"mdown","b":0},{"k":"wheel","dy":1.0}]}
/// ...
/// ```
///
/// Frame records carry the full mouse state plus any discrete events
/// that fired during that frame. Discrete events are keyed by short
/// type codes:
///
/// | code   | meaning                  | fields                        |
/// |--------|--------------------------|-------------------------------|
/// | mdown  | mouse button down        | b (button index 0-4)          |
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

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace donner::editor::repro {

/// File format version. Bump when format changes; loader rejects unknown versions.
constexpr int kReproFileVersion = 1;

/// Maximum number of mouse buttons recorded. Matches ImGui's
/// `ImGuiMouseButton_COUNT`.
constexpr int kMaxMouseButtons = 5;

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
  /// Bitmask of currently-held mouse buttons. Bit N set means button N down.
  int mouseButtonMask = 0;
  /// Current modifier bitmask (see `ReproEvent::modifiers`).
  int modifiers = 0;
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
/// to `stderr`.
[[nodiscard]] std::optional<ReproFile> ReadReproFile(const std::filesystem::path& path);

}  // namespace donner::editor::repro
