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
#include <thread>
#include <unordered_map>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Vector2.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/compositor/ScopedCompositorHint.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {
class SVGDocument;
namespace compositor {
class CompositorController;
}
}

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
    svg::compositor::InteractionHint interactionKind =
        svg::compositor::InteractionHint::ActiveDrag;
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
  struct CompositedPreview {
    svg::RendererBitmap backgroundBitmap;
    svg::RendererBitmap promotedBitmap;
    svg::RendererBitmap foregroundBitmap;
    Entity entity = entt::null;
    /// Compositor-reported translation (in document-space units) to apply when
    /// drawing `promotedBitmap` independently of bg/fg — i.e. the delta between
    /// the bitmap's rasterize-time DOM transform and the entity's current DOM
    /// transform. For the split-layer display path, the editor blits the
    /// promoted bitmap at this offset so it aligns with bg/fg (which were
    /// rendered against the current DOM state, with the promoted entity
    /// hidden).
    Vector2d promotedTranslationDoc = Vector2d::Zero();

    [[nodiscard]] bool valid() const {
      return entity != entt::null && !promotedBitmap.empty();
    }
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

private:
  void workerLoop();

  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  enum class State : std::uint8_t {
    Idle,    ///< No work. Worker is blocked on `cv_`.
    Busy,    ///< Render in progress on the worker.
    Done,    ///< Render finished; bitmap available in `result_`.
    Shutdown ///< Destructor requested shutdown; worker exits.
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
  svg::SVGDocument* compositorDocument_ = nullptr;
  svg::Renderer* compositorRenderer_ = nullptr;
  Entity compositorEntity_ = entt::null;
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
};

}  // namespace donner::editor
