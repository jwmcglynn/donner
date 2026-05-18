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
    RenderRequest stagedRequest = request;
    if (!request.structuralRemap.empty()) {
      retainedStructuralRemaps_[request.documentGeneration] = request.structuralRemap;
    } else {
      const auto retainedIt = retainedStructuralRemaps_.find(request.documentGeneration);
      if (retainedIt != retainedStructuralRemaps_.end()) {
        stagedRequest.structuralRemap = retainedIt->second;
      }
    }
    // The new request always wins the pending slot. If the worker was
    // mid-render the cancel signal below makes it bail at the next safe
    // point and the inner loop picks up `pendingRequest_` for the
    // restart. If the worker was Done (a prior result the caller never
    // drained), we drop that result — the new request supersedes it.
    pendingRequest_ = std::move(stagedRequest);
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

void AsyncRenderer::cancelInFlight() {
  bool signalCancel = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Busy) {
      // Worker is mid-renderFrame. Transition to `Cancelling` (not
      // `Idle`) — busy_ stays true so the editor's `!isBusy()`
      // gates keep gating registry reads until the worker actually
      // observes the cancel and bails. If we flipped to Idle here,
      // there'd be a window where the editor thinks the worker is
      // done but the worker is still mid-`prepareDocumentFor
      // Rendering` / shadow-tree teardown, and the editor's
      // sidebar's cached SVGElement reads would SIGSEGV in
      // `parentElement` against a torn-down entity.
      state_ = State::Cancelling;
      signalCancel = true;
    } else if (state_ == State::Done) {
      // Worker raced to completion before we got here. The result
      // is already in `result_` but the user-input event that
      // triggered this cancel supersedes it. Drop the result and
      // transition to Idle directly.
      state_ = State::Idle;
      result_ = RenderResult{};
      busy_.store(false, std::memory_order_release);
    }
    // Either way: drop any staged-but-undrained intermediate result.
    // An intermediate already drained by `pollResult` before this
    // cancel has landed in editor / GL land already — that's by
    // design; the editor's next request will replace the textures.
    hasIntermediateResult_ = false;
    intermediateResult_ = RenderResult{};
  }
  if (signalCancel) {
    cancelRender_.cancel();
    // Notify in case the worker was still in `cv_.wait` when we
    // landed — its updated predicate also wakes on `Cancelling`.
    cv_.notify_one();
    // `busy_` stays true here. The worker thread is the only thing
    // allowed to flip it false — once it actually observes the
    // cancel and unwinds out of `renderFrame` (or, if it was still
    // asleep at cv_.wait, drops back to Idle without doing work).
  }
}

std::optional<RenderResult> AsyncRenderer::pollResult() {
  std::lock_guard<std::mutex> lock(mutex_);
  // Design doc 0034 progressive rendering: drain a staged
  // intermediate first. State stays `Busy` and `busy_` stays true —
  // the worker is still chugging on the canvas-sized refinement.
  if (hasIntermediateResult_) {
    hasIntermediateResult_ = false;
    return std::move(intermediateResult_);
  }
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
      cv_.wait(lock, [this] {
        return state_ == State::Busy || state_ == State::Cancelling || state_ == State::Shutdown;
      });
      if (state_ == State::Shutdown) {
        // `renderer` is destroyed here as the local unwinds, i.e. on
        // the worker thread — mirroring its construction.
        return;
      }
      if (state_ == State::Cancelling) {
        // `cancelInFlight` raced with the worker before it could
        // start renderFrame (worker was still in cv_.wait when the
        // cancel landed). Transition to Idle, flip busy_, loop
        // back to cv_.wait — no work to actually do.
        state_ = State::Idle;
        busy_.store(false, std::memory_order_release);
        continue;
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

      // Compositor lifecycle is split into two independent decisions:
      //
      //   1. Do we need a fresh `CompositorController` instance? Only
      //      on first construction or when the renderer pointer changes
      //      (e.g. backend swap). The renderer is owned by the worker
      //      and constructed at the top of `workerLoop`, so in steady
      //      state this is just the first-frame case.
      //
      //   2. Did the document space swap underneath us? `setDocument`
      //      and `setDocumentMaybeStructural` both bump
      //      `documentGeneration` and produce a fresh `Registry` (the
      //      `SVGDocumentHandle` pointer changes). When that happens we
      //      try the structural-remap path FIRST — it preserves cached
      //      filter / bucket bitmaps, `canvasFromBitmap` stamps, and
      //      the pre-warmed bg/fg pair — and only fall back to a
      //      destructive `resetAllLayers(documentReplaced=true)` when
      //      no remap is available or the remap itself fails an
      //      invariant check.
      //
      // The previous implementation collapsed step 1 onto a pointer-
      // identity check that fired on every `setDocumentMaybeStructural`
      // (since the new doc carries a new `Registry` handle), making
      // step 2's structural-remap branch unreachable on the
      // drag-release writeback path. The user-visible symptom was a
      // filter-group "snap back to original position" on drag release:
      // the freshly-reconstructed compositor blitted its zero-offset
      // bitmap of the pre-drag layer state while the editor's cached
      // GL textures still showed the dragged element at its rasterize-
      // time position. Pinned by
      // `RnrReplayTest::FilterSnapbackReproPreservesCompositorAcrossWriteback`.
      const bool needsFreshCompositor = !compositor_ || compositorRenderer_ != request.renderer;
      if (needsFreshCompositor) {
        compositor_ = std::make_unique<svg::compositor::CompositorController>(*request.document,
                                                                              *request.renderer);
        compositorDocument_ = *request.document;  // cheap: refcount bump on the Registry handle.
        compositorRenderer_ = request.renderer;
        compositorEntity_ = entt::null;
        compositorInteractionKind_ = svg::compositor::InteractionHint::Selection;
        compositorDocumentGeneration_ = request.documentGeneration;
        compositorReconstructCount_.fetch_add(1, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lock(mutex_);
          retainedStructuralRemaps_.erase(request.documentGeneration);
        }
      }

      const bool documentSwapDetected =
          !needsFreshCompositor &&
          (request.documentGeneration != compositorDocumentGeneration_ ||
           (compositorDocument_.has_value() &&
            compositorDocument_->handle().get() != request.document->handle().get()));
      if (documentSwapDetected) {
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
        compositorDocument_ = *request.document;
        compositorDocumentGeneration_ = request.documentGeneration;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          retainedStructuralRemaps_.erase(request.documentGeneration);
        }
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
      // Only re-promote when the entity changes or the user is starting
      // a new drag on the currently-selected entity (Selection →
      // ActiveDrag upgrade). Do NOT downgrade ActiveDrag → Selection
      // on mouse-up: keep the compositor in ActiveDrag mode for the
      // rest of the selection lifecycle so `composeLayers`'s
      // `skipMainCompose` gate (`compositor.cc:2267`) stays engaged —
      // it checks `hasActiveDrag` (walks `activeHints_` for an
      // `ActiveDrag`-kind hint). Without this carve-out every mouse-up
      // runs the full `composeLayers` (~370 ms at 3× canvas on the
      // splash) before the next drag-start can engage the fast path —
      // the operator-visible "drag-release ⇄ drag-again hitch" from
      // design doc 0034. The kind downgrades naturally when the
      // selection itself changes (entityChanged ⇒ demote + re-promote
      // with whatever kind the new request says).
      const bool kindUpgrade =
          desiredEntity != entt::null &&
          compositorInteractionKind_ == svg::compositor::InteractionHint::Selection &&
          desiredKind == svg::compositor::InteractionHint::ActiveDrag;
      if (entityChanged || kindUpgrade) {
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

      // Keep the compositor hint in ActiveDrag across mouse-up so the
      // layer/segment caches survive quick release->drag-again cycles, but
      // only skip the main-renderer compose while an actual drag request is
      // in flight. Post-release and Selection-prewarm renders must refresh
      // the flat fallback bitmap; otherwise `takeSnapshot()` returns the
      // pre-drag baseline while the DOM and tile metadata have moved on.
      const bool activeDragRequest =
          request.dragPreview.has_value() &&
          request.dragPreview->interactionKind == svg::compositor::InteractionHint::ActiveDrag;
      compositor_->setSkipMainComposeDuringSplit(activeDragRequest);

      // Build a CompositedPreview from the compositor's current tile
      // state. Used for both the intermediate stage (fresh drag-
      // target layer + stale segments) AND the final stage (all
      // tiles fresh). Returns nullopt when there's no active drag
      // preview to compose or the compositor has no layers.
      // Build a CompositedPreview from the compositor's current tile
      // state. Used for both stages (design doc 0034):
      //   * `Final` (skipNonDragTargetBitmaps=false): every tile
      //     carries fresh pixels. Editor uploads each as a new GL
      //     texture.
      //   * `Intermediate` (skipNonDragTargetBitmaps=true): only the
      //     drag-target layer carries pixels. Other tiles ship
      //     metadata only (empty bitmap, but valid tile id +
      //     position); the editor's `GlTextureCache::upload
      //     Composited` keeps their previously-uploaded GL texture
      //     alive via the tile-id match and blits at the metadata's
      //     position. Lets the intermediate ship in ~ms instead of
      //     hundreds of ms at high zoom (~60 MB per canvas-sized
      //     segment × N segments to copy out of the compositor).
      const auto buildCompositedPreview =
          [&](bool skipNonDragTargetBitmaps) -> std::optional<RenderResult::CompositedPreview> {
        if (!request.dragPreview.has_value() || compositorEntity_ == entt::null ||
            compositor_->layerCount() == 0u) {
          return std::nullopt;
        }
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
          if (ct.bitmap == nullptr || ct.bitmap->empty()) continue;
          using OutKind = RenderResult::CompositedTile::Kind;
          RenderResult::CompositedTile tile;
          tile.kind = ct.layerEntity == entt::null ? OutKind::Segment : OutKind::Layer;
          tile.id = std::to_string(ct.tileId);
          tile.generation = ct.generation;
          tile.canvasOffsetDoc = canvasToDoc(ct.canvasOffsetPx);
          tile.bitmapDimsDoc = canvasToDoc(Vector2d(static_cast<double>(ct.bitmap->dimensions.x),
                                                    static_cast<double>(ct.bitmap->dimensions.y)));
          if (ct.layerEntity != entt::null) {
            tile.dragTranslationDoc = canvasToDoc(ct.canvasFromBitmap.translation());
          }
          tile.isDragTarget = ct.isDragTarget;
          if (!skipNonDragTargetBitmaps || tile.isDragTarget) {
            tile.bitmap = *ct.bitmap;
          }
          previewTiles.push_back(std::move(tile));
        }
        if (previewTiles.empty()) {
          return std::nullopt;
        }
        return RenderResult::CompositedPreview{
            .tiles = std::move(previewTiles),
            .entity = compositorEntity_,
            .interactionKind = request.dragPreview->interactionKind,
        };
      };

      double workerMs = 0.0;
      bool renderCompleted = true;
      const auto renderStart = std::chrono::steady_clock::now();
      // Design doc 0034 progressive rendering: the compositor fires
      // this callback once after the drag-target layer rasterize +
      // before canvas-sized segment work. Stage an intermediate
      // result so the UI thread can upload the partial preview
      // without waiting for the slow canvas-sized refinement below.
      // Bitmap field gets a snapshot of the renderer's CURRENT
      // surface contents — that's the PRIOR frame's flat baseline,
      // since this frame's `driver.draw` hasn't run yet. Editor
      // displays it stretched at the new viewport while the canvas-
      // sized work catches up.
      //
      // Only meaningful when an ActiveDrag is in flight — the user
      // is waiting for visible feedback from their click-and-drag.
      // For Selection-kind prewarms (post-click, no drag yet, or
      // pinch-zoom on a selected entity) the intermediate path
      // misaligns: the intermediate's per-tile `canvasOffsetDoc`
      // is computed against the NEW canvas-coordinate frame, but
      // the cached GL textures are rasterized in the PRIOR canvas-
      // coordinate frame. Without an ActiveDrag the user has no
      // input latency to win and the misalignment shows as the
      // scene jumping by the canvas-scale ratio until the final
      // refinement lands. Skip the intermediate for Selection-kind.
      const bool emitIntermediate =
          request.dragPreview.has_value() &&
          request.dragPreview->interactionKind == svg::compositor::InteractionHint::ActiveDrag;
      const auto onIntermediate = [&]() {
        const auto t = std::chrono::steady_clock::now();
        const double partialMs = std::chrono::duration<double, std::milli>(t - renderStart).count();
        // Design doc 0034: the intermediate result intentionally
        // ships WITHOUT a fresh flat-baseline bitmap. Copying the
        // renderer surface at high zoom (5228×3000 = 60 MB at 3×
        // canvas) takes ~30–50 ms — significant on the critical
        // path. Editor's prior-frame flat-baseline GL texture
        // remains valid and gets stretched at the new viewport
        // scale; the intermediate's compositedPreview tiles
        // overlay the drag-target layer (intrinsic-sized in doc
        // units; valid at any viewport scale) on top.
        auto intermediatePreview = buildCompositedPreview(/*skipNonDragTargetBitmaps=*/true);
        if (!intermediatePreview.has_value()) {
          return;
        }
        std::function<void()> wake;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          // Don't stage if we've been cancelled or shut down between
          // the callback firing and reaching this lock.
          if (state_ != State::Busy) {
            return;
          }
          intermediateResult_.stage = RenderResult::Stage::Intermediate;
          intermediateResult_.bitmap = svg::RendererBitmap{};
          intermediateResult_.compositedPreview = std::move(intermediatePreview);
          intermediateResult_.version = request.version;
          intermediateResult_.workerMs = partialMs;
          hasIntermediateResult_ = true;
          wake = wakeCallback_;
        }
        if (wake) {
          wake();
        }
      };
      {
        ZoneScopedN("Compositor::renderFrame");
        // Wall-clock the core backend render so the editor can plot it
        // on the frame graph next to the ImGui frame time. This measures
        // the actual work the compositor performs per request, not the
        // total worker iteration (which also pays for takeSnapshot and
        // the result handoff). Scoped here so Tracy's zone cost isn't
        // included either.
        if (emitIntermediate) {
          renderCompleted = compositor_->renderFrame(viewport, cancelRender_, onIntermediate);
        } else {
          renderCompleted = compositor_->renderFrame(viewport, cancelRender_);
        }
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
      //
      // Two-flavor cancel:
      //   * `requestRender` while busy: state is still `Busy` here
      //     (the new request overwrote `pendingRequest_` and the
      //     loop restarts with the new args).
      //   * `cancelInFlight`: state is `Cancelling`. There's no
      //     pending request, so transition to `Idle` and flip
      //     `busy_` false so the editor unblocks. (Doing this in the
      //     worker — not in `cancelInFlight` itself — closes the
      //     window where the editor thinks the worker is idle but
      //     the worker is still mid-`prepareDocumentForRendering`.)
      if (!renderCompleted) {
        cancelledRenderCount_.fetch_add(1, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Cancelling) {
          state_ = State::Idle;
          busy_.store(false, std::memory_order_release);
        }
        continue;
      }

      // Build the final-stage CompositedPreview. Same helper as the
      // intermediate above, but invoked AFTER `renderFrame` finishes
      // its canvas-sized work — so segment tiles are now at the
      // current canvas size rather than the prior frame's. Design
      // doc 0033 §M2C explains the per-tile blit shape; design doc
      // 0034 explains why we use the same builder for both stages.
      compositedPreview = buildCompositedPreview(/*skipNonDragTargetBitmaps=*/false);

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
        // Only transition to Done if we weren't shut down or
        // cancelled mid-render.
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
        } else if (state_ == State::Cancelling) {
          // `cancelInFlight` raced with the worker's final lap —
          // renderFrame finished naturally but the user-input event
          // wants the result dropped. Drop it and transition to
          // Idle so the worker's cv_.wait at the top of the loop
          // doesn't deadlock.
          state_ = State::Idle;
          busy_.store(false, std::memory_order_release);
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
