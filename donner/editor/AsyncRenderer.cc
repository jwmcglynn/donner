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
      if (request.dragPreview.has_value()) {
        if (!compositor_ || compositorDocument_ != request.document ||
            compositorRenderer_ != request.renderer) {
          compositor_ = std::make_unique<svg::compositor::CompositorController>(*request.document,
                                                                                *request.renderer);
          compositorDocument_ = request.document;
          compositorRenderer_ = request.renderer;
          compositorEntity_ = entt::null;
          compositorDocumentVersion_ = request.version;
        }

        // Detect document rebuild (ReplaceDocumentCommand).  When the document version jumps,
        // all entity handles from the previous document are invalid.  Reset the compositor's
        // layers so it doesn't try to demote/iterate stale entities.
        if (request.version != compositorDocumentVersion_) {
          compositor_->resetAllLayers();
          compositorEntity_ = entt::null;
          compositorDocumentVersion_ = request.version;
        }

        if (compositorEntity_ != request.dragPreview->entity) {
          if (compositorEntity_ != entt::null) {
            compositor_->demoteEntity(compositorEntity_);
          }

          compositorEntity_ = entt::null;
          if (compositor_->promoteEntity(request.dragPreview->entity,
                                         request.dragPreview->interactionKind)) {
            compositorEntity_ = request.dragPreview->entity;
          }
        }

        if (compositorEntity_ != entt::null) {
          compositor_->setLayerCompositionTransform(
              compositorEntity_, Transform2d::Translate(request.dragPreview->translation));

          svg::RenderViewport viewport;
          const Vector2i canvasSize = request.document->canvasSize();
          viewport.size = Vector2d(canvasSize.x, canvasSize.y);
          viewport.devicePixelRatio = 1.0;
          compositor_->renderFrame(viewport);

          if (compositor_->hasSplitStaticLayers()) {
            compositedPreview = RenderResult::CompositedPreview{
                .backgroundBitmap = compositor_->backgroundBitmap(),
                .promotedBitmap = compositor_->layerBitmapOf(compositorEntity_),
                .foregroundBitmap = compositor_->foregroundBitmap(),
                .entity = compositorEntity_,
            };
          }
        } else {
          request.renderer->draw(*request.document);
        }
      } else {
        compositor_.reset();
        compositorDocument_ = nullptr;
        compositorRenderer_ = nullptr;
        compositorEntity_ = entt::null;
        compositorDocumentVersion_ = 0;
        request.renderer->draw(*request.document);
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
