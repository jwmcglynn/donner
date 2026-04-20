#pragma once
/// @file
///
/// Session-level framing codec for the long-lived `SandboxSession` IPC.
///
/// Encodes and decodes the session-layer framing defined in `SessionProtocol.h`:
///
/// ```
/// u32 magic         = 'DRNS' (0x534E5244)
/// u64 requestId
/// u32 opcode        (SessionOpcode)
/// u32 payloadLength
/// u8  payload[payloadLength]
/// ```
///
/// The codec is deliberately separated from the session transport so it can be
/// tested in isolation and reused by both the host (via `SandboxSession`) and
/// the backend binary (`editor_backend_main`).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "donner/editor/sandbox/SessionProtocol.h"

namespace donner::editor::sandbox {

/// Size of the session frame header: magic(4) + requestId(8) + opcode(4) + length(4).
inline constexpr std::size_t kSessionFrameHeaderSize = 4 + 8 + 4 + 4;

/// A decoded session frame.
struct SessionFrame {
  uint64_t requestId = 0;
  SessionOpcode opcode = SessionOpcode::kInvalid;
  std::vector<uint8_t> payload;
};

/// Encodes a `SessionFrame` into a self-contained byte vector (header + payload).
std::vector<uint8_t> EncodeFrame(const SessionFrame& frame);

/// Attempts to decode one frame from the front of `bytes`.
///
/// @param bytes Input buffer (may contain partial data or multiple frames).
/// @param[out] out Decoded frame on success.
/// @param[out] consumed Number of bytes consumed from `bytes` on success.
///   Set to 0 when more data is needed ("need more").
///   Set to `SIZE_MAX` on protocol error (wrong magic, payload too large).
/// @return `true` if a complete frame was decoded; `false` otherwise.
bool DecodeFrame(std::span<const uint8_t> bytes, SessionFrame& out, std::size_t& consumed);

/// Blocking fd helper used by the backend. Reads exactly one session frame
/// from `fd`, handling EINTR. Returns `false` on EOF or protocol error (with
/// `errorMessage` populated).
bool ReadNextFrame(int fd, SessionFrame& out, std::string& errorMessage);

/// Blocking fd helper used by the backend. Writes a complete session frame to
/// `fd`, handling EINTR. Returns `false` on write error.
bool WriteFrame(int fd, const SessionFrame& frame, std::string& errorMessage);

}  // namespace donner::editor::sandbox
