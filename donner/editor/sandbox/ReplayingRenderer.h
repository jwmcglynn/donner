#pragma once
/// @file
///
/// Decodes a `SerializingRenderer` wire stream and dispatches its messages
/// onto a wrapped, real `RendererInterface`. The replay is host-side: the
/// target can be `RendererTinySkia`, `RendererGeode`, or a test mock.
///
/// `ReplayingRenderer::pumpFrame()` is the single entry point. It reads
/// messages from the wire until it encounters `kEndFrame`, an unknown opcode,
/// a malformed payload, or end-of-stream. It never crashes on adversarial
/// input; every failure is reported via a `ReplayStatus` return code.

#include <cstdint>
#include <memory>
#include <span>

#include "donner/base/EcsRegistry.h"
#include "donner/editor/sandbox/Wire.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::sandbox {

/// Outcome of replaying a single frame onto the wrapped backend.
enum class ReplayStatus {
  kOk,                ///< Frame ended cleanly with `kEndFrame`.
  kEndOfStream,       ///< Reader ran out of bytes without seeing `kEndFrame`.
  kMalformed,         ///< A payload failed to decode (length, tag, range).
  kUnknownOpcode,     ///< Reader saw an opcode it doesn't know.
  kHeaderMismatch,    ///< Magic or version mismatch on the stream header.
  kEncounteredUnsupported,  ///< Replay succeeded but hit a `kUnsupported` message.
};

/// Statistics and diagnostic info from a replay. Always valid regardless of status.
struct ReplayReport {
  uint32_t messagesProcessed = 0;
  uint32_t unsupportedCount = 0;
  Opcode lastOpcode = Opcode::kInvalid;
};

class ReplayingRenderer {
public:
  /// Constructs a replayer that will dispatch to `target`. The target's
  /// lifetime must exceed every call to `pumpFrame()`.
  explicit ReplayingRenderer(svg::RendererInterface& target);
  ~ReplayingRenderer();

  ReplayingRenderer(const ReplayingRenderer&) = delete;
  ReplayingRenderer& operator=(const ReplayingRenderer&) = delete;

  /// Replays the given wire-format bytes onto the target backend. The bytes
  /// are expected to begin with a `kStreamHeader` and end with `kEndFrame`.
  ///
  /// @param wire Byte span containing the full frame (may be a view into
  ///   stdout bytes, a mapped file, or a test buffer).
  /// @param[out] report Populated with per-message statistics regardless of
  ///   status.
  /// @returns `kOk` on full success, or an error discriminant describing
  ///   where replay stopped.
  ReplayStatus pumpFrame(std::span<const uint8_t> wire, ReplayReport& report);

private:
  enum class DispatchOutcome {
    kHandled,       ///< Message was fully consumed and dispatched.
    kUnsupported,   ///< `kUnsupported` message — replay skips but remains valid.
    kDecodeError,   ///< Payload failed to decode; reader is marked failed.
    kUnknownOpcode, ///< Opcode not recognized; payload must be skipped.
  };

  DispatchOutcome handleMessage(WireReader& r, Opcode opcode);

  // WIRE.5: the replayer owns a private ECS registry used to materialize
  // gradient paint-server references. Each frame gets a fresh entity per
  // gradient; the registry is cleared on `kBeginFrame` so stale entities
  // don't pile up across long runs. The registry is intentionally
  // heap-allocated behind an opaque `impl_` pointer to keep the public
  // header free of the entt include.
  struct Impl;
  std::unique_ptr<Impl> impl_;

  svg::RendererInterface& target_;
};

}  // namespace donner::editor::sandbox
