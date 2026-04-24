/// @file
///
/// Desktop implementation of `EditorBackendClient`. Wraps a
/// `sandbox::SandboxSession` and encodes each host-side call via
/// `EditorApiCodec`, sends as a `SessionFrame`, decodes the reply, replays
/// the render wire into a local `Renderer`, and produces a `FrameResult`.
///
/// See docs/design_docs/0023-editor_sandbox.md §S9.

#include <mutex>
#include <thread>

#include "donner/base/Vector2.h"
#include "donner/editor/AddressBarStatus.h"
#include "donner/editor/EditorBackendClient.h"
#include "donner/editor/SelectionOverlay.h"
#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/editor/sandbox/SandboxSession.h"
#include "donner/editor/sandbox/SessionCodec.h"
#include "donner/editor/sandbox/SessionProtocol.h"
#include "donner/editor/sandbox/bridge/BridgeTexture.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

namespace {

using sandbox::DecodeFrame;
using sandbox::EncodeFrame;
using sandbox::FramePayload;
using sandbox::SessionFrame;
using sandbox::SessionOpcode;
using sandbox::WireRequest;
using sandbox::WireResponse;

/// Converts a decoded `FramePayload` into a `FrameResult`. Historically
/// this replayed the backend's render wire into a local `Renderer`; the
/// thin-client architecture instead ships pre-composed bitmap bytes in
/// `frame.finalBitmapPixels` which we just re-wrap. No renderer needed.
FrameResult DecodeFrameResult(const FramePayload& frame) {
  FrameResult result;
  result.ok = true;
  result.frameId = frame.frameId;

  // Backend ships the pre-composed bitmap directly; host just
  // re-wraps it as `result.bitmap` for upload.
  if (frame.hasFinalBitmap) {
    result.bitmap.dimensions = Vector2i(frame.finalBitmapWidth, frame.finalBitmapHeight);
    result.bitmap.rowBytes = frame.finalBitmapRowBytes;
    result.bitmap.alphaType = static_cast<svg::AlphaType>(frame.finalBitmapAlphaType);
    result.bitmap.pixels = frame.finalBitmapPixels;
  }

  if (frame.hasCompositedPreview) {
    const auto convertBitmap =
        [](const sandbox::FrameBitmapPayload& payload) -> svg::RendererBitmap {
      svg::RendererBitmap bitmap;
      bitmap.dimensions = Vector2i(payload.width, payload.height);
      bitmap.rowBytes = payload.rowBytes;
      bitmap.alphaType = static_cast<svg::AlphaType>(payload.alphaType);
      bitmap.pixels = payload.pixels;
      return bitmap;
    };
    result.compositedPreview = FrameResult::CompositedPreview{
        .backgroundBitmap = convertBitmap(frame.compositedPreviewBackground),
        .promotedBitmap = convertBitmap(frame.compositedPreviewPromoted),
        .foregroundBitmap = convertBitmap(frame.compositedPreviewForeground),
        .overlayBitmap = convertBitmap(frame.compositedPreviewOverlay),
        .promotedTranslationDoc = Vector2d(frame.compositedPreviewTranslationDoc[0],
                                           frame.compositedPreviewTranslationDoc[1]),
        .active = frame.compositedPreviewActive,
        .includesBitmapUploads = frame.hasCompositedPreviewBitmaps,
    };
  }

  // Convert selection entries.
  for (const auto& sel : frame.selections) {
    OverlaySelection overlay;
    overlay.worldBBox =
        Box2d(Vector2d(sel.bbox[0], sel.bbox[1]), Vector2d(sel.bbox[2], sel.bbox[3]));
    overlay.hasWorldTransform = sel.hasTransform;
    if (sel.hasTransform) {
      Transform2d t(Transform2d::uninitialized);
      t.data[0] = sel.transform[0];
      t.data[1] = sel.transform[1];
      t.data[2] = sel.transform[2];
      t.data[3] = sel.transform[3];
      t.data[4] = sel.transform[4];
      t.data[5] = sel.transform[5];
      overlay.worldTransform = t;
    }
    overlay.handleMask = sel.handleMask;
    result.selection.selections.push_back(overlay);
  }

  // Marquee.
  if (frame.hasMarquee) {
    result.selection.marquee = Box2d(Vector2d(frame.marquee[0], frame.marquee[1]),
                                     Vector2d(frame.marquee[2], frame.marquee[3]));
  }

  // Hover rect.
  if (frame.hasHoverRect) {
    result.selection.hoverRect = Box2d(Vector2d(frame.hoverRect[0], frame.hoverRect[1]),
                                       Vector2d(frame.hoverRect[2], frame.hoverRect[3]));
  }

  // Writebacks.
  for (const auto& wb : frame.writebacks) {
    SourceWriteback swb;
    swb.sourceStart = wb.start;
    swb.sourceEnd = wb.end;
    swb.newText = wb.newText;
    swb.reason = wb.reason;
    result.writebacks.push_back(std::move(swb));
  }

  // Source replace-all.
  if (frame.hasSourceReplaceAll) {
    result.sourceReplaceAll = frame.sourceReplaceAll;
  }

  // Status chip.
  if (frame.statusKind != sandbox::FrameStatusKind::kNone) {
    AddressBarStatusChip chip;
    switch (frame.statusKind) {
      case sandbox::FrameStatusKind::kRendered: chip.status = AddressBarStatus::kRendered; break;
      case sandbox::FrameStatusKind::kRenderedLossy:
        chip.status = AddressBarStatus::kRenderedLossy;
        break;
      case sandbox::FrameStatusKind::kParseError:
        chip.status = AddressBarStatus::kParseError;
        break;
      default: break;
    }
    chip.message = frame.statusMessage;
    result.statusChip = chip;
  }

  // Diagnostics.
  for (const auto& diag : frame.diagnostics) {
    ParseDiagnostic pd;
    pd.reason = RcString(diag.message);
    // Use line/column to approximate source range.
    pd.range = {FileOffset::Offset(0), FileOffset::Offset(0)};
    result.parseDiagnostics.push_back(std::move(pd));
  }

  // Tree summary.
  result.tree = frame.tree;

  // Inspector snapshot — matches EditorBackendClient_InProcess.cc.
  if (frame.hasInspectedElement) {
    result.inspectedElement = frame.inspectedElement;
  }

  // Document viewBox — needed by the host to drive screen↔document
  // math; see the mirroring path in EditorBackendClient_InProcess.cc.
  if (frame.hasDocumentViewBox) {
    result.documentViewBox = Box2d::FromXYWH(frame.documentViewBox[0], frame.documentViewBox[1],
                                             frame.documentViewBox[2], frame.documentViewBox[3]);
  }

  return result;
}

}  // namespace

/// Desktop session-backed client implementation.
class SessionBackedEditorBackendClient : public EditorBackendClient {
public:
  explicit SessionBackedEditorBackendClient(sandbox::SandboxSession& session) : session_(session) {}

  ~SessionBackedEditorBackendClient() override = default;

  // ------------ document / source ------------

  std::future<FrameResult> loadBytes(std::span<const uint8_t> bytes,
                                     std::optional<std::string> originUri) override {
    sandbox::LoadBytesPayload payload;
    payload.bytes = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    payload.originUri = std::move(originUri);
    return submitFrameRequest(SessionOpcode::kLoadBytes, sandbox::EncodeLoadBytes(payload));
  }

  std::future<FrameResult> replaceSource(std::string bytes, bool preserveUndoOnReparse) override {
    sandbox::ReplaceSourcePayload payload;
    payload.bytes = std::move(bytes);
    payload.preserveUndoOnReparse = preserveUndoOnReparse;
    return submitFrameRequest(SessionOpcode::kReplaceSource, sandbox::EncodeReplaceSource(payload));
  }

  std::future<FrameResult> applySourcePatch(uint32_t sourceStart, uint32_t sourceEnd,
                                            std::string newText) override {
    sandbox::ApplySourcePatchPayload payload;
    payload.start = sourceStart;
    payload.end = sourceEnd;
    payload.newText = std::move(newText);
    return submitFrameRequest(SessionOpcode::kApplySourcePatch,
                              sandbox::EncodeApplySourcePatch(payload));
  }

  // ------------ input events ------------

  std::future<FrameResult> pointerEvent(const PointerEventPayload& ev) override {
    sandbox::PointerEventPayload payload;
    payload.phase = ev.phase;
    payload.documentX = ev.documentPoint.x;
    payload.documentY = ev.documentPoint.y;
    payload.buttons = ev.buttons;
    payload.modifiers = ev.modifiers;
    return submitFrameRequest(SessionOpcode::kPointerEvent, sandbox::EncodePointerEvent(payload));
  }

  std::future<FrameResult> keyEvent(const KeyEventPayload& ev) override {
    sandbox::KeyEventPayload payload;
    payload.phase = ev.phase;
    payload.keyCode = ev.keyCode;
    payload.modifiers = ev.modifiers;
    payload.textInput = ev.textInput;
    return submitFrameRequest(SessionOpcode::kKeyEvent, sandbox::EncodeKeyEvent(payload));
  }

  std::future<FrameResult> wheelEvent(const WheelEventPayload& ev) override {
    sandbox::WheelEventPayload payload;
    payload.documentX = ev.documentPoint.x;
    payload.documentY = ev.documentPoint.y;
    payload.deltaX = ev.deltaX;
    payload.deltaY = ev.deltaY;
    payload.modifiers = ev.modifiers;
    return submitFrameRequest(SessionOpcode::kWheelEvent, sandbox::EncodeWheelEvent(payload));
  }

  // ------------ tool + viewport + history ------------

  std::future<FrameResult> setTool(sandbox::ToolKind kind) override {
    sandbox::SetToolPayload payload;
    payload.toolKind = kind;
    return submitFrameRequest(SessionOpcode::kSetTool, sandbox::EncodeSetTool(payload));
  }

  std::future<FrameResult> setViewport(int width, int height) override {
    sandbox::SetViewportPayload payload;
    payload.width = width;
    payload.height = height;
    return submitFrameRequest(SessionOpcode::kSetViewport, sandbox::EncodeSetViewport(payload));
  }

  std::future<FrameResult> attachSharedTexture(
      const sandbox::bridge::BridgeTextureHandle& handle) override {
    sandbox::AttachSharedTexturePayload payload;
    payload.kind = static_cast<uint8_t>(handle.kind);
    payload.handle = handle.handle;
    payload.width = handle.dimensions.x;
    payload.height = handle.dimensions.y;
    payload.rowBytes = handle.rowBytes;
    return submitFrameRequest(SessionOpcode::kAttachSharedTexture,
                              sandbox::EncodeAttachSharedTexture(payload));
  }

  std::future<FrameResult> undo() override {
    return submitFrameRequest(SessionOpcode::kUndo, sandbox::EncodeUndo());
  }

  std::future<FrameResult> redo() override {
    return submitFrameRequest(SessionOpcode::kRedo, sandbox::EncodeRedo());
  }

  // ------------ tree selection ------------

  std::future<FrameResult> selectElement(uint64_t entityId, uint64_t entityGeneration,
                                         uint8_t mode) override {
    sandbox::SelectElementPayload payload;
    payload.entityId = entityId;
    payload.entityGeneration = entityGeneration;
    payload.mode = mode;
    return submitFrameRequest(SessionOpcode::kSelectElement, sandbox::EncodeSelectElement(payload));
  }

  // ------------ export ------------

  std::future<ExportResult> exportDocument(const ExportPayload& payload) override {
    sandbox::ExportRequestPayload req;
    req.format = payload.format;
    auto encodedPayload = sandbox::EncodeExport(req);

    SessionFrame frame;
    frame.requestId = 0;
    frame.opcode = SessionOpcode::kExport;
    frame.payload = std::move(encodedPayload);

    WireRequest wireReq;
    wireReq.bytes = sandbox::EncodeFrame(frame);

    auto wireFuture = session_.submit(std::move(wireReq));

    auto promise = std::make_shared<std::promise<ExportResult>>();
    auto future = promise->get_future();

    std::thread([promise, wireFuture = std::move(wireFuture)]() mutable {
      WireResponse resp = wireFuture.get();
      ExportResult result;
      if (resp.status == sandbox::SandboxStatus::kOk) {
        // resp.bytes is the raw payload (session already stripped the frame header).
        sandbox::ExportResponsePayload exportResp;
        if (sandbox::DecodeExportResponse(resp.bytes, exportResp)) {
          result.ok = true;
          result.format = exportResp.format;
          result.bytes = std::move(exportResp.bytes);
        }
      }
      promise->set_value(std::move(result));
    }).detach();

    return future;
  }

  // ------------ async-push callbacks ------------

  void setToastCallback(ToastCallback cb) override {
    std::lock_guard lock(callbackMutex_);
    toastCallback_ = std::move(cb);
  }

  void setDialogRequestCallback(DialogRequestCallback cb) override {
    std::lock_guard lock(callbackMutex_);
    dialogRequestCallback_ = std::move(cb);
  }

  void setDiagnosticCallback(DiagnosticCallback cb) override {
    std::lock_guard lock(callbackMutex_);
    diagnosticCallback_ = std::move(cb);
    session_.setDiagnosticCallback([this](std::string_view msg) {
      std::lock_guard lock(callbackMutex_);
      if (diagnosticCallback_) {
        diagnosticCallback_(std::string(msg));
      }
    });
  }

  // ------------ read-only state from the latest frame ------------

  uint64_t lastFrameId() const override {
    std::lock_guard lock(stateMutex_);
    return lastFrameId_;
  }

  const SelectionOverlay& selection() const override {
    std::lock_guard lock(stateMutex_);
    return selection_;
  }

  const svg::RendererBitmap& latestBitmap() const override {
    std::lock_guard lock(stateMutex_);
    return latestBitmap_;
  }

  std::optional<Box2d> latestDocumentViewBox() const override {
    std::lock_guard lock(stateMutex_);
    return latestDocumentViewBox_;
  }

  std::optional<ParseDiagnostic> lastParseError() const override {
    std::lock_guard lock(stateMutex_);
    return lastParseError_;
  }

  const sandbox::FrameTreeSummary& tree() const override {
    std::lock_guard lock(stateMutex_);
    return tree_;
  }

  const std::optional<sandbox::InspectedElementSnapshot>& inspectedElement() const override {
    std::lock_guard lock(stateMutex_);
    return inspectedElement_;
  }

  CompositorFastPathCounters compositorFastPathCountersForTesting() const override { return {}; }

private:
  /// Encodes + submits a request that expects a kFrame response, spawning a
  /// worker thread to decode the response and produce a FrameResult.
  std::future<FrameResult> submitFrameRequest(SessionOpcode opcode,
                                              std::vector<uint8_t> encodedPayload) {
    SessionFrame frame;
    frame.requestId = 0;
    frame.opcode = opcode;
    frame.payload = std::move(encodedPayload);

    WireRequest wireReq;
    wireReq.bytes = sandbox::EncodeFrame(frame);

    auto wireFuture = session_.submit(std::move(wireReq));

    auto promise = std::make_shared<std::promise<FrameResult>>();
    auto future = promise->get_future();

    std::thread([this, promise, wireFuture = std::move(wireFuture)]() mutable {
      WireResponse resp = wireFuture.get();

      FrameResult result;
      if (resp.status != sandbox::SandboxStatus::kOk) {
        result.ok = false;
        promise->set_value(std::move(result));
        return;
      }

      // resp.bytes is the raw payload (session already stripped the frame header).
      FramePayload framePayload;
      if (sandbox::DecodeFrame(resp.bytes, framePayload)) {
        result = DecodeFrameResult(framePayload);
        cacheResult(result);
      } else {
        result.ok = false;
      }

      promise->set_value(std::move(result));
    }).detach();

    return future;
  }

  void cacheResult(const FrameResult& result) {
    std::lock_guard lock(stateMutex_);
    lastFrameId_ = result.frameId;
    selection_ = result.selection;
    latestBitmap_ = result.bitmap;
    if (result.documentViewBox.has_value()) {
      latestDocumentViewBox_ = result.documentViewBox;
    }
    tree_ = result.tree;
    inspectedElement_ = result.inspectedElement;
    if (!result.parseDiagnostics.empty()) {
      lastParseError_ = result.parseDiagnostics.front();
    } else {
      lastParseError_.reset();
    }
  }

  sandbox::SandboxSession& session_;

  // Cached state from the latest frame.
  mutable std::mutex stateMutex_;
  uint64_t lastFrameId_ = 0;
  SelectionOverlay selection_;
  svg::RendererBitmap latestBitmap_;
  std::optional<Box2d> latestDocumentViewBox_;
  sandbox::FrameTreeSummary tree_;
  std::optional<sandbox::InspectedElementSnapshot> inspectedElement_;
  std::optional<ParseDiagnostic> lastParseError_;

  // Callbacks.
  mutable std::mutex callbackMutex_;
  ToastCallback toastCallback_;
  DialogRequestCallback dialogRequestCallback_;
  DiagnosticCallback diagnosticCallback_;
};

// Factory.
std::unique_ptr<EditorBackendClient> EditorBackendClient::MakeSessionBacked(
    sandbox::SandboxSession& session) {
  return std::make_unique<SessionBackedEditorBackendClient>(session);
}

}  // namespace donner::editor
