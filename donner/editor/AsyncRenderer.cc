#include "donner/editor/AsyncRenderer.h"

#include "donner/editor/OverlayRenderer.h"
#include "donner/svg/SVGDocument.h"

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

std::optional<svg::RendererBitmap> AsyncRenderer::pollResult() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::Done) {
    return std::nullopt;
  }
  state_ = State::Idle;
  busy_.store(false, std::memory_order_release);
  return std::move(resultBitmap_);
}

void AsyncRenderer::workerLoop() {
  while (true) {
    RenderRequest request;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return state_ == State::Busy || state_ == State::Shutdown; });
      if (state_ == State::Shutdown) {
        return;
      }
      request = pendingRequest_;
    }

    // Execute the render outside the lock so the UI thread can poll
    // `isBusy()` / `pollResult()` while we work.
    if (request.renderer != nullptr && request.document != nullptr) {
      request.renderer->draw(*request.document);
      // Draw selection chrome using the snapshot captured at request
      // time. The overlay renderer touches ECS (via worldBounds) but
      // the worker thread owns the document during the render so that's
      // safe — it only can't touch EditorApp, which is why we use the
      // snapshot-based overload.
      OverlayRenderer::drawChrome(*request.renderer, request.selection);
      svg::RendererBitmap bitmap = request.renderer->takeSnapshot();

      std::lock_guard<std::mutex> lock(mutex_);
      // Only transition to Done if we weren't shut down mid-render.
      if (state_ == State::Busy) {
        resultBitmap_ = std::move(bitmap);
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
