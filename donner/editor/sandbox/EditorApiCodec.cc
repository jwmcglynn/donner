/// @file
///
/// Per-opcode payload encode/decode implementation for the editor session
/// protocol (§S8). Uses `WireWriter` / `WireReader` from `Wire.h` for all
/// serialization, with bounds-checking on every variable-length field.

#include "donner/editor/sandbox/EditorApiCodec.h"

#include <cstring>

#include "donner/editor/sandbox/SessionProtocol.h"
#include "donner/editor/sandbox/Wire.h"

namespace donner::editor::sandbox {

namespace {

/// Max enum value for validation.
constexpr uint32_t kMaxPointerPhase = static_cast<uint32_t>(PointerPhase::kCancel);
constexpr uint32_t kMaxKeyPhase = static_cast<uint32_t>(KeyPhase::kChar);
constexpr uint32_t kMaxToolKind = static_cast<uint32_t>(ToolKind::kText);
constexpr uint32_t kMaxExportFormat = static_cast<uint32_t>(ExportFormat::kPng);
constexpr uint32_t kMaxToastSeverity = static_cast<uint32_t>(ToastSeverity::kError);
constexpr uint32_t kMaxWritebackReason = static_cast<uint32_t>(WritebackReason::kElementRemoval);
constexpr uint32_t kMaxSessionErrorKind = static_cast<uint32_t>(SessionErrorKind::kInternalError);

/// Cap on selection count and writeback count per frame.
constexpr uint32_t kMaxSelectionCount = 100'000;
constexpr uint32_t kMaxWritebackCount = 100'000;
constexpr uint32_t kMaxDiagnosticCount = 100'000;
constexpr uint32_t kMaxTreeNodeCount = 500'000;

void WriteFrameBitmap(WireWriter& w, const FrameBitmapPayload& bitmap) {
  w.writeI32(bitmap.width);
  w.writeI32(bitmap.height);
  w.writeU32(bitmap.rowBytes);
  w.writeU8(bitmap.alphaType);
  w.writeU32(static_cast<uint32_t>(bitmap.pixels.size()));
  w.writeBytes(bitmap.pixels);
}

bool ReadFrameBitmap(WireReader& r, FrameBitmapPayload& bitmap) {
  if (!r.readI32(bitmap.width)) return false;
  if (!r.readI32(bitmap.height)) return false;
  if (!r.readU32(bitmap.rowBytes)) return false;
  if (!r.readU8(bitmap.alphaType)) return false;
  uint32_t pixelCount = 0;
  if (!r.readCount(pixelCount, kMaxFrameBytes)) return false;
  bitmap.pixels.resize(pixelCount);
  if (pixelCount > 0) {
    if (!r.readBytes(bitmap.pixels)) return false;
  }
  return true;
}

}  // namespace

// ===========================================================================
// Request encoders
// ===========================================================================

std::vector<uint8_t> EncodeHandshake(const HandshakePayload& payload) {
  WireWriter w;
  w.writeU32(payload.protocolVersion);
  w.writeString(payload.buildId);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeShutdown() {
  return {};
}

std::vector<uint8_t> EncodeSetViewport(const SetViewportPayload& payload) {
  WireWriter w;
  w.writeI32(payload.width);
  w.writeI32(payload.height);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeLoadBytes(const LoadBytesPayload& payload) {
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(payload.bytes.size()));
  w.writeBytes({reinterpret_cast<const uint8_t*>(payload.bytes.data()), payload.bytes.size()});
  w.writeBool(payload.originUri.has_value());
  if (payload.originUri.has_value()) {
    w.writeString(*payload.originUri);
  }
  return std::move(w).take();
}

std::vector<uint8_t> EncodeReplaceSource(const ReplaceSourcePayload& payload) {
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(payload.bytes.size()));
  w.writeBytes({reinterpret_cast<const uint8_t*>(payload.bytes.data()), payload.bytes.size()});
  w.writeBool(payload.preserveUndoOnReparse);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeApplySourcePatch(const ApplySourcePatchPayload& payload) {
  WireWriter w;
  w.writeU32(payload.start);
  w.writeU32(payload.end);
  w.writeString(payload.newText);
  return std::move(w).take();
}

std::vector<uint8_t> EncodePointerEvent(const PointerEventPayload& payload) {
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(payload.phase));
  w.writeF64(payload.documentX);
  w.writeF64(payload.documentY);
  w.writeU32(payload.buttons);
  w.writeU32(payload.modifiers);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeKeyEvent(const KeyEventPayload& payload) {
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(payload.phase));
  w.writeI32(payload.keyCode);
  w.writeU32(payload.modifiers);
  w.writeString(payload.textInput);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeWheelEvent(const WheelEventPayload& payload) {
  WireWriter w;
  w.writeF64(payload.documentX);
  w.writeF64(payload.documentY);
  w.writeF64(payload.deltaX);
  w.writeF64(payload.deltaY);
  w.writeU32(payload.modifiers);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeSetTool(const SetToolPayload& payload) {
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(payload.toolKind));
  return std::move(w).take();
}

std::vector<uint8_t> EncodeSelectElement(const SelectElementPayload& payload) {
  WireWriter w;
  w.writeU64(payload.entityId);
  w.writeU64(payload.entityGeneration);
  w.writeU8(payload.mode);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeUndo() {
  return {};
}

std::vector<uint8_t> EncodeRedo() {
  return {};
}

std::vector<uint8_t> EncodeExport(const ExportRequestPayload& payload) {
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(payload.format));
  return std::move(w).take();
}

std::vector<uint8_t> EncodeAttachSharedTexture(const AttachSharedTexturePayload& payload) {
  WireWriter w;
  w.writeU8(payload.kind);
  w.writeU64(payload.handle);
  w.writeI32(payload.width);
  w.writeI32(payload.height);
  w.writeU32(payload.rowBytes);
  return std::move(w).take();
}

// ===========================================================================
// Response encoders
// ===========================================================================

std::vector<uint8_t> EncodeHandshakeAck(const HandshakeAckPayload& payload) {
  WireWriter w;
  w.writeU32(payload.protocolVersion);
  w.writeU64(payload.pid);
  w.writeString(payload.capabilities);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeShutdownAck() {
  return {};
}

std::vector<uint8_t> EncodeFrame(const FramePayload& payload) {
  WireWriter w;
  w.writeU64(payload.frameId);

  // selections
  w.writeU32(static_cast<uint32_t>(payload.selections.size()));
  for (const auto& sel : payload.selections) {
    for (int i = 0; i < 4; ++i) {
      w.writeF64(sel.bbox[i]);
    }
    w.writeBool(sel.hasTransform);
    if (sel.hasTransform) {
      for (int i = 0; i < 6; ++i) {
        w.writeF64(sel.transform[i]);
      }
    }
    w.writeU32(sel.handleMask);
  }

  // marquee
  w.writeBool(payload.hasMarquee);
  if (payload.hasMarquee) {
    for (int i = 0; i < 4; ++i) {
      w.writeF64(payload.marquee[i]);
    }
  }

  // hoverRect
  w.writeBool(payload.hasHoverRect);
  if (payload.hasHoverRect) {
    for (int i = 0; i < 4; ++i) {
      w.writeF64(payload.hoverRect[i]);
    }
  }

  // writebacks
  w.writeU32(static_cast<uint32_t>(payload.writebacks.size()));
  for (const auto& wb : payload.writebacks) {
    w.writeU32(wb.start);
    w.writeU32(wb.end);
    w.writeString(wb.newText);
    w.writeU8(static_cast<uint8_t>(wb.reason));
  }

  // sourceReplaceAll
  w.writeBool(payload.hasSourceReplaceAll);
  if (payload.hasSourceReplaceAll) {
    w.writeString(payload.sourceReplaceAll);
  }

  // status
  w.writeU8(static_cast<uint8_t>(payload.statusKind));
  w.writeString(payload.statusMessage);

  // diagnostics
  w.writeU32(static_cast<uint32_t>(payload.diagnostics.size()));
  for (const auto& diag : payload.diagnostics) {
    w.writeU32(diag.line);
    w.writeU32(diag.column);
    w.writeString(diag.message);
  }

  // cursorHint
  w.writeBool(payload.hasCursorHint);
  if (payload.hasCursorHint) {
    w.writeU32(payload.cursorHintSourceOffset);
  }

  // tree summary
  w.writeU64(payload.tree.generation);
  w.writeU32(payload.tree.rootIndex);
  w.writeU32(static_cast<uint32_t>(payload.tree.nodes.size()));
  for (const auto& node : payload.tree.nodes) {
    w.writeU64(node.entityId);
    w.writeU64(node.entityGeneration);
    w.writeU32(node.parentIndex);
    w.writeU32(node.depth);
    w.writeString(node.tagName);
    w.writeString(node.idAttr);
    w.writeString(node.displayName);
    w.writeU32(node.sourceStart);
    w.writeU32(node.sourceEnd);
    w.writeBool(node.selected);
  }

  // documentViewBox (added after tree summary so older decoders that stop
  // at tree summary don't crash — they'll just see a trailing blob).
  w.writeBool(payload.hasDocumentViewBox);
  if (payload.hasDocumentViewBox) {
    for (int i = 0; i < 4; ++i) {
      w.writeF64(payload.documentViewBox[i]);
    }
  }

  // finalBitmap — appended after documentViewBox for the same back-compat
  // reason. Skip entirely when absent (the flag byte is 0 → 1 byte of
  // overhead per frame; fine).
  w.writeBool(payload.hasFinalBitmap);
  if (payload.hasFinalBitmap) {
    WriteFrameBitmap(w, FrameBitmapPayload{
                            .width = payload.finalBitmapWidth,
                            .height = payload.finalBitmapHeight,
                            .rowBytes = payload.finalBitmapRowBytes,
                            .alphaType = payload.finalBitmapAlphaType,
                            .pixels = payload.finalBitmapPixels,
                        });
  }

  // compositedPreview — optional trailing block. Carries bg/promoted/fg
  // textures plus a selection-overlay texture for drag preview.
  w.writeBool(payload.hasCompositedPreview);
  if (payload.hasCompositedPreview) {
    w.writeBool(payload.compositedPreviewActive);
    w.writeBool(payload.hasCompositedPreviewBitmaps);
    w.writeF64(payload.compositedPreviewTranslationDoc[0]);
    w.writeF64(payload.compositedPreviewTranslationDoc[1]);
    if (payload.hasCompositedPreviewBitmaps) {
      WriteFrameBitmap(w, payload.compositedPreviewBackground);
      WriteFrameBitmap(w, payload.compositedPreviewPromoted);
      WriteFrameBitmap(w, payload.compositedPreviewForeground);
      WriteFrameBitmap(w, payload.compositedPreviewOverlay);
    }
  }

  // Inspector snapshot — optional trailing block (G4 in §S12).
  w.writeBool(payload.hasInspectedElement);
  if (payload.hasInspectedElement) {
    const auto& insp = payload.inspectedElement;
    w.writeU64(insp.entityId);
    w.writeU64(insp.entityGeneration);
    w.writeString(insp.tagName);
    w.writeString(insp.idAttr);
    w.writeString(insp.className);
    w.writeU32(static_cast<uint32_t>(insp.xmlAttributes.size()));
    for (const auto& attr : insp.xmlAttributes) {
      w.writeString(attr.name);
      w.writeString(attr.value);
    }
    w.writeU32(static_cast<uint32_t>(insp.computedStyle.size()));
    for (const auto& attr : insp.computedStyle) {
      w.writeString(attr.name);
      w.writeString(attr.value);
    }
  }

  return std::move(w).take();
}

std::vector<uint8_t> EncodeExportResponse(const ExportResponsePayload& payload) {
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(payload.format));
  w.writeU32(static_cast<uint32_t>(payload.bytes.size()));
  w.writeBytes(payload.bytes);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeSourceReplaceAll(const SourceReplaceAllPayload& payload) {
  WireWriter w;
  w.writeString(payload.bytes);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeToast(const ToastResponsePayload& payload) {
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(payload.severity));
  w.writeString(payload.message);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeDialogRequest(const DialogRequestPayload& payload) {
  WireWriter w;
  w.writeU32(payload.kind);
  w.writeString(payload.fileName);
  w.writeString(payload.message);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeDiagnostic(const DiagnosticPayload& payload) {
  WireWriter w;
  w.writeString(payload.message);
  return std::move(w).take();
}

std::vector<uint8_t> EncodeError(const ErrorPayload& payload) {
  WireWriter w;
  w.writeU32(static_cast<uint32_t>(payload.errorKind));
  w.writeString(payload.message);
  return std::move(w).take();
}

// ===========================================================================
// Request decoders
// ===========================================================================

bool DecodeHandshake(std::span<const uint8_t> data, HandshakePayload& out) {
  WireReader r(data);
  if (!r.readU32(out.protocolVersion)) return false;
  if (!r.readString(out.buildId)) return false;
  return !r.failed();
}

bool DecodeShutdown(std::span<const uint8_t> /*data*/) {
  return true;
}

bool DecodeSetViewport(std::span<const uint8_t> data, SetViewportPayload& out) {
  WireReader r(data);
  if (!r.readI32(out.width)) return false;
  if (!r.readI32(out.height)) return false;
  return !r.failed();
}

bool DecodeLoadBytes(std::span<const uint8_t> data, LoadBytesPayload& out) {
  WireReader r(data);
  uint32_t bytesLen = 0;
  if (!r.readU32(bytesLen)) return false;
  if (bytesLen > kMaxStringBytes) {
    return false;
  }
  out.bytes.resize(bytesLen);
  if (bytesLen > 0) {
    if (!r.readBytes({reinterpret_cast<uint8_t*>(out.bytes.data()), bytesLen})) return false;
  }
  bool hasUri = false;
  if (!r.readBool(hasUri)) return false;
  if (hasUri) {
    std::string uri;
    if (!r.readString(uri)) return false;
    out.originUri = std::move(uri);
  } else {
    out.originUri.reset();
  }
  return !r.failed();
}

bool DecodeReplaceSource(std::span<const uint8_t> data, ReplaceSourcePayload& out) {
  WireReader r(data);
  uint32_t bytesLen = 0;
  if (!r.readU32(bytesLen)) return false;
  if (bytesLen > kMaxStringBytes) {
    return false;
  }
  out.bytes.resize(bytesLen);
  if (bytesLen > 0) {
    if (!r.readBytes({reinterpret_cast<uint8_t*>(out.bytes.data()), bytesLen})) return false;
  }
  if (!r.readBool(out.preserveUndoOnReparse)) return false;
  return !r.failed();
}

bool DecodeApplySourcePatch(std::span<const uint8_t> data, ApplySourcePatchPayload& out) {
  WireReader r(data);
  if (!r.readU32(out.start)) return false;
  if (!r.readU32(out.end)) return false;
  if (!r.readString(out.newText)) return false;
  return !r.failed();
}

bool DecodePointerEvent(std::span<const uint8_t> data, PointerEventPayload& out) {
  WireReader r(data);
  uint32_t phase = 0;
  if (!r.readU32(phase)) return false;
  if (phase > kMaxPointerPhase) return false;
  out.phase = static_cast<PointerPhase>(phase);
  if (!r.readF64(out.documentX)) return false;
  if (!r.readF64(out.documentY)) return false;
  if (!r.readU32(out.buttons)) return false;
  if (!r.readU32(out.modifiers)) return false;
  return !r.failed();
}

bool DecodeKeyEvent(std::span<const uint8_t> data, KeyEventPayload& out) {
  WireReader r(data);
  uint32_t phase = 0;
  if (!r.readU32(phase)) return false;
  if (phase > kMaxKeyPhase) return false;
  out.phase = static_cast<KeyPhase>(phase);
  if (!r.readI32(out.keyCode)) return false;
  if (!r.readU32(out.modifiers)) return false;
  if (!r.readString(out.textInput)) return false;
  return !r.failed();
}

bool DecodeWheelEvent(std::span<const uint8_t> data, WheelEventPayload& out) {
  WireReader r(data);
  if (!r.readF64(out.documentX)) return false;
  if (!r.readF64(out.documentY)) return false;
  if (!r.readF64(out.deltaX)) return false;
  if (!r.readF64(out.deltaY)) return false;
  if (!r.readU32(out.modifiers)) return false;
  return !r.failed();
}

bool DecodeSetTool(std::span<const uint8_t> data, SetToolPayload& out) {
  WireReader r(data);
  uint32_t kind = 0;
  if (!r.readU32(kind)) return false;
  if (kind > kMaxToolKind) return false;
  out.toolKind = static_cast<ToolKind>(kind);
  return !r.failed();
}

bool DecodeSelectElement(std::span<const uint8_t> data, SelectElementPayload& out) {
  WireReader r(data);
  if (!r.readU64(out.entityId)) return false;
  if (!r.readU64(out.entityGeneration)) return false;
  if (!r.readU8(out.mode)) return false;
  if (out.mode > 2) return false;
  return !r.failed();
}

bool DecodeUndo(std::span<const uint8_t> /*data*/) {
  return true;
}

bool DecodeRedo(std::span<const uint8_t> /*data*/) {
  return true;
}

bool DecodeExport(std::span<const uint8_t> data, ExportRequestPayload& out) {
  WireReader r(data);
  uint32_t format = 0;
  if (!r.readU32(format)) return false;
  if (format > kMaxExportFormat) return false;
  out.format = static_cast<ExportFormat>(format);
  return !r.failed();
}

bool DecodeAttachSharedTexture(std::span<const uint8_t> data, AttachSharedTexturePayload& out) {
  WireReader r(data);
  if (!r.readU8(out.kind)) return false;
  if (!r.readU64(out.handle)) return false;
  if (!r.readI32(out.width)) return false;
  if (!r.readI32(out.height)) return false;
  if (!r.readU32(out.rowBytes)) return false;
  return !r.failed();
}

// ===========================================================================
// Response decoders
// ===========================================================================

bool DecodeHandshakeAck(std::span<const uint8_t> data, HandshakeAckPayload& out) {
  WireReader r(data);
  if (!r.readU32(out.protocolVersion)) return false;
  if (!r.readU64(out.pid)) return false;
  if (!r.readString(out.capabilities)) return false;
  return !r.failed();
}

bool DecodeShutdownAck(std::span<const uint8_t> /*data*/) {
  return true;
}

bool DecodeFrame(std::span<const uint8_t> data, FramePayload& out) {
  WireReader r(data);
  if (!r.readU64(out.frameId)) return false;

  // selections
  uint32_t selCount = 0;
  if (!r.readCount(selCount, kMaxSelectionCount)) return false;
  out.selections.resize(selCount);
  for (uint32_t i = 0; i < selCount; ++i) {
    auto& sel = out.selections[i];
    for (int j = 0; j < 4; ++j) {
      if (!r.readF64(sel.bbox[j])) return false;
    }
    if (!r.readBool(sel.hasTransform)) return false;
    if (sel.hasTransform) {
      for (int j = 0; j < 6; ++j) {
        if (!r.readF64(sel.transform[j])) return false;
      }
    }
    if (!r.readU32(sel.handleMask)) return false;
  }

  // marquee
  if (!r.readBool(out.hasMarquee)) return false;
  if (out.hasMarquee) {
    for (int i = 0; i < 4; ++i) {
      if (!r.readF64(out.marquee[i])) return false;
    }
  }

  // hoverRect
  if (!r.readBool(out.hasHoverRect)) return false;
  if (out.hasHoverRect) {
    for (int i = 0; i < 4; ++i) {
      if (!r.readF64(out.hoverRect[i])) return false;
    }
  }

  // writebacks
  uint32_t wbCount = 0;
  if (!r.readCount(wbCount, kMaxWritebackCount)) return false;
  out.writebacks.resize(wbCount);
  for (uint32_t i = 0; i < wbCount; ++i) {
    auto& wb = out.writebacks[i];
    if (!r.readU32(wb.start)) return false;
    if (!r.readU32(wb.end)) return false;
    if (!r.readString(wb.newText)) return false;
    uint8_t reason = 0;
    if (!r.readU8(reason)) return false;
    if (reason > kMaxWritebackReason) return false;
    wb.reason = static_cast<WritebackReason>(reason);
  }

  // sourceReplaceAll
  if (!r.readBool(out.hasSourceReplaceAll)) return false;
  if (out.hasSourceReplaceAll) {
    if (!r.readString(out.sourceReplaceAll)) return false;
  }

  // status
  uint8_t statusKind = 0;
  if (!r.readU8(statusKind)) return false;
  out.statusKind = static_cast<FrameStatusKind>(statusKind);
  if (!r.readString(out.statusMessage)) return false;

  // diagnostics
  uint32_t diagCount = 0;
  if (!r.readCount(diagCount, kMaxDiagnosticCount)) return false;
  out.diagnostics.resize(diagCount);
  for (uint32_t i = 0; i < diagCount; ++i) {
    auto& diag = out.diagnostics[i];
    if (!r.readU32(diag.line)) return false;
    if (!r.readU32(diag.column)) return false;
    if (!r.readString(diag.message)) return false;
  }

  // cursorHint
  if (!r.readBool(out.hasCursorHint)) return false;
  if (out.hasCursorHint) {
    if (!r.readU32(out.cursorHintSourceOffset)) return false;
  }

  // tree summary
  if (!r.readU64(out.tree.generation)) return false;
  if (!r.readU32(out.tree.rootIndex)) return false;
  uint32_t treeNodeCount = 0;
  if (!r.readCount(treeNodeCount, kMaxTreeNodeCount)) return false;
  out.tree.nodes.resize(treeNodeCount);
  for (uint32_t i = 0; i < treeNodeCount; ++i) {
    auto& node = out.tree.nodes[i];
    if (!r.readU64(node.entityId)) return false;
    if (!r.readU64(node.entityGeneration)) return false;
    if (!r.readU32(node.parentIndex)) return false;
    if (!r.readU32(node.depth)) return false;
    if (!r.readString(node.tagName)) return false;
    if (!r.readString(node.idAttr)) return false;
    if (!r.readString(node.displayName)) return false;
    if (!r.readU32(node.sourceStart)) return false;
    if (!r.readU32(node.sourceEnd)) return false;
    if (!r.readBool(node.selected)) return false;
  }

  // documentViewBox — optional trailing block. Old encoders (fuzzer
  // corpus, archived captures) stop at the tree summary; skip the
  // field cleanly in that case rather than failing the whole decode.
  if (r.remaining() == 0) {
    out.hasDocumentViewBox = false;
    return !r.failed();
  }
  if (!r.readBool(out.hasDocumentViewBox)) return false;
  if (out.hasDocumentViewBox) {
    for (int i = 0; i < 4; ++i) {
      if (!r.readF64(out.documentViewBox[i])) return false;
    }
  }

  // finalBitmap — optional trailing block (may not be present in
  // pre-composited-rendering captures).
  if (r.remaining() == 0) {
    out.hasFinalBitmap = false;
    return !r.failed();
  }
  if (!r.readBool(out.hasFinalBitmap)) return false;
  if (out.hasFinalBitmap) {
    FrameBitmapPayload finalBitmap;
    if (!ReadFrameBitmap(r, finalBitmap)) return false;
    out.finalBitmapWidth = finalBitmap.width;
    out.finalBitmapHeight = finalBitmap.height;
    out.finalBitmapRowBytes = finalBitmap.rowBytes;
    out.finalBitmapAlphaType = finalBitmap.alphaType;
    out.finalBitmapPixels = std::move(finalBitmap.pixels);
  }

  // compositedPreview — optional trailing block.
  if (r.remaining() == 0) {
    out.hasCompositedPreview = false;
    return !r.failed();
  }
  if (!r.readBool(out.hasCompositedPreview)) return false;
  if (out.hasCompositedPreview) {
    if (!r.readBool(out.compositedPreviewActive)) return false;
    if (!r.readBool(out.hasCompositedPreviewBitmaps)) return false;
    if (!r.readF64(out.compositedPreviewTranslationDoc[0])) return false;
    if (!r.readF64(out.compositedPreviewTranslationDoc[1])) return false;
    if (out.hasCompositedPreviewBitmaps) {
      if (!ReadFrameBitmap(r, out.compositedPreviewBackground)) return false;
      if (!ReadFrameBitmap(r, out.compositedPreviewPromoted)) return false;
      if (!ReadFrameBitmap(r, out.compositedPreviewForeground)) return false;
      if (!ReadFrameBitmap(r, out.compositedPreviewOverlay)) return false;
    }
  }

  // Inspector snapshot — optional trailing block.
  if (r.remaining() == 0) {
    out.hasInspectedElement = false;
    return !r.failed();
  }
  if (!r.readBool(out.hasInspectedElement)) return false;
  if (out.hasInspectedElement) {
    auto& insp = out.inspectedElement;
    if (!r.readU64(insp.entityId)) return false;
    if (!r.readU64(insp.entityGeneration)) return false;
    if (!r.readString(insp.tagName)) return false;
    if (!r.readString(insp.idAttr)) return false;
    if (!r.readString(insp.className)) return false;
    uint32_t xmlCount = 0;
    // Cap at the same "max selections" ballpark; a single element
    // with thousands of attributes would be pathological but non-fatal
    // — reject to keep the inspector table responsive.
    constexpr uint32_t kMaxAttributes = 4096;
    if (!r.readCount(xmlCount, kMaxAttributes)) return false;
    insp.xmlAttributes.resize(xmlCount);
    for (auto& attr : insp.xmlAttributes) {
      if (!r.readString(attr.name)) return false;
      if (!r.readString(attr.value)) return false;
    }
    uint32_t styleCount = 0;
    if (!r.readCount(styleCount, kMaxAttributes)) return false;
    insp.computedStyle.resize(styleCount);
    for (auto& attr : insp.computedStyle) {
      if (!r.readString(attr.name)) return false;
      if (!r.readString(attr.value)) return false;
    }
  }

  return !r.failed();
}

bool DecodeExportResponse(std::span<const uint8_t> data, ExportResponsePayload& out) {
  WireReader r(data);
  uint32_t format = 0;
  if (!r.readU32(format)) return false;
  if (format > kMaxExportFormat) return false;
  out.format = static_cast<ExportFormat>(format);
  uint32_t bytesLen = 0;
  if (!r.readCount(bytesLen, kMaxFrameBytes)) return false;
  out.bytes.resize(bytesLen);
  if (bytesLen > 0) {
    if (!r.readBytes(out.bytes)) return false;
  }
  return !r.failed();
}

bool DecodeSourceReplaceAll(std::span<const uint8_t> data, SourceReplaceAllPayload& out) {
  WireReader r(data);
  if (!r.readString(out.bytes)) return false;
  return !r.failed();
}

bool DecodeToast(std::span<const uint8_t> data, ToastResponsePayload& out) {
  WireReader r(data);
  uint32_t severity = 0;
  if (!r.readU32(severity)) return false;
  if (severity > kMaxToastSeverity) return false;
  out.severity = static_cast<ToastSeverity>(severity);
  if (!r.readString(out.message)) return false;
  return !r.failed();
}

bool DecodeDialogRequest(std::span<const uint8_t> data, DialogRequestPayload& out) {
  WireReader r(data);
  if (!r.readU32(out.kind)) return false;
  if (!r.readString(out.fileName)) return false;
  if (!r.readString(out.message)) return false;
  return !r.failed();
}

bool DecodeDiagnostic(std::span<const uint8_t> data, DiagnosticPayload& out) {
  WireReader r(data);
  if (!r.readString(out.message)) return false;
  return !r.failed();
}

bool DecodeError(std::span<const uint8_t> data, ErrorPayload& out) {
  WireReader r(data);
  uint32_t kind = 0;
  if (!r.readU32(kind)) return false;
  if (kind > kMaxSessionErrorKind) return false;
  out.errorKind = static_cast<SessionErrorKind>(kind);
  if (!r.readString(out.message)) return false;
  return !r.failed();
}

}  // namespace donner::editor::sandbox
