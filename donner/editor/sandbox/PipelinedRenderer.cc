#include "donner/editor/sandbox/PipelinedRenderer.h"

#include <chrono>
#include <utility>

#include "donner/editor/sandbox/ReplayingRenderer.h"
#include "donner/editor/sandbox/SerializingRenderer.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererTinySkia.h"

namespace donner::editor::sandbox {

std::unique_ptr<svg::RendererInterface> MakeDefaultRendererTinySkia() {
  return std::make_unique<svg::RendererTinySkia>();
}

PipelinedRenderer::PipelinedRenderer(RendererFactory factory) : factory_(factory) {
  worker_ = std::thread([this] { workerMain(); });
}

PipelinedRenderer::~PipelinedRenderer() {
  {
    std::lock_guard<std::mutex> lock(inboxMutex_);
    shutdown_.store(true, std::memory_order_release);
  }
  inboxCv_.notify_all();
  outboxCv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

uint64_t PipelinedRenderer::submit(svg::SVGDocument& document, int width, int height) {
  const uint64_t frameId = nextFrameId_.fetch_add(1, std::memory_order_relaxed);

  // Serialize on the calling thread. This is intentional: the document is
  // thread-owned and the driver mutates its caches during traversal, so the
  // safest place to encode is exactly where the document lives.
  document.setCanvasSize(width, height);
  SerializingRenderer serializer;
  serializer.draw(document);

  PendingFrame frame;
  frame.frameId = frameId;
  frame.wire = std::move(serializer).takeBuffer();
  frame.width = width;
  frame.height = height;

  {
    std::lock_guard<std::mutex> lock(inboxMutex_);
    // Newest wins: overwrite any previously-queued frame that the worker
    // hasn't picked up yet.
    pending_ = std::move(frame);
  }
  inboxCv_.notify_one();
  return frameId;
}

std::optional<PipelinedFrame> PipelinedRenderer::acquireLatestFrame() {
  std::lock_guard<std::mutex> lock(outboxMutex_);
  if (!latest_.has_value()) {
    return std::nullopt;
  }
  std::optional<PipelinedFrame> out = std::move(latest_);
  latest_.reset();
  return out;
}

std::optional<PipelinedFrame> PipelinedRenderer::waitForFrame(uint64_t target) {
  std::unique_lock<std::mutex> lock(outboxMutex_);
  outboxCv_.wait(lock, [&] {
    if (shutdown_.load(std::memory_order_acquire)) return true;
    return latest_.has_value() && latest_->frameId >= target;
  });
  if (!latest_.has_value() || latest_->frameId < target) {
    return std::nullopt;
  }
  std::optional<PipelinedFrame> out = std::move(latest_);
  latest_.reset();
  return out;
}

void PipelinedRenderer::workerMain() {
  std::unique_ptr<svg::RendererInterface> backend;

  while (true) {
    PendingFrame pending;
    {
      std::unique_lock<std::mutex> lock(inboxMutex_);
      inboxCv_.wait(lock, [&] {
        return pending_.has_value() || shutdown_.load(std::memory_order_acquire);
      });
      if (shutdown_.load(std::memory_order_acquire) && !pending_.has_value()) {
        return;
      }
      pending = std::move(*pending_);
      pending_.reset();
    }

    if (!backend) {
      backend = factory_();
    }
    if (!backend) {
      // Factory returned nullptr — publish a failed frame and keep running
      // so the caller can observe the failure deterministically.
      PipelinedFrame failed;
      failed.frameId = pending.frameId;
      failed.ok = false;
      {
        std::lock_guard<std::mutex> lock(outboxMutex_);
        latest_ = std::move(failed);
      }
      outboxCv_.notify_all();
      continue;
    }

    ReplayingRenderer replay(*backend);
    ReplayReport report;
    const ReplayStatus status = replay.pumpFrame(pending.wire, report);

    PipelinedFrame out;
    out.frameId = pending.frameId;
    out.unsupportedCount = report.unsupportedCount;
    out.ok = (status == ReplayStatus::kOk) ||
             (status == ReplayStatus::kEncounteredUnsupported);
    if (out.ok) {
      out.bitmap = backend->takeSnapshot();
    }

    {
      std::lock_guard<std::mutex> lock(outboxMutex_);
      latest_ = std::move(out);
    }
    outboxCv_.notify_all();
  }
}

}  // namespace donner::editor::sandbox
