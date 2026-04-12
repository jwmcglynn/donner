#pragma once
/// @file
///
/// Headless frame inspection: decode a sandbox wire stream into an indexed
/// list of commands with human-readable summaries, and replay any prefix of
/// those commands into a real backend. This is the **engine** that S4's
/// eventual ImGui frame inspector panel will sit on top of; keeping it
/// UI-free means we can test it now and reuse it from non-editor contexts
/// (bug reports, `.rnr` replay CLIs, structural diffs).
///
/// The inspector never rasterizes on its own — rasterization is always
/// delegated to a caller-provided `RendererInterface`. Decoding, on the
/// other hand, is fully self-contained and deterministic.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "donner/editor/sandbox/ReplayingRenderer.h"
#include "donner/editor/sandbox/Wire.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::sandbox {

/// One message in the decoded stream. The summary is meant for UI display
/// (ImGui command-list rows, text dumps, structural diffs) — it is
/// intentionally terse and not round-trippable.
struct DecodedCommand {
  /// Zero-based message index inside the wire stream. Index 0 is always
  /// the `kStreamHeader` message.
  uint32_t index = 0;
  /// Opcode raw value. Use `Opcode` cast for type-safe comparisons.
  Opcode opcode = Opcode::kInvalid;
  /// LIFO nesting depth at the point this command was emitted. Incremented
  /// by every `push*` and decremented by every `pop*`. Unbalanced streams
  /// can go negative briefly; the inspector clamps at zero for display.
  int32_t depth = 0;
  /// Byte offset of this message's header inside the wire buffer.
  std::size_t byteOffset = 0;
  /// Total bytes this message occupies (8-byte header + payload).
  std::size_t byteLength = 0;
  /// Human-readable one-liner (e.g. `drawRect (10,10,30,30)`,
  /// `setPaint fill=#FF0000 stroke=none`). May be empty for opcodes the
  /// summarizer doesn't understand yet.
  std::string summary;
};

/// Outcome of `FrameInspector::Decode`. `streamValid` is true iff the entire
/// wire stream parsed without error — a false value means `commands`
/// contains whatever the inspector was able to decode before the first
/// failure, and `error` describes why it stopped.
struct InspectionResult {
  std::vector<DecodedCommand> commands;
  bool streamValid = false;
  std::string error;
  /// Final nesting depth after the last successfully decoded command.
  /// Should be 0 for a well-formed frame; any non-zero value means the
  /// push/pop pairs are unbalanced.
  int32_t finalDepth = 0;
};

/// All static helpers — inspector instances are stateless beyond the
/// inputs each call is given.
class FrameInspector {
public:
  /// Parses `wire` into a command list without invoking any renderer.
  /// Always produces a result (even on partial parses); callers inspect
  /// `InspectionResult::streamValid` to distinguish "fully decoded" from
  /// "stopped at an error".
  static InspectionResult Decode(std::span<const uint8_t> wire);

  /// Replays exactly the first `commandCount` commands into `target`. The
  /// counting starts at the `kBeginFrame` message — the `kStreamHeader`
  /// is always consumed but not counted as a "command". After the prefix
  /// is dispatched, `target.endFrame()` is synthesized if the prefix
  /// didn't already include a real `kEndFrame`, so the target has a valid
  /// frame to snapshot.
  ///
  /// Passing `commandCount = SIZE_MAX` replays the entire stream verbatim.
  /// `commandCount = 0` begins a frame and immediately ends it — useful as
  /// a "clear to the start of the frame" baseline in the UI.
  ///
  /// @returns `ReplayStatus::kOk` on success, `kMalformed` if the stream
  ///   is corrupt, `kHeaderMismatch` on a missing/wrong header.
  static ReplayStatus ReplayPrefix(std::span<const uint8_t> wire,
                                   std::size_t commandCount,
                                   svg::RendererInterface& target);

  /// Builds a single-line textual summary of the wire stream: one row per
  /// command, with indentation tracking nesting depth. Intended for
  /// `sandbox_inspect` output and crash-report attachments.
  static std::string Dump(std::span<const uint8_t> wire);

  /// Returns a short, human-readable name for an opcode (e.g. "drawRect",
  /// "pushTransform"). Stable — safe to embed in logs and bug reports.
  static std::string_view OpcodeName(Opcode op);

  /// Returns a short, human-readable name for an unsupported-kind tag.
  static std::string_view UnsupportedKindName(UnsupportedKind kind);
};

}  // namespace donner::editor::sandbox
