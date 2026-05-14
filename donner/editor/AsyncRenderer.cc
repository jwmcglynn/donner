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
  bool wasBusy = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    wasBusy = (state_ == State::Busy);
    // The new request always wins the pending slot. If the worker was
    // mid-render the cancel signal below makes it bail at the next safe
    // point and the inner loop picks up `pendingRequest_` for the
    // restart. If the worker was Done (a prior result the caller never
    // drained), we drop that result — the new request supersedes it.
    pendingRequest_ = request;
    state_ = State::Busy;
  }
  if (wasBusy) {
    // §M4: tell the in-flight render to bail. The worker's `renderFrame`
    // polls `cancelRender_.isCancelled()` between rasterize loops and
    // returns early; the worker iteration then restarts with the new
    // `pendingRequest_` without committing the cancelled result.
    cancelRender_.cancel();
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
    // §M4: every iteration starts with a fresh (non-cancelled) token.
    // The UI thread sets cancel via `requestRender` ONLY when posting
    // while busy, and we're idle here right before the render runs —
    // so any cancel signal from a previous iteration is stale.
    cancelRender_.reset();

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
      // Identity check uses the underlying `SVGDocumentHandle` (a shared_ptr
      // to the Registry), not the `SVGDocument` pointer — SVGDocument is a
      // value facade and two facades wrapping the same handle are the same
      // document.
      const bool documentChanged =
          !compositorDocument_.has_value() ||
          compositorDocument_->handle().get() != request.document->handle().get();
      if (!compositor_ || documentChanged || compositorRenderer_ != request.renderer) {
        compositor_ = std::make_unique<svg::compositor::CompositorController>(*request.document,
                                                                              *request.renderer);
        compositor_->setSkipMainComposeDuringSplit(true);
        compositorDocument_ = *request.document;  // cheap: refcount bump on the Registry handle.
        compositorRenderer_ = request.renderer;
        compositorEntity_ = entt::null;
        compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
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
          compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
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

      // Re-promote when EITHER the entity changes OR the kind changes (the
      // editor flips Selection → ActiveDrag at drag start without changing
      // the entity). The compositor's `promoteEntity` refreshes the kind
      // in place for an already-promoted entity instead of demoting and
      // re-promoting, so the layer's cached bitmap survives the
      // transition. Skipping the kind-change re-promote left the
      // compositor treating an active drag as a Selection prewarm and
      // tripped the descendant-segment dirty cascade every drag frame
      // post-zoom — sustained > 1 s/frame on the splash.
      const bool entityChanged = compositorEntity_ != desiredEntity;
      const bool kindChanged =
          desiredEntity != entt::null && desiredKind != compositorInteractionKind_;
      if (entityChanged || kindChanged) {
        if (entityChanged && compositorEntity_ != entt::null) {
          compositor_->demoteEntity(compositorEntity_);
        }
        if (entityChanged) {
          compositorEntity_ = entt::null;
          compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
        }
        if (desiredEntity != entt::null && compositor_->promoteEntity(desiredEntity, desiredKind)) {
          compositorEntity_ = desiredEntity;
          compositorInteractionKind_ = desiredKind;
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
      bool renderCompleted = true;
      {
        ZoneScopedN("Compositor::renderFrame");
        // Wall-clock the core backend render so the editor can plot it
        // on the frame graph next to the ImGui frame time. This measures
        // the actual work the compositor performs per request, not the
        // total worker iteration (which also pays for takeSnapshot and
        // the result handoff). Scoped here so Tracy's zone cost isn't
        // included either.
        const auto renderStart = std::chrono::steady_clock::now();
        renderCompleted = compositor_->renderFrame(viewport, cancelRender_);
        const auto renderEnd = std::chrono::steady_clock::now();
        workerMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
      }

      // §M4: a cancelled render leaves the compositor's internal state
      // partially-updated (some segments / layers re-rasterized, the
      // rest still flagged dirty for the next pass). Don't commit a
      // half-finished result_; instead, count the cancellation,
      // discard any partial work we'd otherwise package up, and loop
      // back so the worker picks up `pendingRequest_` for the
      // restart. `state_` is still Busy (the UI thread set it that
      // way when posting the new request); `cancelRender_.reset()` at
      // the top of the loop clears the cancel signal so the restarted
      // render runs to completion.
      if (!renderCompleted) {
        cancelledRenderCount_.fetch_add(1, std::memory_order_release);
        continue;
      }

      // Expose the compositor's paint-order tile list to the editor (design
      // doc 0033 §M2C). The editor blits each tile directly at its canvas
      // offset, replacing the legacy bg/promoted/fg flatten step. Pre-warmed
      // state (no active drag) also goes through this path so the editor
      // can upload GL textures as soon as selection happens and the first
      // drag frame is zero-cost.
      if (request.dragPreview.has_value() && compositorEntity_ != entt::null &&
          compositor_->layerCount() > 0u) {
        const Vector2i canvasSize = request.document->canvasSize();
        const Box2d viewBox = request.document->svgElement().viewBox().value_or(Box2d::FromXYWH(
            0, 0, static_cast<double>(canvasSize.x), static_cast<double>(canvasSize.y)));
        const double viewBoxWidth = viewBox.size().x;
        const double viewBoxHeight = viewBox.size().y;
        const double scaleX =
            viewBoxWidth > 0.0 ? static_cast<double>(canvasSize.x) / viewBoxWidth : 1.0;
        const double scaleY =
            viewBoxHeight > 0.0 ? static_cast<double>(canvasSize.y) / viewBoxHeight : 1.0;
        const auto canvasToDoc = [&](const Vector2d& canvas) {
          return Vector2d(scaleX != 0.0 ? canvas.x / scaleX : 0.0,
                          scaleY != 0.0 ? canvas.y / scaleY : 0.0);
        };

        const auto compositorTiles = compositor_->snapshotTilesForUpload();
        std::vector<RenderResult::CompositedTile> previewTiles;
        previewTiles.reserve(compositorTiles.size());
        for (const auto& ct : compositorTiles) {
          if (ct.bitmap == nullptr || ct.bitmap->empty()) {
            continue;
          }
          using OutKind = RenderResult::CompositedTile::Kind;
          RenderResult::CompositedTile tile;
          tile.kind = ct.layerEntity == entt::null ? OutKind::Segment : OutKind::Layer;
          // Reuse the compositor's stable `tileId` as the editor cache
          // key. Segment ids are entity-pair encoded so they survive a
          // segment split; layer ids carry the high bit + entity.
          tile.id = std::to_string(ct.tileId);
          tile.generation = ct.generation;
          tile.canvasOffsetDoc = canvasToDoc(ct.canvasOffsetPx);
          tile.bitmapDimsDoc = canvasToDoc(Vector2d(static_cast<double>(ct.bitmap->dimensions.x),
                                                    static_cast<double>(ct.bitmap->dimensions.y)));
          // Extract `canvasFromBitmap.translation()` for EVERY layer
          // tile, not just the active drag target. For the live drag
          // target this carries the per-frame drag delta the fast
          // path stamps each renderFrame. For non-drag layer tiles
          // it carries the residual delta from when they were last
          // the drag target — critical for pending-demote layers
          // (§M9 hysteresis): a layer whose hint is queued for
          // demote keeps its bitmap content rasterized at the old
          // (pre-drag) entity transform, with `canvasFromBitmap` =
          // Translate(total drag delta) compensating at compose
          // time. Without applying this translation, the editor
          // blits the pending-demote layer at its rasterize-time
          // canvas offset — the user sees previously-moved shapes
          // "pop back" to their pre-drag positions during the
          // hysteresis window, then pop forward again when the
          // demote actually fires and the segment re-rasterizes.
          //
          // Segments are always Identity (canvas-aligned bitmaps with
          // their own offset; no compose-time translation), so the
          // segment branch is effectively a no-op.
          if (ct.layerEntity != entt::null) {
            tile.dragTranslationDoc = canvasToDoc(ct.canvasFromBitmap.translation());
          }
          tile.isDragTarget = ct.isDragTarget;
          tile.bitmap = *ct.bitmap;
          previewTiles.push_back(std::move(tile));
        }
        if (!previewTiles.empty()) {
          compositedPreview = RenderResult::CompositedPreview{
              .tiles = std::move(previewTiles),
              .entity = compositorEntity_,
          };
        }
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
          lastFastPathCounters_ = compositor_->fastPathCountersForTesting();
          lastLayerInspectorRows_ = compositor_->snapshotLayerInspectorRows();
          lastSegmentInspectorRows_ = compositor_->snapshotSegmentInspectorRows();
          lastCompositeTiles_ = compositor_->snapshotCompositeTiles();
          lastStateSnapshot_ = compositor_->snapshotState();
          lastWorkerCompositorEntity_ = compositorEntity_;
          lastDocumentCanvasSize_ = canvasSize;
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
