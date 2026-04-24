/// @file FrameResponse_fuzzer.cc
///
/// LibFuzzer target for the backend→host response path. The editor host
/// deserializes every frame the backend sends, so a malformed
/// `FramePayload` (bitmap dims, selection count, tree entries, etc.)
/// or a malformed error/toast/dialog payload could crash the editor —
/// that would turn a backend bug into a host DOS vector. Fuzz all
/// response decoders to make sure they reject gracefully rather than
/// crashing.

#include <cstddef>
#include <cstdint>
#include <span>

#include "donner/editor/sandbox/EditorApiCodec.h"

namespace donner::editor::sandbox {

namespace {

void FuzzResponsePayload(uint8_t selector, std::span<const uint8_t> payload) {
  switch (selector % 9) {
    case 0: {
      HandshakeAckPayload out;
      (void)DecodeHandshakeAck(payload, out);
      break;
    }
    case 1: (void)DecodeShutdownAck(payload); break;
    case 2: {
      FramePayload out;
      (void)DecodeFrame(payload, out);
      break;
    }
    case 3: {
      ExportResponsePayload out;
      (void)DecodeExportResponse(payload, out);
      break;
    }
    case 4: {
      SourceReplaceAllPayload out;
      (void)DecodeSourceReplaceAll(payload, out);
      break;
    }
    case 5: {
      ToastResponsePayload out;
      (void)DecodeToast(payload, out);
      break;
    }
    case 6: {
      DialogRequestPayload out;
      (void)DecodeDialogRequest(payload, out);
      break;
    }
    case 7: {
      DiagnosticPayload out;
      (void)DecodeDiagnostic(payload, out);
      break;
    }
    case 8: {
      ErrorPayload out;
      (void)DecodeError(payload, out);
      break;
    }
  }
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;
  const uint8_t selector = data[0];
  const std::span<const uint8_t> payload(data + 1, size - 1);
  FuzzResponsePayload(selector, payload);
  return 0;
}

}  // namespace donner::editor::sandbox
