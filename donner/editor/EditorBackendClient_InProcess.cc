/// @file
///
/// In-process implementation of `EditorBackendClient`. Statically constructs an
/// `EditorBackendCore` in the same address space and services each call
/// synchronously. Intended for WASM builds where the browser is the sandbox,
/// but compiles on desktop for testing.
///
/// See docs/design_docs/0023-editor_sandbox.md §S9.

#include "donner/base/Vector2.h"
#include "donner/editor/AddressBarStatus.h"
#include "donner/editor/EditorBackendClient.h"
#include "donner/editor/SelectionOverlay.h"
#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/editor/sandbox/EditorBackendCore.h"
#include "donner/editor/sandbox/SessionProtocol.h"
#include "donner/editor/sandbox/bridge/BridgeTexture.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

namespace {

/// Converts a `FramePayload` into a `FrameResult`. Historically this
/// replayed the backend's render wire into a local `Renderer`; the
/// thin-client architecture instead ships pre-composed bitmap bytes in
/// `frame.finalBitmapPixels` which we just re-wrap. No renderer needed.
FrameResult MakeFrameResult(const sandbox::FramePayload& frame) {
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
    pd.range = {FileOffset::Offset(0), FileOffset::Offset(0)};
    result.parseDiagnostics.push_back(std::move(pd));
  }

  // Tree summary.
  result.tree = frame.tree;

  // Inspector snapshot — present only when exactly one element is
  // selected. Host UI shows an empty inspector otherwise.
  if (frame.hasInspectedElement) {
    result.inspectedElement = frame.inspectedElement;
  }

  // Document viewBox — the SVG-user-space coordinate system bboxes /
  // pointer events travel in. Absent when the backend has no document
  // loaded yet.
  if (frame.hasDocumentViewBox) {
    result.documentViewBox = Box2d::FromXYWH(frame.documentViewBox[0], frame.documentViewBox[1],
                                             frame.documentViewBox[2], frame.documentViewBox[3]);
  }

  return result;
}

}  // namespace

/// In-process client that owns an EditorBackendCore and services calls
/// synchronously without IPC.
class InProcessEditorBackendClient : public EditorBackendClient {
public:
  InProcessEditorBackendClient() = default;
  ~InProcessEditorBackendClient() override = default;

  // ------------ document / source ------------

  std::future<FrameResult> loadBytes(std::span<const uint8_t> bytes,
                                     std::optional<std::string> originUri) override {
    sandbox::LoadBytesPayload payload;
    payload.bytes = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    payload.originUri = std::move(originUri);
    return makeReadyFuture(core_.handleLoadBytes(payload));
  }

  std::future<FrameResult> replaceSource(std::string bytes, bool preserveUndoOnReparse) override {
    sandbox::ReplaceSourcePayload payload;
    payload.bytes = std::move(bytes);
    payload.preserveUndoOnReparse = preserveUndoOnReparse;
    return makeReadyFuture(core_.handleReplaceSource(payload));
  }

  std::future<FrameResult> applySourcePatch(uint32_t sourceStart, uint32_t sourceEnd,
                                            std::string newText) override {
    sandbox::ApplySourcePatchPayload payload;
    payload.start = sourceStart;
    payload.end = sourceEnd;
    payload.newText = std::move(newText);
    return makeReadyFuture(core_.handleApplySourcePatch(payload));
  }

  // ------------ input events ------------

  std::future<FrameResult> pointerEvent(const PointerEventPayload& ev) override {
    sandbox::PointerEventPayload payload;
    payload.phase = ev.phase;
    payload.documentX = ev.documentPoint.x;
    payload.documentY = ev.documentPoint.y;
    payload.buttons = ev.buttons;
    payload.modifiers = ev.modifiers;
    return makeReadyFuture(core_.handlePointerEvent(payload));
  }

  std::future<FrameResult> keyEvent(const KeyEventPayload& ev) override {
    sandbox::KeyEventPayload payload;
    payload.phase = ev.phase;
    payload.keyCode = ev.keyCode;
    payload.modifiers = ev.modifiers;
    payload.textInput = ev.textInput;
    return makeReadyFuture(core_.handleKeyEvent(payload));
  }

  std::future<FrameResult> wheelEvent(const WheelEventPayload& ev) override {
    sandbox::WheelEventPayload payload;
    payload.documentX = ev.documentPoint.x;
    payload.documentY = ev.documentPoint.y;
    payload.deltaX = ev.deltaX;
    payload.deltaY = ev.deltaY;
    payload.modifiers = ev.modifiers;
    return makeReadyFuture(core_.handleWheelEvent(payload));
  }

  // ------------ tool + viewport + history ------------

  std::future<FrameResult> setTool(sandbox::ToolKind kind) override {
    sandbox::SetToolPayload payload;
    payload.toolKind = kind;
    return makeReadyFuture(core_.handleSetTool(payload));
  }

  std::future<FrameResult> setViewport(int width, int height) override {
    sandbox::SetViewportPayload payload;
    payload.width = width;
    payload.height = height;
    return makeReadyFuture(core_.handleSetViewport(payload));
  }

  std::future<FrameResult> attachSharedTexture(
      const sandbox::bridge::BridgeTextureHandle& handle) override {
    sandbox::AttachSharedTexturePayload payload;
    payload.kind = static_cast<uint8_t>(handle.kind);
    payload.handle = handle.handle;
    payload.width = handle.dimensions.x;
    payload.height = handle.dimensions.y;
    payload.rowBytes = handle.rowBytes;
    return makeReadyFuture(core_.handleAttachSharedTexture(payload));
  }

  std::future<FrameResult> undo() override { return makeReadyFuture(core_.handleUndo()); }

  std::future<FrameResult> redo() override { return makeReadyFuture(core_.handleRedo()); }

  // ------------ tree selection ------------

  std::future<FrameResult> selectElement(uint64_t entityId, uint64_t entityGeneration,
                                         uint8_t mode) override {
    sandbox::SelectElementPayload payload;
    payload.entityId = entityId;
    payload.entityGeneration = entityGeneration;
    payload.mode = mode;
    return makeReadyFuture(core_.handleSelectElement(payload));
  }

  // ------------ export ------------

  std::future<ExportResult> exportDocument(const ExportPayload& payload) override {
    sandbox::ExportRequestPayload req;
    req.format = payload.format;
    auto resp = core_.handleExport(req);

    ExportResult result;
    result.ok = true;
    result.format = resp.format;
    result.bytes = std::move(resp.bytes);

    std::promise<ExportResult> promise;
    promise.set_value(std::move(result));
    return promise.get_future();
  }

  // ------------ async-push callbacks ------------

  void setToastCallback(ToastCallback cb) override { toastCallback_ = std::move(cb); }
  void setDialogRequestCallback(DialogRequestCallback cb) override {
    dialogRequestCallback_ = std::move(cb);
  }
  void setDiagnosticCallback(DiagnosticCallback cb) override {
    diagnosticCallback_ = std::move(cb);
  }

  // ------------ read-only state from the latest frame ------------

  uint64_t lastFrameId() const override { return lastFrameId_; }
  const SelectionOverlay& selection() const override { return selection_; }
  const svg::RendererBitmap& latestBitmap() const override { return latestBitmap_; }
  std::optional<Box2d> latestDocumentViewBox() const override { return latestDocumentViewBox_; }
  std::optional<ParseDiagnostic> lastParseError() const override { return lastParseError_; }
  const sandbox::FrameTreeSummary& tree() const override { return tree_; }
  const std::optional<sandbox::InspectedElementSnapshot>& inspectedElement() const override {
    return inspectedElement_;
  }
  CompositorFastPathCounters compositorFastPathCountersForTesting() const override {
    const auto counters = core_.compositorFastPathCountersForTesting();
    return CompositorFastPathCounters{
        .fastPathFrames = counters.fastPathFrames,
        .slowPathFramesWithDirty = counters.slowPathFramesWithDirty,
        .noDirtyFrames = counters.noDirtyFrames,
    };
  }

private:
  /// Processes a FramePayload synchronously and returns an already-ready future.
  std::future<FrameResult> makeReadyFuture(const sandbox::FramePayload& framePayload) {
    FrameResult result = MakeFrameResult(framePayload);
    cacheResult(result);

    std::promise<FrameResult> promise;
    promise.set_value(std::move(result));
    return promise.get_future();
  }

  void cacheResult(const FrameResult& result) {
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

  sandbox::EditorBackendCore core_;

  uint64_t lastFrameId_ = 0;
  SelectionOverlay selection_;
  svg::RendererBitmap latestBitmap_;
  std::optional<Box2d> latestDocumentViewBox_;
  sandbox::FrameTreeSummary tree_;
  std::optional<sandbox::InspectedElementSnapshot> inspectedElement_;
  std::optional<ParseDiagnostic> lastParseError_;

  ToastCallback toastCallback_;
  DialogRequestCallback dialogRequestCallback_;
  DiagnosticCallback diagnosticCallback_;
};

// Factory.
std::unique_ptr<EditorBackendClient> EditorBackendClient::MakeInProcess() {
  return std::make_unique<InProcessEditorBackendClient>();
}

}  // namespace donner::editor
