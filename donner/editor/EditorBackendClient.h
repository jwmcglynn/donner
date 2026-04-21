#pragma once
/// @file
///
/// **`EditorBackendClient`** — host-side API surface for talking to the
/// editor backend process. Encapsulates everything the UI layer needs to
/// drive a document: load bytes, route input events, undo/redo, ask for
/// exports. Every call returns a `std::future<FrameResult>` so the UI can
/// either await (blocking navigation) or attach continuations (per-event
/// pipeline).
///
/// Two implementations, one header:
///
/// - **`EditorBackendClient_Session.cc`** — desktop. Wraps a
///   `sandbox::SandboxSession` and encodes each request as an S8 wire
///   message. Replies decode back into `FrameResult`. The parser is never
///   linked into this translation unit.
/// - **`EditorBackendClient_InProcess.cc`** — WASM. Statically links the
///   backend library and invokes the editor directly. Same `FrameResult`
///   shape; no IPC. Browser is the sandbox.
///
/// See docs/design_docs/0023-editor_sandbox.md §S9.

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/ParseDiagnostic.h"
#include "donner/base/Vector2.h"
#include "donner/editor/AddressBarStatus.h"  // AddressBarStatusChip — no ImGui dep
#include "donner/editor/SelectionOverlay.h"
#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/editor/sandbox/SessionProtocol.h"
#include "donner/editor/sandbox/bridge/BridgeTexture.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::sandbox {
class SandboxSession;
}  // namespace donner::editor::sandbox

namespace donner::editor {

/// Pointer event the host forwards to the backend. Positions are already
/// mapped into document space by the host's viewport transform.
struct PointerEventPayload {
  sandbox::PointerPhase phase = sandbox::PointerPhase::kMove;
  Vector2d documentPoint;
  uint32_t buttons = 0;    ///< Bitmask — bit 0 = primary, bit 1 = secondary.
  uint32_t modifiers = 0;  ///< Bitmask — Ctrl / Shift / Alt / Meta.
};

struct KeyEventPayload {
  sandbox::KeyPhase phase = sandbox::KeyPhase::kDown;
  int32_t keyCode = 0;       ///< Platform-independent scancode (GLFW keycode on desktop).
  uint32_t modifiers = 0;
  std::string textInput;     ///< Populated for `kChar`; empty otherwise.
};

struct WheelEventPayload {
  Vector2d documentPoint;
  double deltaX = 0.0;
  double deltaY = 0.0;
  uint32_t modifiers = 0;
};

struct ExportPayload {
  sandbox::ExportFormat format = sandbox::ExportFormat::kSvgText;
};

/// Single-shot edit the backend asks the host to apply to its TextEditor
/// (e.g. after a canvas drag commits). Byte offsets reference the source
/// bytes the host last sent the backend (via `kLoadBytes` or
/// `kReplaceSource`).
struct SourceWriteback {
  uint32_t sourceStart = 0;
  uint32_t sourceEnd = 0;
  std::string newText;
  sandbox::WritebackReason reason = sandbox::WritebackReason::kAttributeEdit;
};

/// Unified per-event reply. Produced by every mutating API call.
struct FrameResult {
  bool ok = false;
  /// Sequence number. Monotonic within a session.
  uint64_t frameId = 0;
  /// Backend produced at least one `kUnsupported` during this frame's
  /// render. Host may show the "lossy" chip color.
  uint32_t unsupportedCount = 0;
  /// Locally-rasterized bitmap produced by replaying the backend's
  /// render wire into the host's real `Renderer`.
  svg::RendererBitmap bitmap;
  /// Updated selection chrome.
  SelectionOverlay selection;
  /// Source writebacks to apply to the host's TextEditor in order.
  std::vector<SourceWriteback> writebacks;
  /// When present, the host's TextEditor should adopt these bytes
  /// wholesale as the new source baseline.
  std::optional<std::string> sourceReplaceAll;
  /// Optional status-chip override. `nullopt` → caller picks a default.
  std::optional<AddressBarStatusChip> statusChip;
  /// Parse diagnostics from the most recent load / patch.
  std::vector<ParseDiagnostic> parseDiagnostics;
  /// Document tree summary from the backend.
  sandbox::FrameTreeSummary tree;
  /// The SVG's own viewBox, in document (user-space) coordinates.
  /// `nullopt` when the backend reports no document (before loadBytes
  /// or after a parse error). Host uses this to drive
  /// `ViewportState::documentViewBox` so screen↔document math works.
  std::optional<Box2d> documentViewBox;
};

struct ExportResult {
  bool ok = false;
  sandbox::ExportFormat format = sandbox::ExportFormat::kSvgText;
  std::vector<uint8_t> bytes;
};

struct ToastPayload {
  sandbox::ToastSeverity severity = sandbox::ToastSeverity::kInfo;
  std::string message;
};

struct DialogRequestPayload {
  enum class Kind : uint32_t {
    kSaveAs,
    kConfirmOverwrite,
    kConfirmDestructive,
  };
  Kind kind = Kind::kSaveAs;
  std::string suggestedFileName;
  std::string message;
};

/// Opaque handle: the ctor picks an implementation at link time.
class EditorBackendClient {
public:
  /// Desktop ctor — uses `SandboxSession` as the transport. The session
  /// must outlive this client.
  static std::unique_ptr<EditorBackendClient> MakeSessionBacked(
      sandbox::SandboxSession& session);

  /// WASM ctor — runs the backend library in-process. Never linked on
  /// desktop (and vice versa).
  static std::unique_ptr<EditorBackendClient> MakeInProcess();

  virtual ~EditorBackendClient() = default;

  EditorBackendClient(const EditorBackendClient&) = delete;
  EditorBackendClient& operator=(const EditorBackendClient&) = delete;

  // ------------ document / source ------------

  [[nodiscard]] virtual std::future<FrameResult> loadBytes(
      std::span<const uint8_t> bytes,
      std::optional<std::string> originUri) = 0;

  [[nodiscard]] virtual std::future<FrameResult> replaceSource(
      std::string bytes, bool preserveUndoOnReparse) = 0;

  [[nodiscard]] virtual std::future<FrameResult> applySourcePatch(
      uint32_t sourceStart, uint32_t sourceEnd, std::string newText) = 0;

  // ------------ input events ------------

  [[nodiscard]] virtual std::future<FrameResult> pointerEvent(
      const PointerEventPayload& ev) = 0;
  [[nodiscard]] virtual std::future<FrameResult> keyEvent(
      const KeyEventPayload& ev) = 0;
  [[nodiscard]] virtual std::future<FrameResult> wheelEvent(
      const WheelEventPayload& ev) = 0;

  // ------------ tool + viewport + history ------------

  [[nodiscard]] virtual std::future<FrameResult> setTool(
      sandbox::ToolKind kind) = 0;
  [[nodiscard]] virtual std::future<FrameResult> setViewport(
      int width, int height) = 0;

  /// Hand a host-allocated shared GPU texture descriptor to the
  /// backend so subsequent renders can target it directly (zero-copy
  /// GPU path). Session-lifetime one-shot — call once after
  /// construction, before `loadBytes`. Handle ownership stays with
  /// the caller; backend takes a retain on the underlying platform
  /// surface. See `donner/editor/sandbox/bridge/BridgeTexture.h`.
  [[nodiscard]] virtual std::future<FrameResult> attachSharedTexture(
      const sandbox::bridge::BridgeTextureHandle& handle) = 0;

  [[nodiscard]] virtual std::future<FrameResult> undo() = 0;
  [[nodiscard]] virtual std::future<FrameResult> redo() = 0;

  // ------------ tree selection ------------

  [[nodiscard]] virtual std::future<FrameResult> selectElement(uint64_t entityId,
                                                                uint64_t entityGeneration,
                                                                uint8_t mode) = 0;

  // ------------ export ------------

  [[nodiscard]] virtual std::future<ExportResult> exportDocument(
      const ExportPayload& payload) = 0;

  // ------------ async-push callbacks ------------

  using ToastCallback = std::function<void(ToastPayload)>;
  using DialogRequestCallback = std::function<void(DialogRequestPayload)>;
  using DiagnosticCallback = std::function<void(std::string)>;

  virtual void setToastCallback(ToastCallback cb) = 0;
  virtual void setDialogRequestCallback(DialogRequestCallback cb) = 0;
  virtual void setDiagnosticCallback(DiagnosticCallback cb) = 0;

  // ------------ read-only state from the latest frame ------------

  [[nodiscard]] virtual uint64_t lastFrameId() const = 0;
  [[nodiscard]] virtual const SelectionOverlay& selection() const = 0;
  [[nodiscard]] virtual const svg::RendererBitmap& latestBitmap() const = 0;
  /// Latest document viewBox the backend reported — the user-space
  /// coordinate system for selection bboxes and pointer events.
  /// `nullopt` when no document has been loaded yet.
  [[nodiscard]] virtual std::optional<Box2d> latestDocumentViewBox() const = 0;
  [[nodiscard]] virtual std::optional<ParseDiagnostic> lastParseError() const = 0;
  [[nodiscard]] virtual const sandbox::FrameTreeSummary& tree() const = 0;

protected:
  EditorBackendClient() = default;
};

}  // namespace donner::editor
