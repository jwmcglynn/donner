#include "donner/editor/AsyncRenderer.h"

#include <chrono>

#include "donner/editor/OverlayRenderer.h"
#include "donner/editor/TracyWrapper.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/compositor/CompositorController.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

AsyncRenderer::AsyncRenderer() {
  thread_ = std::thread([this] { workerLoop(); });
}

AsyncRenderer::~AsyncRenderer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = State::Shutdown;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void AsyncRenderer::requestRender(const RenderRequest& request) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Caller contract: must have checked !isBusy() first. If we're
    // currently in Done state (a prior result the caller never picked
    // up), we drop it — the new request supersedes it.
    pendingRequest_ = request;
    state_ = State::Busy;
  }
  busy_.store(true, std::memory_order_release);
  cv_.notify_one();
}

std::optional<RenderResult> AsyncRenderer::pollResult() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::Done) {
    return std::nullopt;
  }
  state_ = State::Idle;
  busy_.store(false, std::memory_order_release);
  return std::move(result_);
}

void AsyncRenderer::setWakeCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  wakeCallback_ = std::move(callback);
}

void AsyncRenderer::workerLoop() {
  // Construct the Renderer on the worker thread so all its backend-owned
  // resources (WebGPU device/queue/textures under Geode, etc.) are
  // created and used from a single thread. Under
  // Emscripten pthreads this is load-bearing: `WebGPU.Internals.jsObjects`
  // is per-worker, and crossing thread boundaries triggers a
  // `getJsObject` assert on the first `wgpuDeviceCreateTexture`.
  svg::Renderer renderer;

  while (true) {
    RenderRequest request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return state_ == State::Busy || state_ == State::Shutdown; });
      if (state_ == State::Shutdown) {
        // `renderer` is destroyed here as the local unwinds, i.e. on
        // the worker thread — mirroring its construction.
        return;
      }
      request = pendingRequest_;
    }

    // Execute the render outside the lock so the UI thread can poll
    // `isBusy()` / `pollResult()` while we work.
    if (request.renderer != nullptr && request.document != nullptr) {
      ZoneScopedN("AsyncRenderer::workerIteration");
      std::optional<RenderResult::CompositedPreview> compositedPreview;

      // Keep the compositor alive across drag → idle transitions. Recreate
      // only when the underlying document or renderer changes (different
      // editor session / backend). This preserves mandatory-filter / bucket
      // layer bitmap caches and the pre-warmed bg/fg pair between the
      // selection pre-warm and the first drag frame that follows.
      if (!compositor_ || compositorDocument_ != request.document ||
          compositorRenderer_ != request.renderer) {
        compositor_ = std::make_unique<svg::compositor::CompositorController>(*request.document,
                                                                              *request.renderer);
        compositor_->setSkipMainComposeDuringSplit(true);
        compositorDocument_ = request.document;
        compositorRenderer_ = request.renderer;
        compositorEntity_ = entt::null;
        compositorDocumentGeneration_ = request.documentGeneration;
      }

      // Detect *document replacement* (ReplaceDocumentCommand / source
      // reparse). The inner SVGDocument lives in `AsyncSVGDocument`'s
      // optional storage, so its address is stable across replacement —
      // pointer identity won't catch it. `documentGeneration` bumps only
      // on replacement (NOT on every mutation), so comparing against
      // the compositor's snapshot correctly distinguishes "entity space
      // blown away" from "user dragged one element". Hitting this branch
      // every frame (as would happen if we tracked `version` instead)
      // would nuke activeHints_ on every drag, demote the drag layer,
      // and crash in `~ScopedCompositorHint` when a subsequent rebuild
      // leaves the registry in a transient state.
      if (request.documentGeneration != compositorDocumentGeneration_) {
        // A `setDocument` happened since our last tick. Two sub-cases:
        //
        //   1. The editor built a structural remap (`request.structural
        //      Remap` non-empty) — the new document describes the same
        //      tree shape, so we can preserve the compositor's cached
        //      bitmaps + segments and just swap entity ids via
        //      `remapAfterStructuralReplace`. If the remap fails an
        //      invariant check, fall through to the full-reset path.
        //
        //   2. Otherwise (user edited source-pane to change the tree,
        //      new file loaded, etc.): full reset — the old entity
        //      space is gone and every cache is keyed on dead ids.
        //      `resetAllLayers(documentReplaced=true)` defuses the
        //      ScopedCompositorHint dtors so they don't SIGSEGV.
        bool remapped = false;
        if (!request.structuralRemap.empty()) {
          remapped = compositor_->remapAfterStructuralReplace(request.structuralRemap);
          if (remapped && compositorEntity_ != entt::null) {
            const auto it = request.structuralRemap.find(compositorEntity_);
            if (it != request.structuralRemap.end()) {
              compositorEntity_ = it->second;
            } else {
              // The drag/selection target didn't survive the remap — fall
              // through to the reset branch so subsequent promote calls
              // start clean.
              compositorEntity_ = entt::null;
              remapped = false;
            }
          }
        }
        if (!remapped) {
          compositor_->resetAllLayers(/*documentReplaced=*/true);
          compositorEntity_ = entt::null;
          compositorResetCount_.fetch_add(1, std::memory_order_release);
        }
        compositorDocumentGeneration_ = request.documentGeneration;
      }

      // Resolve what the compositor should be promoted on this render.
      // Priority: an explicit drag wins over the persistent selection hint;
      // otherwise we keep the selected entity promoted so the next drag
      // arrives with everything pre-warmed.
      const Entity desiredEntity =
          request.dragPreview.has_value() ? request.dragPreview->entity : request.selectedEntity;
      const svg::compositor::InteractionHint desiredKind =
          request.dragPreview.has_value() ? request.dragPreview->interactionKind
                                          : svg::compositor::InteractionHint::Selection;

      if (compositorEntity_ != desiredEntity) {
        if (compositorEntity_ != entt::null) {
          compositor_->demoteEntity(compositorEntity_);
        }
        compositorEntity_ = entt::null;
        if (desiredEntity != entt::null &&
            compositor_->promoteEntity(desiredEntity, desiredKind)) {
          compositorEntity_ = desiredEntity;
        }
      }

      // The DOM is the sole source of truth for the dragged entity's
      // position — `SelectTool` mutates the `transform` attribute every
      // drag frame, so by the time we reach here the compositor's fast
      // path will diff the new DOM transform against the cached bitmap's
      // rasterize-time transform and either reuse the bitmap via a
      // pure-translation compose offset or mark it dirty for re-rasterize.
      // No emulation layer on top of the DOM.
      svg::RenderViewport viewport;
      const Vector2i canvasSize = request.document->canvasSize();
      viewport.size = Vector2d(canvasSize.x, canvasSize.y);
      viewport.devicePixelRatio = 1.0;
      // Push the current UI-thread setting for tight-bounded segments
      // into the compositor. Setter is a no-op when unchanged; otherwise
      // it marks all segments dirty so the flip takes effect this frame.
      compositor_->setTightBoundedSegmentsEnabled(
          tightBoundedSegments_.load(std::memory_order_acquire));
      double workerMs = 0.0;
      {
        ZoneScopedN("Compositor::renderFrame");
        // Wall-clock the core backend render so the editor can plot it
        // on the frame graph next to the ImGui frame time. This measures
        // the actual work the compositor performs per request, not the
        // total worker iteration (which also pays for takeSnapshot and
        // the result handoff). Scoped here so Tracy's zone cost isn't
        // included either.
        const auto renderStart = std::chrono::steady_clock::now();
        compositor_->renderFrame(viewport);
        const auto renderEnd = std::chrono::steady_clock::now();
        workerMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
      }

      // Expose the split bg/drag/fg trio to the editor only when it was
      // actually produced (single-layer drag path). Pre-warmed state
      // (no active drag) still goes through this path, which means the
      // editor can upload the GL textures as soon as selection happens and
      // the first real drag frame is zero-cost. The compositor-reported
      // compose offset travels back so the editor can place the promoted
      // bitmap to line up with bg/fg on the GPU side.
      if (request.dragPreview.has_value() && compositorEntity_ != entt::null &&
          compositor_->hasSplitStaticLayers()) {
        const Transform2d composeOffset = compositor_->layerComposeOffset(compositorEntity_);
        compositedPreview = RenderResult::CompositedPreview{
            .backgroundBitmap = compositor_->backgroundBitmap(),
            .promotedBitmap = compositor_->layerBitmapOf(compositorEntity_),
            .foregroundBitmap = compositor_->foregroundBitmap(),
            .entity = compositorEntity_,
            .promotedTranslationDoc = composeOffset.translation(),
        };
      }

      // Selection chrome is no longer baked into the bitmap — main.cc
      // draws it via the ImGui draw list every frame so clicks don't
      // pay the SVG re-rasterize cost. The `request.selection` field
      // is left in place for back-compat callers but ignored here.
      (void)request.selection;
      // Always take a snapshot so the flat fallback texture stays current even
      // during composited renders.  This prevents a visual "pop" when the display
      // transitions from composited layers to the flat texture (e.g. during
      // settling after a drag release).
      svg::RendererBitmap bitmap;
      {
        ZoneScopedN("Renderer::takeSnapshot");
        bitmap = request.renderer->takeSnapshot();
      }

      std::function<void()> wake;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only transition to Done if we weren't shut down mid-render.
        if (state_ == State::Busy) {
          result_.bitmap = std::move(bitmap);
          result_.compositedPreview = std::move(compositedPreview);
          result_.version = request.version;
          result_.workerMs = workerMs;
          state_ = State::Done;
          // Snapshot the callback under the lock so a concurrent
          // `setWakeCallback` swap can't tear the invocation. Fire it
          // outside the lock to keep the hook cheap and avoid any
          // chance of deadlock if the caller re-enters AsyncRenderer.
          wake = wakeCallback_;
        }
      }
      if (wake) {
        wake();
      }
    } else {
      std::lock_guard<std::mutex> lock(mutex_);
      if (state_ == State::Busy) {
        state_ = State::Idle;
        busy_.store(false, std::memory_order_release);
      }
    }
  }
}

}  // namespace donner::editor
