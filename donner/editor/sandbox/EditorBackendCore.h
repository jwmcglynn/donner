#pragma once
/// @file
///
/// Shared dispatch core for the editor backend. Used by both the standalone
/// `donner_editor_backend` child process and the in-process WASM client so
/// the two paths cannot diverge. Owns an `EditorApp`, viewport state, and
/// frame-id counter.
///
/// See docs/design_docs/0023-editor_sandbox.md §S8–S9.

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "donner/editor/backend_lib/EditorApp.h"
#include "donner/editor/backend_lib/SelectTool.h"
#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/editor/sandbox/bridge/BridgeTexture.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor::sandbox {

/// Reusable dispatch core that services session-protocol opcodes.
///
/// The core is deliberately transport-agnostic: it consumes decoded payload
/// structs and produces encoded response payloads. The surrounding harness
/// (standalone binary or in-process client) handles framing and I/O.
class EditorBackendCore {
public:
  EditorBackendCore();
  ~EditorBackendCore();

  EditorBackendCore(const EditorBackendCore&) = delete;
  EditorBackendCore& operator=(const EditorBackendCore&) = delete;

  // -----------------------------------------------------------------------
  // Opcode handlers — each returns a FramePayload for the current state.
  // -----------------------------------------------------------------------

  /// Renders the current editor state into a `FramePayload`.
  [[nodiscard]] FramePayload buildFramePayload();

  /// Handles kSetViewport. Updates viewport dims if valid.
  [[nodiscard]] FramePayload handleSetViewport(const SetViewportPayload& vp);

  /// Handles kLoadBytes. Loads the document from raw bytes.
  [[nodiscard]] FramePayload handleLoadBytes(const LoadBytesPayload& load);

  /// Handles kReplaceSource. Replaces the source and reparses.
  [[nodiscard]] FramePayload handleReplaceSource(const ReplaceSourcePayload& rep);

  /// Handles kApplySourcePatch. Placeholder — returns current state.
  [[nodiscard]] FramePayload handleApplySourcePatch(const ApplySourcePatchPayload& patch);

  /// Handles kPointerEvent. Performs hit testing on pointer-down.
  [[nodiscard]] FramePayload handlePointerEvent(const PointerEventPayload& ptr);

  /// Handles kKeyEvent. Placeholder — returns current state.
  [[nodiscard]] FramePayload handleKeyEvent(const KeyEventPayload& key);

  /// Handles kWheelEvent. Placeholder — returns current state.
  [[nodiscard]] FramePayload handleWheelEvent(const WheelEventPayload& wheel);

  /// Handles kSetTool. Placeholder — returns current state.
  [[nodiscard]] FramePayload handleSetTool(const SetToolPayload& tool);

  /// Handles kSelectElement. Resolves an entity from the wire handle, applies
  /// the requested selection mode, and returns a frame.
  [[nodiscard]] FramePayload handleSelectElement(const SelectElementPayload& sel);

  /// Handles kUndo.
  [[nodiscard]] FramePayload handleUndo();

  /// Handles kRedo.
  [[nodiscard]] FramePayload handleRedo();

  /// Handles kExport. Placeholder — returns empty export response payload.
  [[nodiscard]] ExportResponsePayload handleExport(const ExportRequestPayload& req);

  /// Handles kAttachSharedTexture. Imports the host-allocated shared GPU
  /// texture via `bridge::MakeBackend_macOS` (or the platform variant),
  /// storing the result for subsequent `buildFramePayload` calls to use
  /// as a render target. Returns a one-shot frame carrying the ack —
  /// no bitmap yet since the first real render hasn't fired.
  [[nodiscard]] FramePayload handleAttachSharedTexture(const AttachSharedTexturePayload& attach);

  /// Direct access to the underlying EditorApp (for tests).
  [[nodiscard]] EditorApp& editor() { return editor_; }

  /// Current viewport dimensions.
  [[nodiscard]] int viewportWidth() const { return viewportWidth_; }
  [[nodiscard]] int viewportHeight() const { return viewportHeight_; }

private:
  /// Bump entity-handle generation, invalidating all outstanding wire handles.
  void bumpEntityGeneration();

  /// Walk the document tree and populate \p tree.
  void populateTreeSummary(FrameTreeSummary& tree);

  /// Get or assign a stable wire id for the given entity. Also records the
  /// SVGElement so it can be resolved later.
  uint64_t entityIdFor(entt::entity entity, const svg::SVGElement& element);

  /// Resolve a wire handle back to an SVGElement, or nullopt if stale.
  [[nodiscard]] std::optional<svg::SVGElement> resolveElement(uint64_t entityId,
                                                              uint64_t entityGeneration) const;

  EditorApp editor_;
  /// Owns the drag/marquee/hover state for pointer events. Dispatched
  /// in `handlePointerEvent` (k{Down,Move,Up}) so dragging a selection
  /// actually moves it — without this, the backend's default pointer
  /// handler only hit-tested and called `setSelection`, leaving drag
  /// dead. Sharing a single `SelectTool` across events matches how
  /// `EditorShell` drives it on main.
  SelectTool selectTool_;
  /// Real CPU rasterizer. The backend now owns the pixel output rather
  /// than emitting a wire for the host to replay — shipping pre-
  /// composed bitmaps is what lets `CompositorController` below
  /// actually save work during drag (its layer bitmap cache survives
  /// across frames, so a drag's translation-only fast path skips the
  /// shape re-raster entirely). See `docs/design_docs/0023-editor
  /// _sandbox.md` §"Rendering data flow" G6b.
  donner::svg::Renderer renderer_;
  /// Separate renderer used to rasterize selection chrome (path
  /// outlines, AABB box, marquee) into its own transparent bitmap.
  /// Kept distinct from `renderer_` because the Geode backend's
  /// `drawRect` calls are only valid between `beginFrame` /
  /// `endFrame` against the renderer's render-target texture — the
  /// compositor's `renderFrame` has already ended the frame by the
  /// time chrome is drawn, so sharing the main renderer drops the
  /// chrome draws on Geode. With a dedicated overlay renderer each
  /// chrome pass opens its own frame, and the result is
  /// software-composited onto the compositor's snapshot below.
  donner::svg::Renderer overlayRenderer_;
  /// Lazily constructed on first render after a document change. Tied
  /// to `editor_`'s current `SVGDocument` handle; invalidated via
  /// `compositor_.reset()` whenever the document is reparsed / replaced
  /// so the next render rebuilds against the new registry.
  std::optional<donner::svg::compositor::CompositorController> compositor_;

public:
  /// Test-only: read the compositor's fast-path counters so regression
  /// tests can assert that a drag sequence actually took the fast path
  /// instead of relying on wall-clock budgets that flake under CI
  /// scheduling noise.
  [[nodiscard]] donner::svg::compositor::CompositorController::FastPathCounters
  compositorFastPathCountersForTesting() const {
    if (!compositor_.has_value()) {
      return {};
    }
    return compositor_->fastPathCountersForTesting();
  }

private:
  /// Entity currently promoted on the compositor, or `entt::null`.
  /// Maintained to match the SelectTool's current drag / selection
  /// target so promote/demote calls stay O(changes) not O(frames).
  Entity compositorEntity_ = entt::null;
  int viewportWidth_ = 512;
  int viewportHeight_ = 384;
  /// What we last called `SVGDocument::setCanvasSize` with. Used to
  /// skip the call when the request hasn't changed — `setCanvasSize`
  /// unconditionally `invalidateRenderTree()`s, which drops us out of
  /// the compositor's translation-only fast path. Can't use
  /// `doc.canvasSize()` for the compare because the document reports
  /// the aspect-fit-scaled result, not the request we made.
  Vector2i lastSetCanvasSize_ = Vector2i(-1, -1);

  /// Host-provided shared GPU texture the backend renders into when
  /// available (`bridge_->ready() == true`). Null until the host
  /// sends `kAttachSharedTexture`; on platforms where wgpu-native
  /// can't yet import the handle (every platform as of this commit
  /// — see the design doc's §"Implementation status") the bridge
  /// reports `ready() == false` and we fall through to shipping
  /// `finalBitmapPixels` in the wire.
  std::unique_ptr<bridge::BridgeTextureBackend> bridge_;
  uint64_t frameIdCounter_ = 1;

  /// Entity handle bimap state.
  uint64_t entityGeneration_ = 1;
  uint64_t nextEntityId_ = 1;
  std::unordered_map<entt::entity, uint64_t> entityToId_;
  std::unordered_map<uint64_t, svg::SVGElement> idToElement_;
};

}  // namespace donner::editor::sandbox
