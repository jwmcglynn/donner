#pragma once
/// @file
///
/// Per-opcode payload encode/decode functions for the editor session protocol
/// (§S8 of docs/design_docs/0023-editor_sandbox.md).
///
/// Every encoder returns a `std::vector<uint8_t>` that becomes the `payload`
/// field of a `SessionFrame`. Every decoder reads from a span and populates an
/// output struct, returning `false` on malformed input.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/sandbox/SessionProtocol.h"
#include "donner/editor/sandbox/Wire.h"

namespace donner::editor::sandbox {

// ---------------------------------------------------------------------------
// Request payload structs (host → backend)
// ---------------------------------------------------------------------------

struct SetViewportPayload {
  int32_t width = 0;
  int32_t height = 0;
};

struct LoadBytesPayload {
  std::string bytes;
  std::optional<std::string> originUri;
};

struct ReplaceSourcePayload {
  std::string bytes;
  bool preserveUndoOnReparse = false;
};

struct ApplySourcePatchPayload {
  uint32_t start = 0;
  uint32_t end = 0;
  std::string newText;
};

struct PointerEventPayload {
  PointerPhase phase = PointerPhase::kDown;
  double documentX = 0.0;
  double documentY = 0.0;
  uint32_t buttons = 0;
  uint32_t modifiers = 0;
};

struct KeyEventPayload {
  KeyPhase phase = KeyPhase::kDown;
  int32_t keyCode = 0;
  uint32_t modifiers = 0;
  std::string textInput;
};

struct WheelEventPayload {
  double documentX = 0.0;
  double documentY = 0.0;
  double deltaX = 0.0;
  double deltaY = 0.0;
  uint32_t modifiers = 0;
};

struct SetToolPayload {
  ToolKind toolKind = ToolKind::kSelect;
};

struct SelectElementPayload {
  uint64_t entityId = 0;
  uint64_t entityGeneration = 0;
  uint8_t mode = 0;  ///< 0 = Replace, 1 = Toggle, 2 = Add.
};

struct ExportRequestPayload {
  ExportFormat format = ExportFormat::kSvgText;
};

struct HandshakePayload {
  uint32_t protocolVersion = 0;
  std::string buildId;
};

// ---------------------------------------------------------------------------
// Response payload structs (backend → host)
// ---------------------------------------------------------------------------

struct HandshakeAckPayload {
  uint32_t protocolVersion = 0;
  uint64_t pid = 0;
  std::string capabilities;
};

struct FrameSelectionEntry {
  double bbox[4] = {};  ///< topLeft.x, topLeft.y, bottomRight.x, bottomRight.y
  bool hasTransform = false;
  double transform[6] = {};  ///< a, b, c, d, e, f (affine 2×3)
  uint32_t handleMask = 0;
};

struct FrameWritebackEntry {
  uint32_t start = 0;
  uint32_t end = 0;
  std::string newText;
  WritebackReason reason = WritebackReason::kAttributeEdit;
};

struct FrameDiagnosticEntry {
  uint32_t line = 0;
  uint32_t column = 0;
  std::string message;
};

/// Status kind for the per-frame status chip.
enum class FrameStatusKind : uint8_t {
  kNone = 0,
  kRendered = 1,
  kRenderedLossy = 2,
  kParseError = 3,
};

/// Single node in the backend's flattened tree summary.
struct TreeNodeEntry {
  uint64_t entityId = 0;          ///< Opaque handle — sandbox-assigned.
  uint64_t entityGeneration = 0;
  uint32_t parentIndex = 0xFFFFFFFF;  ///< Index into FrameTreeSummary::nodes; 0xFFFFFFFF for root.
  uint32_t depth = 0;
  std::string tagName;            ///< Lower-case: "rect", "g", etc.
  std::string idAttr;             ///< DOM id or empty.
  std::string displayName;        ///< e.g. "<rect id=foo>".
  uint32_t sourceStart = 0;
  uint32_t sourceEnd = 0;
  bool selected = false;
};

/// Flat tree summary shipped with every frame.
struct FrameTreeSummary {
  uint64_t generation = 0;        ///< Bumped on structural changes.
  uint32_t rootIndex = 0xFFFFFFFF;
  std::vector<TreeNodeEntry> nodes;
};

struct FramePayload {
  uint64_t frameId = 0;
  std::vector<uint8_t> renderWire;
  std::vector<FrameSelectionEntry> selections;
  bool hasMarquee = false;
  double marquee[4] = {};
  bool hasHoverRect = false;
  double hoverRect[4] = {};
  std::vector<FrameWritebackEntry> writebacks;
  bool hasSourceReplaceAll = false;
  std::string sourceReplaceAll;
  FrameStatusKind statusKind = FrameStatusKind::kNone;
  std::string statusMessage;
  std::vector<FrameDiagnosticEntry> diagnostics;
  bool hasCursorHint = false;
  uint32_t cursorHintSourceOffset = 0;
  FrameTreeSummary tree;
};

struct ExportResponsePayload {
  ExportFormat format = ExportFormat::kSvgText;
  std::vector<uint8_t> bytes;
};

struct SourceReplaceAllPayload {
  std::string bytes;
};

struct ToastResponsePayload {
  ToastSeverity severity = ToastSeverity::kInfo;
  std::string message;
};

struct DialogRequestPayload {
  uint32_t kind = 0;
  std::string fileName;
  std::string message;
};

struct DiagnosticPayload {
  std::string message;
};

struct ErrorPayload {
  SessionErrorKind errorKind = SessionErrorKind::kUnknown;
  std::string message;
};

// ---------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------

/// @name Request encoders (host → backend)
/// @{
std::vector<uint8_t> EncodeHandshake(const HandshakePayload& payload);
std::vector<uint8_t> EncodeShutdown();
std::vector<uint8_t> EncodeSetViewport(const SetViewportPayload& payload);
std::vector<uint8_t> EncodeLoadBytes(const LoadBytesPayload& payload);
std::vector<uint8_t> EncodeReplaceSource(const ReplaceSourcePayload& payload);
std::vector<uint8_t> EncodeApplySourcePatch(const ApplySourcePatchPayload& payload);
std::vector<uint8_t> EncodePointerEvent(const PointerEventPayload& payload);
std::vector<uint8_t> EncodeKeyEvent(const KeyEventPayload& payload);
std::vector<uint8_t> EncodeWheelEvent(const WheelEventPayload& payload);
std::vector<uint8_t> EncodeSetTool(const SetToolPayload& payload);
std::vector<uint8_t> EncodeSelectElement(const SelectElementPayload& payload);
std::vector<uint8_t> EncodeUndo();
std::vector<uint8_t> EncodeRedo();
std::vector<uint8_t> EncodeExport(const ExportRequestPayload& payload);
/// @}

/// @name Response encoders (backend → host)
/// @{
std::vector<uint8_t> EncodeHandshakeAck(const HandshakeAckPayload& payload);
std::vector<uint8_t> EncodeShutdownAck();
std::vector<uint8_t> EncodeFrame(const FramePayload& payload);
std::vector<uint8_t> EncodeExportResponse(const ExportResponsePayload& payload);
std::vector<uint8_t> EncodeSourceReplaceAll(const SourceReplaceAllPayload& payload);
std::vector<uint8_t> EncodeToast(const ToastResponsePayload& payload);
std::vector<uint8_t> EncodeDialogRequest(const DialogRequestPayload& payload);
std::vector<uint8_t> EncodeDiagnostic(const DiagnosticPayload& payload);
std::vector<uint8_t> EncodeError(const ErrorPayload& payload);
/// @}

// ---------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------

/// @name Request decoders (host → backend)
/// @{
bool DecodeHandshake(std::span<const uint8_t> data, HandshakePayload& out);
bool DecodeShutdown(std::span<const uint8_t> data);
bool DecodeSetViewport(std::span<const uint8_t> data, SetViewportPayload& out);
bool DecodeLoadBytes(std::span<const uint8_t> data, LoadBytesPayload& out);
bool DecodeReplaceSource(std::span<const uint8_t> data, ReplaceSourcePayload& out);
bool DecodeApplySourcePatch(std::span<const uint8_t> data, ApplySourcePatchPayload& out);
bool DecodePointerEvent(std::span<const uint8_t> data, PointerEventPayload& out);
bool DecodeKeyEvent(std::span<const uint8_t> data, KeyEventPayload& out);
bool DecodeWheelEvent(std::span<const uint8_t> data, WheelEventPayload& out);
bool DecodeSetTool(std::span<const uint8_t> data, SetToolPayload& out);
bool DecodeSelectElement(std::span<const uint8_t> data, SelectElementPayload& out);
bool DecodeUndo(std::span<const uint8_t> data);
bool DecodeRedo(std::span<const uint8_t> data);
bool DecodeExport(std::span<const uint8_t> data, ExportRequestPayload& out);
/// @}

/// @name Response decoders (backend → host)
/// @{
bool DecodeHandshakeAck(std::span<const uint8_t> data, HandshakeAckPayload& out);
bool DecodeShutdownAck(std::span<const uint8_t> data);
bool DecodeFrame(std::span<const uint8_t> data, FramePayload& out);
bool DecodeExportResponse(std::span<const uint8_t> data, ExportResponsePayload& out);
bool DecodeSourceReplaceAll(std::span<const uint8_t> data, SourceReplaceAllPayload& out);
bool DecodeToast(std::span<const uint8_t> data, ToastResponsePayload& out);
bool DecodeDialogRequest(std::span<const uint8_t> data, DialogRequestPayload& out);
bool DecodeDiagnostic(std::span<const uint8_t> data, DiagnosticPayload& out);
bool DecodeError(std::span<const uint8_t> data, ErrorPayload& out);
/// @}

}  // namespace donner::editor::sandbox
