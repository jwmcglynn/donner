/// @file
///
/// Round-trip and malformed-input tests for EditorApiCodec.

#include "donner/editor/sandbox/EditorApiCodec.h"

#include <gtest/gtest.h>

#include <cstring>

#include "donner/editor/sandbox/SessionProtocol.h"

namespace donner::editor::sandbox {
namespace {

// ===========================================================================
// Handshake round-trip
// ===========================================================================

TEST(EditorApiCodecTest, HandshakeRoundTrip) {
  HandshakePayload in;
  in.protocolVersion = 42;
  in.buildId = "test-build-abc123";

  auto encoded = EncodeHandshake(in);
  ASSERT_FALSE(encoded.empty());

  HandshakePayload out;
  ASSERT_TRUE(DecodeHandshake(encoded, out));
  EXPECT_EQ(out.protocolVersion, 42u);
  EXPECT_EQ(out.buildId, "test-build-abc123");
}

TEST(EditorApiCodecTest, HandshakeAckRoundTrip) {
  HandshakeAckPayload in;
  in.protocolVersion = kSessionProtocolVersion;
  in.pid = 12345;
  in.capabilities = "render,text";

  auto encoded = EncodeHandshakeAck(in);
  HandshakeAckPayload out;
  ASSERT_TRUE(DecodeHandshakeAck(encoded, out));
  EXPECT_EQ(out.protocolVersion, kSessionProtocolVersion);
  EXPECT_EQ(out.pid, 12345u);
  EXPECT_EQ(out.capabilities, "render,text");
}

// ===========================================================================
// SetViewport round-trip
// ===========================================================================

TEST(EditorApiCodecTest, SetViewportRoundTrip) {
  SetViewportPayload in;
  in.width = 1920;
  in.height = 1080;

  auto encoded = EncodeSetViewport(in);
  SetViewportPayload out;
  ASSERT_TRUE(DecodeSetViewport(encoded, out));
  EXPECT_EQ(out.width, 1920);
  EXPECT_EQ(out.height, 1080);
}

// ===========================================================================
// LoadBytes round-trip
// ===========================================================================

TEST(EditorApiCodecTest, LoadBytesRoundTripWithUri) {
  LoadBytesPayload in;
  in.bytes = "<svg></svg>";
  in.originUri = "file:///tmp/test.svg";

  auto encoded = EncodeLoadBytes(in);
  LoadBytesPayload out;
  ASSERT_TRUE(DecodeLoadBytes(encoded, out));
  EXPECT_EQ(out.bytes, "<svg></svg>");
  ASSERT_TRUE(out.originUri.has_value());
  EXPECT_EQ(*out.originUri, "file:///tmp/test.svg");
}

TEST(EditorApiCodecTest, LoadBytesRoundTripWithoutUri) {
  LoadBytesPayload in;
  in.bytes = "<svg><rect/></svg>";
  in.originUri = std::nullopt;

  auto encoded = EncodeLoadBytes(in);
  LoadBytesPayload out;
  ASSERT_TRUE(DecodeLoadBytes(encoded, out));
  EXPECT_EQ(out.bytes, "<svg><rect/></svg>");
  EXPECT_FALSE(out.originUri.has_value());
}

// ===========================================================================
// ReplaceSource round-trip
// ===========================================================================

TEST(EditorApiCodecTest, ReplaceSourceRoundTrip) {
  ReplaceSourcePayload in;
  in.bytes = "<svg><circle r='10'/></svg>";
  in.preserveUndoOnReparse = true;

  auto encoded = EncodeReplaceSource(in);
  ReplaceSourcePayload out;
  ASSERT_TRUE(DecodeReplaceSource(encoded, out));
  EXPECT_EQ(out.bytes, in.bytes);
  EXPECT_TRUE(out.preserveUndoOnReparse);
}

// ===========================================================================
// ApplySourcePatch round-trip
// ===========================================================================

TEST(EditorApiCodecTest, ApplySourcePatchRoundTrip) {
  ApplySourcePatchPayload in;
  in.start = 10;
  in.end = 20;
  in.newText = "fill=\"red\"";

  auto encoded = EncodeApplySourcePatch(in);
  ApplySourcePatchPayload out;
  ASSERT_TRUE(DecodeApplySourcePatch(encoded, out));
  EXPECT_EQ(out.start, 10u);
  EXPECT_EQ(out.end, 20u);
  EXPECT_EQ(out.newText, "fill=\"red\"");
}

// ===========================================================================
// PointerEvent round-trip
// ===========================================================================

TEST(EditorApiCodecTest, PointerEventRoundTrip) {
  PointerEventPayload in;
  in.phase = PointerPhase::kDown;
  in.documentX = 123.456;
  in.documentY = 789.012;
  in.buttons = 1;
  in.modifiers = 2;

  auto encoded = EncodePointerEvent(in);
  PointerEventPayload out;
  ASSERT_TRUE(DecodePointerEvent(encoded, out));
  EXPECT_EQ(out.phase, PointerPhase::kDown);
  EXPECT_DOUBLE_EQ(out.documentX, 123.456);
  EXPECT_DOUBLE_EQ(out.documentY, 789.012);
  EXPECT_EQ(out.buttons, 1u);
  EXPECT_EQ(out.modifiers, 2u);
}

TEST(EditorApiCodecTest, PointerEventAllPhases) {
  for (uint32_t p = 0; p <= static_cast<uint32_t>(PointerPhase::kCancel); ++p) {
    PointerEventPayload in;
    in.phase = static_cast<PointerPhase>(p);
    in.documentX = static_cast<double>(p);
    in.documentY = static_cast<double>(p) + 1.0;

    auto encoded = EncodePointerEvent(in);
    PointerEventPayload out;
    ASSERT_TRUE(DecodePointerEvent(encoded, out)) << "phase=" << p;
    EXPECT_EQ(out.phase, in.phase);
  }
}

// ===========================================================================
// KeyEvent round-trip
// ===========================================================================

TEST(EditorApiCodecTest, KeyEventRoundTrip) {
  KeyEventPayload in;
  in.phase = KeyPhase::kChar;
  in.keyCode = 65;  // 'A'
  in.modifiers = 4;
  in.textInput = "A";

  auto encoded = EncodeKeyEvent(in);
  KeyEventPayload out;
  ASSERT_TRUE(DecodeKeyEvent(encoded, out));
  EXPECT_EQ(out.phase, KeyPhase::kChar);
  EXPECT_EQ(out.keyCode, 65);
  EXPECT_EQ(out.modifiers, 4u);
  EXPECT_EQ(out.textInput, "A");
}

// ===========================================================================
// WheelEvent round-trip
// ===========================================================================

TEST(EditorApiCodecTest, WheelEventRoundTrip) {
  WheelEventPayload in;
  in.documentX = 400.0;
  in.documentY = 300.0;
  in.deltaX = -1.5;
  in.deltaY = 3.0;
  in.modifiers = 1;

  auto encoded = EncodeWheelEvent(in);
  WheelEventPayload out;
  ASSERT_TRUE(DecodeWheelEvent(encoded, out));
  EXPECT_DOUBLE_EQ(out.documentX, 400.0);
  EXPECT_DOUBLE_EQ(out.documentY, 300.0);
  EXPECT_DOUBLE_EQ(out.deltaX, -1.5);
  EXPECT_DOUBLE_EQ(out.deltaY, 3.0);
  EXPECT_EQ(out.modifiers, 1u);
}

// ===========================================================================
// SetTool round-trip
// ===========================================================================

TEST(EditorApiCodecTest, SetToolRoundTrip) {
  SetToolPayload in;
  in.toolKind = ToolKind::kRect;

  auto encoded = EncodeSetTool(in);
  SetToolPayload out;
  ASSERT_TRUE(DecodeSetTool(encoded, out));
  EXPECT_EQ(out.toolKind, ToolKind::kRect);
}

// ===========================================================================
// Empty-payload opcodes
// ===========================================================================

TEST(EditorApiCodecTest, UndoRedoShutdownEmpty) {
  EXPECT_TRUE(EncodeUndo().empty());
  EXPECT_TRUE(EncodeRedo().empty());
  EXPECT_TRUE(EncodeShutdown().empty());
  EXPECT_TRUE(EncodeShutdownAck().empty());

  EXPECT_TRUE(DecodeUndo({}));
  EXPECT_TRUE(DecodeRedo({}));
  EXPECT_TRUE(DecodeShutdown({}));
  EXPECT_TRUE(DecodeShutdownAck({}));
}

// ===========================================================================
// Export round-trip
// ===========================================================================

TEST(EditorApiCodecTest, ExportRoundTrip) {
  ExportRequestPayload in;
  in.format = ExportFormat::kPng;

  auto encoded = EncodeExport(in);
  ExportRequestPayload out;
  ASSERT_TRUE(DecodeExport(encoded, out));
  EXPECT_EQ(out.format, ExportFormat::kPng);
}

TEST(EditorApiCodecTest, ExportResponseRoundTrip) {
  ExportResponsePayload in;
  in.format = ExportFormat::kSvgText;
  in.bytes = {0x3C, 0x73, 0x76, 0x67, 0x3E};  // "<svg>"

  auto encoded = EncodeExportResponse(in);
  ExportResponsePayload out;
  ASSERT_TRUE(DecodeExportResponse(encoded, out));
  EXPECT_EQ(out.format, ExportFormat::kSvgText);
  EXPECT_EQ(out.bytes, in.bytes);
}

// ===========================================================================
// Frame round-trip
// ===========================================================================

TEST(EditorApiCodecTest, FrameRoundTripMinimal) {
  FramePayload in;
  in.frameId = 99;

  auto encoded = EncodeFrame(in);
  FramePayload out;
  ASSERT_TRUE(DecodeFrame(encoded, out));
  EXPECT_EQ(out.frameId, 99u);
  EXPECT_TRUE(out.selections.empty());
  EXPECT_FALSE(out.hasMarquee);
  EXPECT_FALSE(out.hasHoverRect);
  EXPECT_TRUE(out.writebacks.empty());
  EXPECT_FALSE(out.hasSourceReplaceAll);
  EXPECT_EQ(out.statusKind, FrameStatusKind::kNone);
  EXPECT_TRUE(out.statusMessage.empty());
  EXPECT_TRUE(out.diagnostics.empty());
  EXPECT_FALSE(out.hasCursorHint);
}

TEST(EditorApiCodecTest, FrameRoundTripFull) {
  FramePayload in;
  in.frameId = 42;

  FrameSelectionEntry sel;
  sel.bbox[0] = 10.0;
  sel.bbox[1] = 20.0;
  sel.bbox[2] = 100.0;
  sel.bbox[3] = 200.0;
  sel.hasTransform = true;
  sel.transform[0] = 1.0;
  sel.transform[1] = 0.0;
  sel.transform[2] = 0.0;
  sel.transform[3] = 1.0;
  sel.transform[4] = 5.0;
  sel.transform[5] = 10.0;
  sel.handleMask = 0xFF;
  in.selections.push_back(sel);

  in.hasMarquee = true;
  in.marquee[0] = 0.0;
  in.marquee[1] = 0.0;
  in.marquee[2] = 50.0;
  in.marquee[3] = 50.0;

  in.hasHoverRect = true;
  in.hoverRect[0] = 5.0;
  in.hoverRect[1] = 5.0;
  in.hoverRect[2] = 25.0;
  in.hoverRect[3] = 25.0;

  FrameWritebackEntry wb;
  wb.start = 10;
  wb.end = 20;
  wb.newText = "transform=\"translate(5,10)\"";
  wb.reason = WritebackReason::kAttributeEdit;
  in.writebacks.push_back(wb);

  in.hasSourceReplaceAll = true;
  in.sourceReplaceAll = "<svg><rect transform=\"translate(5,10)\"/></svg>";

  in.statusKind = FrameStatusKind::kRendered;
  in.statusMessage = "OK";

  FrameDiagnosticEntry diag;
  diag.line = 1;
  diag.column = 5;
  diag.message = "unexpected attribute";
  in.diagnostics.push_back(diag);

  in.hasCursorHint = true;
  in.cursorHintSourceOffset = 42;

  in.hasFinalBitmap = true;
  in.finalBitmapWidth = 2;
  in.finalBitmapHeight = 1;
  in.finalBitmapRowBytes = 8;
  in.finalBitmapAlphaType = 1;
  in.finalBitmapPixels = {1, 2, 3, 4, 5, 6, 7, 8};

  const auto makeBitmap = [](uint8_t seed) {
    FrameBitmapPayload bitmap;
    bitmap.width = 1;
    bitmap.height = 1;
    bitmap.rowBytes = 4;
    bitmap.alphaType = 1;
    bitmap.pixels = {seed, static_cast<uint8_t>(seed + 1), static_cast<uint8_t>(seed + 2), 255};
    return bitmap;
  };
  in.hasCompositedPreview = true;
  in.compositedPreviewActive = true;
  in.hasCompositedPreviewBitmaps = true;
  in.compositedPreviewTranslationDoc[0] = 3.5;
  in.compositedPreviewTranslationDoc[1] = -2.25;
  in.compositedPreviewBackground = makeBitmap(10);
  in.compositedPreviewPromoted = makeBitmap(20);
  in.compositedPreviewForeground = makeBitmap(30);
  in.compositedPreviewOverlay = makeBitmap(40);

  auto encoded = EncodeFrame(in);
  FramePayload out;
  ASSERT_TRUE(DecodeFrame(encoded, out));

  EXPECT_EQ(out.frameId, 42u);
  ASSERT_EQ(out.selections.size(), 1u);
  EXPECT_DOUBLE_EQ(out.selections[0].bbox[0], 10.0);
  EXPECT_DOUBLE_EQ(out.selections[0].bbox[3], 200.0);
  EXPECT_TRUE(out.selections[0].hasTransform);
  EXPECT_DOUBLE_EQ(out.selections[0].transform[4], 5.0);
  EXPECT_EQ(out.selections[0].handleMask, 0xFFu);
  EXPECT_TRUE(out.hasMarquee);
  EXPECT_DOUBLE_EQ(out.marquee[2], 50.0);
  EXPECT_TRUE(out.hasHoverRect);
  EXPECT_DOUBLE_EQ(out.hoverRect[2], 25.0);
  ASSERT_EQ(out.writebacks.size(), 1u);
  EXPECT_EQ(out.writebacks[0].start, 10u);
  EXPECT_EQ(out.writebacks[0].end, 20u);
  EXPECT_EQ(out.writebacks[0].newText, wb.newText);
  EXPECT_EQ(out.writebacks[0].reason, WritebackReason::kAttributeEdit);
  EXPECT_TRUE(out.hasSourceReplaceAll);
  EXPECT_EQ(out.sourceReplaceAll, in.sourceReplaceAll);
  EXPECT_EQ(out.statusKind, FrameStatusKind::kRendered);
  EXPECT_EQ(out.statusMessage, "OK");
  ASSERT_EQ(out.diagnostics.size(), 1u);
  EXPECT_EQ(out.diagnostics[0].line, 1u);
  EXPECT_EQ(out.diagnostics[0].column, 5u);
  EXPECT_EQ(out.diagnostics[0].message, "unexpected attribute");
  EXPECT_TRUE(out.hasCursorHint);
  EXPECT_EQ(out.cursorHintSourceOffset, 42u);
  EXPECT_TRUE(out.hasFinalBitmap);
  EXPECT_EQ(out.finalBitmapWidth, 2);
  EXPECT_EQ(out.finalBitmapPixels, in.finalBitmapPixels);
  EXPECT_TRUE(out.hasCompositedPreview);
  EXPECT_TRUE(out.compositedPreviewActive);
  EXPECT_TRUE(out.hasCompositedPreviewBitmaps);
  EXPECT_DOUBLE_EQ(out.compositedPreviewTranslationDoc[0], 3.5);
  EXPECT_DOUBLE_EQ(out.compositedPreviewTranslationDoc[1], -2.25);
  EXPECT_EQ(out.compositedPreviewPromoted.pixels, in.compositedPreviewPromoted.pixels);
  EXPECT_EQ(out.compositedPreviewOverlay.pixels, in.compositedPreviewOverlay.pixels);
}

// ===========================================================================
// SourceReplaceAll, Toast, DialogRequest, Diagnostic, Error
// ===========================================================================

TEST(EditorApiCodecTest, SourceReplaceAllRoundTrip) {
  SourceReplaceAllPayload in;
  in.bytes = "<svg><g/></svg>";

  auto encoded = EncodeSourceReplaceAll(in);
  SourceReplaceAllPayload out;
  ASSERT_TRUE(DecodeSourceReplaceAll(encoded, out));
  EXPECT_EQ(out.bytes, in.bytes);
}

TEST(EditorApiCodecTest, ToastRoundTrip) {
  ToastResponsePayload in;
  in.severity = ToastSeverity::kWarning;
  in.message = "something happened";

  auto encoded = EncodeToast(in);
  ToastResponsePayload out;
  ASSERT_TRUE(DecodeToast(encoded, out));
  EXPECT_EQ(out.severity, ToastSeverity::kWarning);
  EXPECT_EQ(out.message, "something happened");
}

TEST(EditorApiCodecTest, DialogRequestRoundTrip) {
  DialogRequestPayload in;
  in.kind = 1;
  in.fileName = "output.svg";
  in.message = "Confirm overwrite?";

  auto encoded = EncodeDialogRequest(in);
  DialogRequestPayload out;
  ASSERT_TRUE(DecodeDialogRequest(encoded, out));
  EXPECT_EQ(out.kind, 1u);
  EXPECT_EQ(out.fileName, "output.svg");
  EXPECT_EQ(out.message, "Confirm overwrite?");
}

TEST(EditorApiCodecTest, DiagnosticRoundTrip) {
  DiagnosticPayload in;
  in.message = "debug: parse took 3ms";

  auto encoded = EncodeDiagnostic(in);
  DiagnosticPayload out;
  ASSERT_TRUE(DecodeDiagnostic(encoded, out));
  EXPECT_EQ(out.message, "debug: parse took 3ms");
}

TEST(EditorApiCodecTest, ErrorRoundTrip) {
  ErrorPayload in;
  in.errorKind = SessionErrorKind::kPayloadMalformed;
  in.message = "invalid pointer phase";

  auto encoded = EncodeError(in);
  ErrorPayload out;
  ASSERT_TRUE(DecodeError(encoded, out));
  EXPECT_EQ(out.errorKind, SessionErrorKind::kPayloadMalformed);
  EXPECT_EQ(out.message, "invalid pointer phase");
}

// ===========================================================================
// Malformed input tests
// ===========================================================================

TEST(EditorApiCodecTest, TruncatedSetViewport) {
  auto encoded = EncodeSetViewport({800, 600});
  // Remove last byte.
  encoded.pop_back();
  SetViewportPayload out;
  EXPECT_FALSE(DecodeSetViewport(encoded, out));
}

TEST(EditorApiCodecTest, TruncatedLoadBytes) {
  auto encoded = EncodeLoadBytes({"<svg/>", "file:///x"});
  // Truncate in the middle of the bytes field.
  encoded.resize(6);
  LoadBytesPayload out;
  EXPECT_FALSE(DecodeLoadBytes(encoded, out));
}

TEST(EditorApiCodecTest, InvalidPointerPhase) {
  PointerEventPayload in;
  in.phase = PointerPhase::kDown;
  auto encoded = EncodePointerEvent(in);
  // Overwrite phase with invalid value.
  uint32_t badPhase = 99;
  std::memcpy(encoded.data(), &badPhase, 4);
  PointerEventPayload out;
  EXPECT_FALSE(DecodePointerEvent(encoded, out));
}

TEST(EditorApiCodecTest, InvalidKeyPhase) {
  KeyEventPayload in;
  in.phase = KeyPhase::kDown;
  auto encoded = EncodeKeyEvent(in);
  uint32_t badPhase = 99;
  std::memcpy(encoded.data(), &badPhase, 4);
  KeyEventPayload out;
  EXPECT_FALSE(DecodeKeyEvent(encoded, out));
}

TEST(EditorApiCodecTest, InvalidToolKind) {
  SetToolPayload in;
  in.toolKind = ToolKind::kSelect;
  auto encoded = EncodeSetTool(in);
  uint32_t badKind = 99;
  std::memcpy(encoded.data(), &badKind, 4);
  SetToolPayload out;
  EXPECT_FALSE(DecodeSetTool(encoded, out));
}

TEST(EditorApiCodecTest, InvalidExportFormat) {
  auto encoded = EncodeExport({ExportFormat::kSvgText});
  uint32_t badFormat = 99;
  std::memcpy(encoded.data(), &badFormat, 4);
  ExportRequestPayload out;
  EXPECT_FALSE(DecodeExport(encoded, out));
}

TEST(EditorApiCodecTest, TruncatedFramePayload) {
  FramePayload in;
  in.frameId = 1;
  auto encoded = EncodeFrame(in);
  // Truncate mid-field.
  encoded.resize(4);
  FramePayload out;
  EXPECT_FALSE(DecodeFrame(encoded, out));
}

TEST(EditorApiCodecTest, EmptyDataForHandshake) {
  HandshakePayload out;
  EXPECT_FALSE(DecodeHandshake({}, out));
}

TEST(EditorApiCodecTest, EmptyDataForPointerEvent) {
  PointerEventPayload out;
  EXPECT_FALSE(DecodePointerEvent({}, out));
}

TEST(EditorApiCodecTest, EmptyDataForFrame) {
  FramePayload out;
  EXPECT_FALSE(DecodeFrame({}, out));
}

TEST(EditorApiCodecTest, InvalidToastSeverity) {
  ToastResponsePayload in;
  in.severity = ToastSeverity::kInfo;
  in.message = "hi";
  auto encoded = EncodeToast(in);
  uint32_t badSev = 99;
  std::memcpy(encoded.data(), &badSev, 4);
  ToastResponsePayload out;
  EXPECT_FALSE(DecodeToast(encoded, out));
}

TEST(EditorApiCodecTest, InvalidErrorKind) {
  ErrorPayload in;
  in.errorKind = SessionErrorKind::kUnknown;
  in.message = "x";
  auto encoded = EncodeError(in);
  uint32_t badKind = 99;
  std::memcpy(encoded.data(), &badKind, 4);
  ErrorPayload out;
  EXPECT_FALSE(DecodeError(encoded, out));
}

// ===========================================================================
// SelectElement round-trip
// ===========================================================================

TEST(EditorApiCodecTest, SelectElementRoundTrip) {
  SelectElementPayload in;
  in.entityId = 42;
  in.entityGeneration = 7;
  in.mode = 1;  // Toggle.

  auto encoded = EncodeSelectElement(in);
  SelectElementPayload out;
  ASSERT_TRUE(DecodeSelectElement(encoded, out));
  EXPECT_EQ(out.entityId, 42u);
  EXPECT_EQ(out.entityGeneration, 7u);
  EXPECT_EQ(out.mode, 1u);
}

TEST(EditorApiCodecTest, SelectElementInvalidMode) {
  SelectElementPayload in;
  in.entityId = 1;
  in.entityGeneration = 1;
  in.mode = 0;
  auto encoded = EncodeSelectElement(in);
  // Overwrite mode byte (at offset 16 = 8+8) with invalid value.
  encoded[16] = 5;
  SelectElementPayload out;
  EXPECT_FALSE(DecodeSelectElement(encoded, out));
}

// ===========================================================================
// FrameTreeSummary round-trip (embedded in FramePayload)
// ===========================================================================

TEST(EditorApiCodecTest, FrameTreeSummaryRoundTrip) {
  FramePayload in;
  in.frameId = 100;

  in.tree.generation = 3;
  in.tree.rootIndex = 0;

  TreeNodeEntry root;
  root.entityId = 1;
  root.entityGeneration = 3;
  root.parentIndex = 0xFFFFFFFF;
  root.depth = 0;
  root.tagName = "svg";
  root.idAttr = "";
  root.displayName = "<svg>";
  root.sourceStart = 0;
  root.sourceEnd = 50;
  root.selected = false;
  in.tree.nodes.push_back(root);

  TreeNodeEntry child1;
  child1.entityId = 2;
  child1.entityGeneration = 3;
  child1.parentIndex = 0;
  child1.depth = 1;
  child1.tagName = "rect";
  child1.idAttr = "myRect";
  child1.displayName = "<rect id=\"myRect\">";
  child1.sourceStart = 5;
  child1.sourceEnd = 30;
  child1.selected = true;
  in.tree.nodes.push_back(child1);

  TreeNodeEntry child2;
  child2.entityId = 3;
  child2.entityGeneration = 3;
  child2.parentIndex = 0;
  child2.depth = 1;
  child2.tagName = "circle";
  child2.idAttr = "";
  child2.displayName = "<circle>";
  child2.sourceStart = 31;
  child2.sourceEnd = 48;
  child2.selected = false;
  in.tree.nodes.push_back(child2);

  auto encoded = EncodeFrame(in);
  FramePayload out;
  ASSERT_TRUE(DecodeFrame(encoded, out));

  EXPECT_EQ(out.tree.generation, 3u);
  EXPECT_EQ(out.tree.rootIndex, 0u);
  ASSERT_EQ(out.tree.nodes.size(), 3u);

  // Root node.
  EXPECT_EQ(out.tree.nodes[0].entityId, 1u);
  EXPECT_EQ(out.tree.nodes[0].entityGeneration, 3u);
  EXPECT_EQ(out.tree.nodes[0].parentIndex, 0xFFFFFFFFu);
  EXPECT_EQ(out.tree.nodes[0].depth, 0u);
  EXPECT_EQ(out.tree.nodes[0].tagName, "svg");
  EXPECT_EQ(out.tree.nodes[0].idAttr, "");
  EXPECT_EQ(out.tree.nodes[0].displayName, "<svg>");
  EXPECT_EQ(out.tree.nodes[0].sourceStart, 0u);
  EXPECT_EQ(out.tree.nodes[0].sourceEnd, 50u);
  EXPECT_FALSE(out.tree.nodes[0].selected);

  // Child 1 (selected rect).
  EXPECT_EQ(out.tree.nodes[1].entityId, 2u);
  EXPECT_EQ(out.tree.nodes[1].tagName, "rect");
  EXPECT_EQ(out.tree.nodes[1].idAttr, "myRect");
  EXPECT_EQ(out.tree.nodes[1].displayName, "<rect id=\"myRect\">");
  EXPECT_EQ(out.tree.nodes[1].parentIndex, 0u);
  EXPECT_EQ(out.tree.nodes[1].depth, 1u);
  EXPECT_TRUE(out.tree.nodes[1].selected);

  // Child 2 (circle).
  EXPECT_EQ(out.tree.nodes[2].entityId, 3u);
  EXPECT_EQ(out.tree.nodes[2].tagName, "circle");
  EXPECT_EQ(out.tree.nodes[2].parentIndex, 0u);
  EXPECT_EQ(out.tree.nodes[2].depth, 1u);
  EXPECT_FALSE(out.tree.nodes[2].selected);
}

TEST(EditorApiCodecTest, FrameTreeSummaryEmpty) {
  FramePayload in;
  in.frameId = 200;
  // tree left default (empty).

  auto encoded = EncodeFrame(in);
  FramePayload out;
  ASSERT_TRUE(DecodeFrame(encoded, out));
  EXPECT_EQ(out.tree.generation, 0u);
  EXPECT_EQ(out.tree.rootIndex, 0xFFFFFFFFu);
  EXPECT_TRUE(out.tree.nodes.empty());
}

// ===========================================================================
// Inspector snapshot round-trip (G4 in §S12 of 0023-editor_sandbox.md).
// Covers empty (no selection), populated with XML attributes + computed
// style, and selector-only (tag/id/class without attributes — the
// degenerate path for a fresh document with no authored styling).
// ===========================================================================

TEST(EditorApiCodecTest, InspectedElementDefaultsWhenAbsent) {
  FramePayload in;
  in.frameId = 7;
  // hasInspectedElement left default (false).

  auto encoded = EncodeFrame(in);
  FramePayload out;
  ASSERT_TRUE(DecodeFrame(encoded, out));
  EXPECT_FALSE(out.hasInspectedElement);
}

TEST(EditorApiCodecTest, InspectedElementRoundTripFull) {
  FramePayload in;
  in.frameId = 8;

  in.hasInspectedElement = true;
  auto& insp = in.inspectedElement;
  insp.entityId = 0xDEADBEEFu;
  insp.entityGeneration = 3;
  insp.tagName = "rect";
  insp.idAttr = "hero";
  insp.className = "highlight accent";
  insp.xmlAttributes = {
      {"x", "10"},      {"y", "20"},     {"width", "100"},
      {"height", "50"}, {"fill", "red"}, {"xlink:href", "#resource"},
  };
  insp.computedStyle = {
      {"display", "Inline (set)"},
      {"fill", "Color(rgba(255,0,0,255)) (set)"},
      {"stroke", "none (not set)"},
  };

  auto encoded = EncodeFrame(in);
  FramePayload out;
  ASSERT_TRUE(DecodeFrame(encoded, out));
  ASSERT_TRUE(out.hasInspectedElement);
  const auto& rtt = out.inspectedElement;
  EXPECT_EQ(rtt.entityId, 0xDEADBEEFu);
  EXPECT_EQ(rtt.entityGeneration, 3u);
  EXPECT_EQ(rtt.tagName, "rect");
  EXPECT_EQ(rtt.idAttr, "hero");
  EXPECT_EQ(rtt.className, "highlight accent");
  ASSERT_EQ(rtt.xmlAttributes.size(), 6u);
  EXPECT_EQ(rtt.xmlAttributes[0].name, "x");
  EXPECT_EQ(rtt.xmlAttributes[0].value, "10");
  EXPECT_EQ(rtt.xmlAttributes[5].name, "xlink:href");
  EXPECT_EQ(rtt.xmlAttributes[5].value, "#resource");
  ASSERT_EQ(rtt.computedStyle.size(), 3u);
  EXPECT_EQ(rtt.computedStyle[1].name, "fill");
  EXPECT_EQ(rtt.computedStyle[1].value, "Color(rgba(255,0,0,255)) (set)");
}

TEST(EditorApiCodecTest, InspectedElementRoundTripTagOnly) {
  FramePayload in;
  in.frameId = 9;
  in.hasInspectedElement = true;
  in.inspectedElement.entityId = 1;
  in.inspectedElement.entityGeneration = 1;
  in.inspectedElement.tagName = "svg";
  // id/class/attributes/computedStyle all empty.

  auto encoded = EncodeFrame(in);
  FramePayload out;
  ASSERT_TRUE(DecodeFrame(encoded, out));
  ASSERT_TRUE(out.hasInspectedElement);
  EXPECT_EQ(out.inspectedElement.tagName, "svg");
  EXPECT_TRUE(out.inspectedElement.idAttr.empty());
  EXPECT_TRUE(out.inspectedElement.className.empty());
  EXPECT_TRUE(out.inspectedElement.xmlAttributes.empty());
  EXPECT_TRUE(out.inspectedElement.computedStyle.empty());
}

}  // namespace
}  // namespace donner::editor::sandbox
