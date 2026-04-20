#pragma once
/// @file
///
/// Session-level wire protocol for the long-lived `SandboxSession`. The IPC
/// boundary is the editor's public API — pointer / keyboard events,
/// document load, source patch, undo, set-tool — not a DOM reflection. See
/// docs/design_docs/0023-editor_sandbox.md §S8.
///
/// Every session message uses the framing:
///
/// ```
/// u32 magic    = 'DRNS'   (distinct from renderer-wire 'DRNR' so a
///                          cross-layer framing bug fails loudly)
/// u64 requestId           (host-assigned; echoed in the matching response;
///                          0 for unsolicited backend pushes like kFrame as
///                          async toast or kDiagnostic)
/// u32 opcode              (SessionOpcode; see below)
/// u32 payloadLength       (bytes that follow; capped at kSessionMaxPayload)
/// u8  payload[payloadLength]
/// ```

#include <cstdint>

namespace donner::editor::sandbox {

/// Session-framing magic: "DRNS" little-endian.
inline constexpr uint32_t kSessionMagic = 0x534E5244u;

inline constexpr uint32_t kSessionProtocolVersion = 1;

/// Hard cap on session payload size.
inline constexpr uint32_t kSessionMaxPayload = 256u * 1024u * 1024u;

/// Session-level opcodes. Two numeric regions:
///
/// - **Requests (host → backend)**: 1–199.
/// - **Responses + pushes (backend → host)**: 200–399.
///
/// Split so a mis-routed opcode is detected at decode time rather than
/// silently processed by the wrong side.
enum class SessionOpcode : uint32_t {
  kInvalid = 0,

  // ---------------------------------------------------------------
  // Requests — host → backend
  // ---------------------------------------------------------------

  /// First request after spawn. Carries protocol version + build id. The
  /// backend replies with `kHandshakeAck`.
  kHandshake = 1,

  /// Graceful exit. Backend replies with `kShutdownAck` and exits.
  kShutdown = 2,

  /// Set the editor viewport (CSS pixels). Triggers a re-render.
  kSetViewport = 10,

  /// Load a fresh document from bytes. Clears undo.
  kLoadBytes = 11,

  /// Replace the entire source (text-editor pane change). Pipes into
  /// `EditorApp::applyMutation(ReplaceDocumentCommand)`.
  kReplaceSource = 12,

  /// Structured-edit fast path: replace a byte range in the current
  /// source. Backend classifies as an attribute-level edit where
  /// possible; falls back to `kReplaceSource` semantics otherwise.
  kApplySourcePatch = 13,

  /// Pointer event — phase + document-space point + buttons + modifiers.
  kPointerEvent = 20,

  /// Keyboard event. Only document-scoped shortcuts reach here; the host
  /// consumes UI-only shortcuts locally.
  kKeyEvent = 21,

  /// Scroll / pinch wheel event.
  kWheelEvent = 22,

  /// Switch the active tool.
  kSetTool = 30,

  /// Pass-through to `EditorApp::undo()` / `redo()`.
  kUndo = 31,
  kRedo = 32,

  /// Request the current source bytes (or a rendered raster) back.
  kExport = 40,

  // ---------------------------------------------------------------
  // Responses + async pushes — backend → host
  // ---------------------------------------------------------------

  /// Reply to `kHandshake`. Carries the backend's protocol version and
  /// capability set. Mismatched build-id → host closes the session.
  kHandshakeAck = 200,

  /// Reply to `kShutdown`. Terminal.
  kShutdownAck = 201,

  /// **The default response** to every mutating request. See the Frame
  /// payload schema in §S8.
  kFrame = 210,

  /// Sent alongside a `kFrame` (or standalone) when the backend's
  /// authoritative source has diverged from the host's cached bytes
  /// (e.g. after `Undo` / `Redo` across structural edits).
  kSourceReplaceAll = 211,

  /// Reply to `kExport`.
  kExportResponse = 220,

  /// Unsolicited user-visible notification.
  kToast = 230,

  /// Backend asks the host to open an OS dialog on its behalf (save-as,
  /// confirm overwrite, etc.). Host responds with the chosen path via
  /// a fresh `kLoadBytes` / `kExport` / … request.
  kDialogRequest = 231,

  /// Free-form developer log message. Routed to the editor's console
  /// pane when debug builds are enabled.
  kDiagnostic = 240,

  /// Fatal-per-request protocol error. Typically causes the session to
  /// be torn down unless the error is advisory (e.g. `kParseError` still
  /// produces a `kFrame` with previous good state preserved).
  kError = 241,
};

/// Subset of errors the backend reports via `kError`. Raw u32 tag on the
/// wire so unknown values are forward-compatible.
enum class SessionErrorKind : uint32_t {
  kUnknown = 0,
  kUnknownOpcode = 1,
  kPayloadMalformed = 2,
  kBuildIdMismatch = 3,
  kInternalError = 4,
};

/// Pointer-event phases — subset of what the editor tools already
/// enumerate, plus a `kEnter` / `kLeave` for the host's focus tracking.
enum class PointerPhase : uint32_t {
  kDown = 0,
  kMove = 1,
  kUp = 2,
  kEnter = 3,
  kLeave = 4,
  kCancel = 5,
};

enum class KeyPhase : uint32_t {
  kDown = 0,
  kUp = 1,
  kChar = 2,
};

enum class ToolKind : uint32_t {
  kSelect = 0,
  kRect = 1,
  kEllipse = 2,
  kPath = 3,
  kText = 4,
  // New tools append here.
};

enum class ExportFormat : uint32_t {
  kSvgText = 0,
  kPng = 1,
};

enum class ToastSeverity : uint32_t {
  kInfo = 0,
  kWarning = 1,
  kError = 2,
};

enum class WritebackReason : uint32_t {
  kAttributeEdit = 0,
  kCanonicalization = 1,
  kElementRemoval = 2,
};

}  // namespace donner::editor::sandbox
