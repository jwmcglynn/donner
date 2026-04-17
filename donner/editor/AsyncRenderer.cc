#include "donner/editor/AsyncRenderer.h"

#include "donner/editor/OverlayRenderer.h"
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
        compositorDocument_ = request.document;
        compositorRenderer_ = request.renderer;
        compositorEntity_ = entt::null;
        compositorDocumentVersion_ = request.version;
      }

      // Detect document rebuild (ReplaceDocumentCommand). When the document version jumps,
      // all entity handles from the previous document are invalid. Reset the compositor's
      // layers so it doesn't try to demote/iterate stale entities.
      if (request.version != compositorDocumentVersion_) {
        compositor_->resetAllLayers();
        compositorEntity_ = entt::null;
        compositorDocumentVersion_ = request.version;
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

      // Apply the drag translation (zero when we're just holding a selection
      // pre-warmed). When nothing is promoted, this is a no-op.
      if (compositorEntity_ != entt::null) {
        const Vector2d translation = request.dragPreview.has_value()
                                         ? request.dragPreview->translation
                                         : Vector2d::Zero();
        compositor_->setLayerCompositionTransform(compositorEntity_,
                                                  Transform2d::Translate(translation));
      }

      svg::RenderViewport viewport;
      const Vector2i canvasSize = request.document->canvasSize();
      viewport.size = Vector2d(canvasSize.x, canvasSize.y);
      viewport.devicePixelRatio = 1.0;
      compositor_->renderFrame(viewport);

      // Expose the split bg/drag/fg trio to the editor only when it was
      // actually produced (single-layer drag path). Pre-warmed state with
      // identity translation still goes through this path, which means the
      // editor can upload the GL textures as soon as selection happens and
      // the first real drag frame is zero-cost.
      if (request.dragPreview.has_value() && compositorEntity_ != entt::null &&
          compositor_->hasSplitStaticLayers()) {
        compositedPreview = RenderResult::CompositedPreview{
            .backgroundBitmap = compositor_->backgroundBitmap(),
            .promotedBitmap = compositor_->layerBitmapOf(compositorEntity_),
            .foregroundBitmap = compositor_->foregroundBitmap(),
            .entity = compositorEntity_,
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
      svg::RendererBitmap bitmap = request.renderer->takeSnapshot();

      std::lock_guard<std::mutex> lock(mutex_);
      // Only transition to Done if we weren't shut down mid-render.
      if (state_ == State::Busy) {
        result_.bitmap = std::move(bitmap);
        result_.compositedPreview = std::move(compositedPreview);
        result_.version = request.version;
        state_ = State::Done;
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
