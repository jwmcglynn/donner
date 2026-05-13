#pragma once
/// @file
///
/// `AsyncRenderer` owns a `svg::Renderer` and runs its `draw()` +
/// `takeSnapshot()` on a dedicated worker thread so heavy renders
/// don't block the UI thread.
///
/// ## Threading model
///
/// The worker thread owns the Renderer for its entire lifetime — the
/// Renderer is constructed on the worker thread at startup and
/// destroyed on the worker thread at shutdown. This is load-bearing
/// for the Geode (WebGPU) backend under Emscripten pthreads: WebGPU
/// JS objects are per-worker, so the device, pipelines, textures,
/// and readback buffers must all be created and used on a single
/// thread. See the "first render aborts with `getJsObject` assertion"
/// incident that motivated moving renderer ownership here.
///
/// The worker additionally takes exclusive ownership of the
/// `SVGDocument` during an active render. The UI thread must not
/// mutate the document while a render is in flight.
///
/// UI thread flow per frame:
/// 1. `pollResult()` — if a render just finished, pick up the bitmap.
/// 2. If NOT busy: process mutations via `flushFrame()`.
/// 3. If NOT busy AND a new render is needed: `requestRender()`.
/// 4. If busy: skip flushFrame, leave pending mutations in the queue.
///    They apply on the next idle frame. Input (drags, typing) still
///    gets processed and queued — just not dispatched to the ECS.
///
/// The safety invariant: between `requestRender()` and a non-`nullopt`
/// return from `pollResult()`, the UI thread must not mutate the
/// `SVGDocument`, and must not touch state the overlay renderer reads
/// (selection, etc. — those are snapshotted at request time, see
/// `RenderRequest`). The UI thread must not call any method on the
/// `Renderer` at any time — it lives on the worker.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {
class SVGDocument;
namespace compositor {
class CompositorController;
}
}  // namespace donner::svg

namespace donner::editor {

/// Per-request handoff data captured at render-request time so the
/// worker has everything it needs without touching live UI state.
struct RenderRequest {
  struct DragPreview {
    Entity entity = entt::null;
    /// Which interaction phase drove this preview. `Selection` means the
    /// editor is pre-warming a layer for the selected entity before any
    /// drag begins. `ActiveDrag` means the user is actively dragging — the
    /// DOM's transform attribute already reflects the cursor delta, so no
    /// extra translation is passed here. The compositor stamps the correct
    /// `InteractionHint` on the entity based on this field so downstream
    /// introspection stays accurate.
    svg::compositor::InteractionHint interactionKind = svg::compositor::InteractionHint::ActiveDrag;
  };

  svg::Renderer* renderer = nullptr;
  svg::SVGDocument* document = nullptr;
  /// Document frame version snapshotted at request time so the UI can
  /// match the landed bitmap with other same-version assets.
  std::uint64_t version = 0;
  /// Generation counter from `AsyncSVGDocument::documentGeneration()`.
  /// Only bumps when the inner document is fully replaced (source-pane
  /// reparse / load). The worker uses this to decide when to drop the
  /// compositor's activeHints_ backing the old entity space — NOT
  /// `version`, which bumps on every mutation and would nuke the
  /// compositor every drag frame.
  std::uint64_t documentGeneration = 0;
  /// When non-empty, carries a remap from the previous document's entity
  /// ids to the current document's entity ids — produced by
  /// `AsyncSVGDocument::setDocumentMaybeStructural` when the swap was
  /// structurally equivalent (same tree shape, same ids). The worker
  /// uses the remap to call `CompositorController::remapAfterStructural
  /// Replace` instead of `resetAllLayers(documentReplaced=true)`,
  /// preserving layer bitmaps + segments across the swap. Empty map
  /// means "fall through to the `documentGeneration` mismatch path"
  /// (full reset).
  std::unordered_map<Entity, Entity> structuralRemap;
  /// Snapshot of the selection at request time (used for overlay chrome).
  /// The worker holds this optional by value, so if the UI thread clears
  /// the selection mid-render the worker still draws the pre-render chrome.
  std::optional<svg::SVGElement> selection;
  /// Currently-selected entity (if any) that the compositor should keep
  /// promoted across renders. The compositor demotes the previous entity and
  /// promotes this one when it changes. Allows pre-warming on selection so
  /// the subsequent drag frame has cached bg/fg/layer bitmaps ready. The
  /// compositor stays alive across drag → idle transitions for as long as
  /// this stays non-null and pointing at the same entity.
  Entity selectedEntity = entt::null;
  /// Optional in-progress drag preview rendered through the compositor fast path.
  std::optional<DragPreview> dragPreview;
};

/// Bitmap plus the document version it was rendered from.
struct RenderResult {
  /// One composite tile from the worker's `CompositorController::
  /// snapshotCompositorTiles()` snapshot (design doc 0033 §M2C). The
  /// editor uploads one GL texture per tile (keyed on `id`) and
  /// blits each tile at its canvas offset, replacing the legacy
  /// `bg`/`promoted`/`fg` triple. Geometry fields are doc-unit
  /// quantities so the editor can scale them by the *current*
  /// `pixelsPerDocUnit` and the bitmap follows pinch-zoom changes
  /// during canvas-resize debouncing (same rationale as the M2B
  /// `promotedBitmapDimsDoc` field this replaces).
  struct CompositedTile {
    enum class Kind : std::uint8_t { Segment, Layer };

    Kind kind = Kind::Segment;
    /// Stable id from the compositor — `"seg:{i}"` or
    /// `"layer:{entity}"`. The editor's per-tile texture cache uses
    /// this to reuse GL textures across frames when the tile's
    /// `generation` hasn't bumped.
    std::string id;
    /// Monotonic generation from the compositor. Editor re-uploads
    /// the bitmap only when this advances.
    std::uint64_t generation = 0;
    /// Source bitmap; uploaded as the tile's GL texture content.
    svg::RendererBitmap bitmap;
    /// Canvas-space top-left of `bitmap`, in document units. Editor
    /// multiplies by `pixelsPerDocUnit` to get the on-screen blit
    /// origin.
    Vector2d canvasOffsetDoc = Vector2d::Zero();
    /// Bitmap's intrinsic dimensions, in document units. Editor
    /// multiplies by current `pixelsPerDocUnit` to get the on-screen
    /// blit size — keeps the bitmap stretching with pinch-zoom while
    /// the canvas-size commit is debounced.
    Vector2d bitmapDimsDoc = Vector2d::Zero();
    /// For drag-target tiles, the per-frame DOM translation in doc
    /// units (the delta between the bitmap's rasterize-time DOM
    /// transform and the entity's current DOM transform). Editor
    /// adds this to `canvasOffsetDoc` so the dragged tile slides in
    /// real time without re-rasterizing.
    Vector2d dragTranslationDoc = Vector2d::Zero();
    /// True when this tile is the active drag target. Useful for
    /// pre-test inspection; the editor's blit math treats drag and
    /// non-drag tiles uniformly via `dragTranslationDoc`.
    bool isDragTarget = false;
  };

  struct CompositedPreview {
    /// Paint-order tile list. Blit in `tiles` order: each tile gets
    /// one `AddImage` call at `(canvasOffsetDoc + dragTranslationDoc)
    /// * pixelsPerDocUnit` with size `bitmapDimsDoc *
    /// pixelsPerDocUnit`.
    std::vector<CompositedTile> tiles;
    /// Active drag-target entity (for selection chrome routing). May
    /// be `entt::null` if no entity is currently being dragged.
    Entity entity = entt::null;

    [[nodiscard]] bool valid() const { return !tiles.empty(); }
  };

  svg::RendererBitmap bitmap;
  std::optional<CompositedPreview> compositedPreview;
  std::uint64_t version = 0;
  /// Wall-clock milliseconds the worker spent inside
  /// `CompositorController::renderFrame` for this iteration. Reported so
  /// the editor can plot backend render time alongside ImGui frame time
  /// on the frame graph. Zero means "no backend work recorded" (e.g. the
  /// request had a null renderer/document and fell through the early-out
  /// branch).
  double workerMs = 0.0;
};

class AsyncRenderer {
public:
  AsyncRenderer();
  ~AsyncRenderer();

  AsyncRenderer(const AsyncRenderer&) = delete;
  AsyncRenderer& operator=(const AsyncRenderer&) = delete;
  AsyncRenderer(AsyncRenderer&&) = delete;
  AsyncRenderer& operator=(AsyncRenderer&&) = delete;

  /// Returns true while a render is in flight on the worker thread.
  /// The UI thread must not touch the `Renderer` or mutate the
  /// `SVGDocument` while this returns true.
  [[nodiscard]] bool isBusy() const { return busy_.load(std::memory_order_acquire); }

  /// Post a render request to the worker. Non-blocking. Transitions
  /// the worker from idle → busy. Caller must have checked `!isBusy()`
  /// first; calling while busy is a programmer error.
  void requestRender(const RenderRequest& request);

  /// If a render has completed since the last call, returns the
  /// resulting bitmap and transitions the worker back to idle. Returns
  /// `std::nullopt` if no render is pending-ready (either still busy
  /// or idle with nothing to hand off).
  std::optional<RenderResult> pollResult();

  /// Install a callback that the worker thread invokes when a render
  /// transitions to Done. Used by the editor's on-demand render loop
  /// to wake the UI thread (e.g. via `glfwPostEmptyEvent`) so the
  /// fresh result gets picked up without continuous polling.
  ///
  /// Callers that never set a callback are unaffected — the default
  /// behavior (worker stores result in `result_`, UI thread polls in
  /// its own cadence) remains exactly as before.
  ///
  /// The callback runs on the worker thread. It must be thread-safe
  /// and must NOT re-enter the renderer — a simple wake-up post into
  /// the window's event queue is the intended use.
  void setWakeCallback(std::function<void()> callback);

  /// Toggle whether the compositor uses tight-bounded segment
  /// rasterization (design doc 0027). The change applies at the start
  /// of the next worker iteration — `renderFrame` calls
  /// `CompositorController::setTightBoundedSegmentsEnabled` before
  /// compositing, which marks all cached segments dirty so the flip
  /// takes full effect that frame.
  ///
  /// Safe to call from the UI thread while a render is in flight; the
  /// flag is stored in an `std::atomic<bool>`, and the worker reads it
  /// at a well-defined point in each iteration.
  void setTightBoundedSegmentsEnabled(bool enabled) {
    tightBoundedSegments_.store(enabled, std::memory_order_release);
  }

  /// Mirror of the current toggle state. UI reads this to render the
  /// correct check state in the View menu without racing the worker.
  [[nodiscard]] bool tightBoundedSegmentsEnabled() const {
    return tightBoundedSegments_.load(std::memory_order_acquire);
  }

  /// Number of times the worker has called `CompositorController::resetAllLayers()`
  /// since construction. Exposed for regression tests that want to assert the
  /// compositor is NOT torn down on every mutation — the bug that made drag-
  /// frame version bumps masquerade as document replacements in the
  /// pre-`documentGeneration_` design.
  ///
  /// Counts only resets driven by a `request.documentGeneration` mismatch; not
  /// the implicit reset performed on first compositor construction.
  ///
  /// Safe to read from the UI thread; incremented under the internal mutex on
  /// the worker.
  [[nodiscard]] std::uint64_t compositorResetCountForTesting() const {
    return compositorResetCount_.load(std::memory_order_acquire);
  }

  /// Snapshot of the compositor's fast-path counters. Read-only — the worker
  /// writes them under the mutex when transitioning to Done. Returns zeros
  /// before the compositor is constructed (first render not yet requested).
  /// UI-thread safe.
  [[nodiscard]] svg::compositor::CompositorController::FastPathCounters
  compositorFastPathCountersForTesting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastFastPathCounters_;
  }

  /// Snapshot of the compositor's per-layer diagnostic rows (design doc
  /// 0033 M1). Captured under the worker mutex at every Done transition;
  /// the UI thread copies the cached vector out under the lock. Empty
  /// before the first render lands or when the compositor isn't
  /// instantiated.
  [[nodiscard]] std::vector<svg::compositor::CompositorController::LayerInspectorRow>
  compositorLayerInspectorRows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastLayerInspectorRows_;
  }

  /// Snapshot of the compositor's per-segment diagnostic rows. Same
  /// capture point and locking as the per-layer rows.
  [[nodiscard]] std::vector<svg::compositor::CompositorController::SegmentInspectorRow>
  compositorSegmentInspectorRows() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastSegmentInspectorRows_;
  }

  /// Unified in-paint-order snapshot of every tile the compositor
  /// blits to produce the final composite (design doc 0033 §M1++).
  /// The editor's layer-inspector panel renders this list with
  /// thumbnails for every tile so the operator can see the
  /// comprehensive composite at a glance.
  [[nodiscard]] std::vector<svg::compositor::CompositorController::CompositeTileSnapshot>
  compositorCompositeTiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastCompositeTiles_;
  }

  /// Compositor-wide diagnostic state: active-hints count, layer
  /// count, split-path active flag, drag-target entity, canvas
  /// size. The editor's layer-inspector panel renders this as a
  /// state header so the operator can spot mismatches between the
  /// editor's idea of the drag target and the compositor's.
  [[nodiscard]] svg::compositor::CompositorController::StateSnapshot compositorState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastStateSnapshot_;
  }

  /// The worker's current view of which entity is promoted. Read on
  /// the UI thread; the worker updates it under the same mutex as the
  /// other snapshot fields when transitioning to Done. `entt::null`
  /// when the worker hasn't promoted anything (e.g. promotion was
  /// refused). Compare against the editor's selection to spot races
  /// between the editor's `selectedEntity` and what the compositor
  /// actually holds.
  [[nodiscard]] Entity workerCompositorEntity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastWorkerCompositorEntity_;
  }

  /// Canvas size in the request the worker is currently processing
  /// (read from `request.document->canvasSize()`). Surfaces the
  /// document's actual canvas size at the worker's last-completed
  /// render, separate from the compositor's `staticSegmentsCanvas_`
  /// (which is the size of the last successful rasterize). When
  /// `documentCanvasAtLastDispatch != compositorCanvas`, the doc was
  /// re-sized but the compositor hasn't re-rasterized at the new
  /// size yet.
  [[nodiscard]] Vector2i lastDocumentCanvasSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastDocumentCanvasSize_;
  }

private:
  void workerLoop();

  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  enum class State : std::uint8_t {
    Idle,     ///< No work. Worker is blocked on `cv_`.
    Busy,     ///< Render in progress on the worker.
    Done,     ///< Render finished; bitmap available in `result_`.
    Shutdown  ///< Destructor requested shutdown; worker exits.
  };

  State state_ = State::Idle;
  std::atomic<bool> busy_{false};

  RenderRequest pendingRequest_;
  RenderResult result_;
  /// Optional UI-thread wake-up hook, invoked by the worker when a
  /// render finishes. Set once at editor startup; owned by the
  /// installer. Held under `mutex_` so mutation vs. invocation races
  /// are impossible.
  std::function<void()> wakeCallback_;
  std::unique_ptr<svg::compositor::CompositorController> compositor_;
  /// The `SVGDocument` this compositor is currently configured for. Stored by
  /// value — `SVGDocument` is a thin value-facade over a `std::shared_ptr<Registry>`
  /// (see `SVGDocumentHandle`), so copying is a refcount bump, not a deep copy
  /// of the document state. `nullopt` before the first render request; set to
  /// a copy of `request.document` on the first iteration and any time the
  /// underlying document handle changes.
  ///
  /// Identity comparison uses `handle().get()` against the incoming request's
  /// document — two `SVGDocument` values wrapping the same `std::shared_ptr<
  /// Registry>` compare equal, which is the right "same document" semantic.
  std::optional<svg::SVGDocument> compositorDocument_;
  svg::Renderer* compositorRenderer_ = nullptr;
  Entity compositorEntity_ = entt::null;
  /// Kind under which `compositorEntity_` is currently promoted. Tracked
  /// alongside the entity so a Selection→ActiveDrag transition refreshes
  /// the hint in place instead of demote-then-re-promote (which would
  /// drop the layer's cached bitmap and tank fast-path engagement on the
  /// first drag move after a click).
  svg::compositor::InteractionHint compositorInteractionKind_ =
      svg::compositor::InteractionHint::Selection;
  /// Document generation at the time the compositor was last configured.
  /// Detects document replacement (ReplaceDocumentCommand / source reparse)
  /// that invalidates all entity handles. Crucially tracks the generation
  /// counter, not the frame version — the frame version bumps on every
  /// mutation (every drag frame) and would falsely trigger a full
  /// `resetAllLayers` every time.
  std::uint64_t compositorDocumentGeneration_ = 0;

  /// Counter of worker-side `resetAllLayers()` invocations. Used by regression
  /// tests to verify that drag-frame mutations (which bump `frameVersion_`) do
  /// NOT fire a reset — only a true `documentGeneration` change does.
  std::atomic<std::uint64_t> compositorResetCount_{0};

  /// Most recent snapshot of the compositor's fast-path counters, copied
  /// under `mutex_` when the worker finishes each render. UI-thread reads
  /// this via `compositorFastPathCountersForTesting`. Mutable because we
  /// lock in a const method.
  svg::compositor::CompositorController::FastPathCounters lastFastPathCounters_;

  /// Most recent per-layer diagnostic snapshot, captured under `mutex_`
  /// at every Done transition. UI-thread reads this via
  /// `compositorLayerInspectorRows()`. Empty before the first render.
  std::vector<svg::compositor::CompositorController::LayerInspectorRow> lastLayerInspectorRows_;

  /// Most recent per-segment diagnostic snapshot. Captured / read on
  /// the same code path as `lastLayerInspectorRows_`.
  std::vector<svg::compositor::CompositorController::SegmentInspectorRow> lastSegmentInspectorRows_;

  /// Most recent unified composite-tile snapshot (in paint order).
  std::vector<svg::compositor::CompositorController::CompositeTileSnapshot> lastCompositeTiles_;

  /// Most recent compositor state snapshot (active-hints count,
  /// split-path active flag, etc.). Captured under `mutex_` at the
  /// Done-transition site alongside the other snapshots.
  svg::compositor::CompositorController::StateSnapshot lastStateSnapshot_;

  /// Snapshot of the worker's `compositorEntity_` at the last Done
  /// transition. Surfaces the worker's view of "which entity is
  /// currently promoted" to the editor's diagnostic panel.
  Entity lastWorkerCompositorEntity_ = entt::null;

  /// Canvas size from `request.document->canvasSize()` at the last
  /// completed render. Diagnostic — compared against the
  /// compositor's `staticSegmentsCanvas_` to spot "document was
  /// re-sized but compositor hasn't caught up yet".
  Vector2i lastDocumentCanvasSize_ = Vector2i::Zero();

  /// Runtime kill-switch for tight-bounded segment rasterization. Pushed
  /// into `CompositorController` at the start of each worker iteration.
  /// Default-true matches `CompositorConfig::tightBoundedSegments`. See
  /// `setTightBoundedSegmentsEnabled`.
  std::atomic<bool> tightBoundedSegments_{true};
};

}  // namespace donner::editor
