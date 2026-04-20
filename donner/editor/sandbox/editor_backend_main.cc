/// @file
///
/// `donner_editor_backend` — long-lived sandbox child for the Donner editor.
///
/// This is the persistent child process that `SandboxSession` manages. It reads
/// session-framed requests from stdin, dispatches them via `EditorBackendCore`,
/// renders via `SerializingRenderer`, and writes responses to stdout.
/// See docs/design_docs/0023-editor_sandbox.md §S8 for the protocol.

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/editor/sandbox/EditorBackendCore.h"
#include "donner/editor/sandbox/SandboxHardening.h"
#include "donner/editor/sandbox/SessionCodec.h"
#include "donner/editor/sandbox/SessionProtocol.h"

namespace {

using donner::editor::sandbox::DecodeApplySourcePatch;
using donner::editor::sandbox::DecodeExport;
using donner::editor::sandbox::DecodeKeyEvent;
using donner::editor::sandbox::DecodeLoadBytes;
using donner::editor::sandbox::DecodePointerEvent;
using donner::editor::sandbox::DecodeReplaceSource;
using donner::editor::sandbox::DecodeSetTool;
using donner::editor::sandbox::DecodeSetViewport;
using donner::editor::sandbox::DecodeWheelEvent;
using donner::editor::sandbox::EditorBackendCore;
using donner::editor::sandbox::EncodeError;
using donner::editor::sandbox::EncodeExportResponse;
using donner::editor::sandbox::EncodeHandshakeAck;
using donner::editor::sandbox::EncodeShutdownAck;
using donner::editor::sandbox::ErrorPayload;
using donner::editor::sandbox::FramePayload;
using donner::editor::sandbox::HandshakeAckPayload;
using donner::editor::sandbox::HardeningOptions;
using donner::editor::sandbox::HardeningStatus;
using donner::editor::sandbox::kSessionProtocolVersion;
using donner::editor::sandbox::ReadNextFrame;
using donner::editor::sandbox::SessionErrorKind;
using donner::editor::sandbox::SessionFrame;
using donner::editor::sandbox::SessionOpcode;
using donner::editor::sandbox::WriteFrame;

/// Writes a response frame to stdout.
bool Respond(const SessionFrame& frame) {
  std::string err;
  if (!WriteFrame(STDOUT_FILENO, frame, err)) {
    std::fprintf(stderr, "editor_backend: write error: %s\n", err.c_str());
    return false;
  }
  return true;
}

/// Sends a kFrame response for the given FramePayload.
bool RespondFrame(const FramePayload& framePayload, uint64_t requestId) {
  std::vector<uint8_t> payload = donner::editor::sandbox::EncodeFrame(framePayload);

  SessionFrame response;
  response.requestId = requestId;
  response.opcode = SessionOpcode::kFrame;
  response.payload = std::move(payload);
  return Respond(response);
}

/// Sends a kError response.
bool RespondError(uint64_t requestId, SessionErrorKind kind, std::string_view message = {}) {
  ErrorPayload err;
  err.errorKind = kind;
  err.message = std::string(message);
  std::vector<uint8_t> payload = EncodeError(err);

  SessionFrame response;
  response.requestId = requestId;
  response.opcode = SessionOpcode::kError;
  response.payload = std::move(payload);
  return Respond(response);
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  // Check for crash-on-handshake testing env var.
  if (const char* crashEnv = std::getenv("DONNER_BACKEND_CRASH_ON_HANDSHAKE")) {
    if (std::strcmp(crashEnv, "1") == 0) {
      std::abort();
    }
  }

  // Apply sandbox hardening.
  donner::editor::sandbox::HardeningOptions opts;
  auto result = donner::editor::sandbox::ApplyHardening(opts);
  if (result.status != HardeningStatus::kOk) {
    std::fprintf(stderr, "editor_backend: hardening failed: %s\n", result.message.c_str());
    return 1;
  }

  EditorBackendCore core;

  for (;;) {
    SessionFrame request;
    std::string err;

    if (!ReadNextFrame(STDIN_FILENO, request, err)) {
      break;
    }

    switch (request.opcode) {
      case SessionOpcode::kHandshake: {
        HandshakeAckPayload ack;
        ack.protocolVersion = kSessionProtocolVersion;
        ack.pid = static_cast<uint64_t>(::getpid());

        SessionFrame response;
        response.requestId = request.requestId;
        response.opcode = SessionOpcode::kHandshakeAck;
        response.payload = EncodeHandshakeAck(ack);
        if (!Respond(response)) return 1;
        break;
      }

      case SessionOpcode::kShutdown: {
        SessionFrame response;
        response.requestId = request.requestId;
        response.opcode = SessionOpcode::kShutdownAck;
        response.payload = EncodeShutdownAck();
        if (!Respond(response)) return 1;
        return 0;
      }

      case SessionOpcode::kSetViewport: {
        donner::editor::sandbox::SetViewportPayload vp;
        if (!DecodeSetViewport(request.payload, vp)) {
          if (!RespondError(request.requestId, SessionErrorKind::kPayloadMalformed)) return 1;
          break;
        }
        if (!RespondFrame(core.handleSetViewport(vp), request.requestId)) return 1;
        break;
      }

      case SessionOpcode::kLoadBytes: {
        donner::editor::sandbox::LoadBytesPayload load;
        if (!DecodeLoadBytes(request.payload, load)) {
          if (!RespondError(request.requestId, SessionErrorKind::kPayloadMalformed)) return 1;
          break;
        }
        if (!RespondFrame(core.handleLoadBytes(load), request.requestId)) return 1;
        break;
      }

      case SessionOpcode::kReplaceSource: {
        donner::editor::sandbox::ReplaceSourcePayload rep;
        if (!DecodeReplaceSource(request.payload, rep)) {
          if (!RespondError(request.requestId, SessionErrorKind::kPayloadMalformed)) return 1;
          break;
        }
        if (!RespondFrame(core.handleReplaceSource(rep), request.requestId)) return 1;
        break;
      }

      case SessionOpcode::kApplySourcePatch: {
        donner::editor::sandbox::ApplySourcePatchPayload patch;
        if (!DecodeApplySourcePatch(request.payload, patch)) {
          if (!RespondError(request.requestId, SessionErrorKind::kPayloadMalformed)) return 1;
          break;
        }
        if (!RespondFrame(core.handleApplySourcePatch(patch), request.requestId)) return 1;
        break;
      }

      case SessionOpcode::kPointerEvent: {
        donner::editor::sandbox::PointerEventPayload ptr;
        if (!DecodePointerEvent(request.payload, ptr)) {
          if (!RespondError(request.requestId, SessionErrorKind::kPayloadMalformed)) return 1;
          break;
        }
        if (!RespondFrame(core.handlePointerEvent(ptr), request.requestId)) return 1;
        break;
      }

      case SessionOpcode::kKeyEvent: {
        donner::editor::sandbox::KeyEventPayload key;
        if (!DecodeKeyEvent(request.payload, key)) {
          if (!RespondError(request.requestId, SessionErrorKind::kPayloadMalformed)) return 1;
          break;
        }
        if (!RespondFrame(core.handleKeyEvent(key), request.requestId)) return 1;
        break;
      }

      case SessionOpcode::kWheelEvent: {
        donner::editor::sandbox::WheelEventPayload wheel;
        if (!DecodeWheelEvent(request.payload, wheel)) {
          if (!RespondError(request.requestId, SessionErrorKind::kPayloadMalformed)) return 1;
          break;
        }
        if (!RespondFrame(core.handleWheelEvent(wheel), request.requestId)) return 1;
        break;
      }

      case SessionOpcode::kSetTool: {
        donner::editor::sandbox::SetToolPayload tool;
        if (!DecodeSetTool(request.payload, tool)) {
          if (!RespondError(request.requestId, SessionErrorKind::kPayloadMalformed)) return 1;
          break;
        }
        if (!RespondFrame(core.handleSetTool(tool), request.requestId)) return 1;
        break;
      }

      case SessionOpcode::kUndo:
        if (!RespondFrame(core.handleUndo(), request.requestId)) return 1;
        break;

      case SessionOpcode::kRedo:
        if (!RespondFrame(core.handleRedo(), request.requestId)) return 1;
        break;

      case SessionOpcode::kExport: {
        donner::editor::sandbox::ExportRequestPayload exportReq;
        if (!DecodeExport(request.payload, exportReq)) {
          if (!RespondError(request.requestId, SessionErrorKind::kPayloadMalformed)) return 1;
          break;
        }
        auto exportResp = core.handleExport(exportReq);
        SessionFrame response;
        response.requestId = request.requestId;
        response.opcode = SessionOpcode::kExportResponse;
        response.payload = EncodeExportResponse(exportResp);
        if (!Respond(response)) return 1;
        break;
      }

      default:
        if (!RespondError(request.requestId, SessionErrorKind::kUnknownOpcode)) return 1;
        break;
    }
  }

  return 0;
}
