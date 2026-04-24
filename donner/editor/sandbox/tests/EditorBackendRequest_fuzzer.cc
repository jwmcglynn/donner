/// @file EditorBackendRequest_fuzzer.cc
///
/// LibFuzzer target that validates the editor-backend request path never
/// crashes on adversarial input. Feeds arbitrary bytes into
/// `SessionCodec::DecodeFrame` + the full set of host→backend
/// `Decode*` payload helpers that the backend loop dispatches on.
///
/// If a malformed frame wire format or a malformed opcode-specific
/// payload could crash the backend, the backend child would die on
/// hostile input and the editor host would see every in-flight request
/// fail. Decoders returning `false` is the expected "malformed input"
/// signal — only crashes (asan, UBsan, libFuzzer-detected) are bugs.

#include <cstddef>
#include <cstdint>
#include <span>

#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/editor/sandbox/SessionCodec.h"
#include "donner/editor/sandbox/SessionProtocol.h"

namespace donner::editor::sandbox {

namespace {

/// Splits `{magic(1)}{payload}` — the first byte of each input picks which
/// request decoder to exercise, the rest is the adversarial payload.
/// libFuzzer is good at wiggling both independently, which is what we want.
void FuzzRequestPayload(uint8_t selector, std::span<const uint8_t> payload) {
  switch (selector % 15) {
    case 0: {
      HandshakePayload out;
      (void)DecodeHandshake(payload, out);
      break;
    }
    case 1:
      (void)DecodeShutdown(payload);
      break;
    case 2: {
      SetViewportPayload out;
      (void)DecodeSetViewport(payload, out);
      break;
    }
    case 3: {
      LoadBytesPayload out;
      (void)DecodeLoadBytes(payload, out);
      break;
    }
    case 4: {
      ReplaceSourcePayload out;
      (void)DecodeReplaceSource(payload, out);
      break;
    }
    case 5: {
      ApplySourcePatchPayload out;
      (void)DecodeApplySourcePatch(payload, out);
      break;
    }
    case 6: {
      PointerEventPayload out;
      (void)DecodePointerEvent(payload, out);
      break;
    }
    case 7: {
      KeyEventPayload out;
      (void)DecodeKeyEvent(payload, out);
      break;
    }
    case 8: {
      WheelEventPayload out;
      (void)DecodeWheelEvent(payload, out);
      break;
    }
    case 9: {
      SetToolPayload out;
      (void)DecodeSetTool(payload, out);
      break;
    }
    case 10: {
      SelectElementPayload out;
      (void)DecodeSelectElement(payload, out);
      break;
    }
    case 11:
      (void)DecodeUndo(payload);
      break;
    case 12:
      (void)DecodeRedo(payload);
      break;
    case 13: {
      ExportRequestPayload out;
      (void)DecodeExport(payload, out);
      break;
    }
    case 14: {
      AttachSharedTexturePayload out;
      (void)DecodeAttachSharedTexture(payload, out);
      break;
    }
  }
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) return 0;

  // Exercise 1: the session-level `DecodeFrame` which parses the
  // wire header and carves out the opcode payload.
  {
    SessionFrame frame;
    std::size_t consumed = 0;
    (void)DecodeFrame(std::span<const uint8_t>(data, size), frame, consumed);
  }

  // Exercise 2: the per-opcode payload decoders. First byte picks
  // which decoder is exercised, the rest of the buffer is the payload.
  const uint8_t selector = data[0];
  const std::span<const uint8_t> payload(data + 1, size - 1);
  FuzzRequestPayload(selector, payload);

  return 0;
}

}  // namespace donner::editor::sandbox
