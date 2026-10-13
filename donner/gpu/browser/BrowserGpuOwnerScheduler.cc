#include "donner/gpu/browser/BrowserGpuOwnerScheduler.h"

#include <cstdlib>

namespace donner::gpu::browser {

void BrowserGpuOwnerScheduler::schedulePulse() {
  if (!pulseScheduled_ && !canvasFrameActive_) {
    pulseScheduled_ = true;
    host_.schedulePulse();
  }
}

std::size_t BrowserGpuOwnerScheduler::executeReadyBatch() {
  const double started = host_.nowMs();
  const std::size_t count = ready_.executeBatch();
  if (count) {
    host_.recordBatch(count, host_.nowMs() - started);
  }
  if (!ready_.empty()) {
    schedulePulse();
  }
  return count;
}

void BrowserGpuOwnerScheduler::enqueue(BrowserGpuTask& task) {
  const bool wasEmpty = ready_.empty();
  ready_.enqueue(task);
  // Keep this future pulse even if inline work empties the FIFO: only it renews the budget.
  schedulePulse();
  if (!wasEmpty || processingBatch_ || canvasFrameActive_ || !inlineBudgetRemains()) {
    return;
  }
  processingBatch_ = true;
  const double started = host_.nowMs();
  // The FIFO held only this request; reentrant requests stay in the next detached batch.
  const std::size_t count = executeReadyBatch();
  const double elapsedMs = host_.nowMs() - started;
  inlineCalls_ += count;
  inlineMs_ += elapsedMs;
  host_.recordInline(count, elapsedMs, inlineCalls_);
  processingBatch_ = false;
}

void BrowserGpuOwnerScheduler::pulse() {
  pulseScheduled_ = false;
  if (canvasFrameActive_) {
    return;
  }
  if (processingBatch_) {
    schedulePulse();
    return;
  }
  inlineCalls_ = 0;
  inlineMs_ = 0.0;
  processingBatch_ = true;
  const std::size_t count = executeReadyBatch();
  processingBatch_ = false;
  host_.recordPulse(count);
}

bool BrowserGpuOwnerScheduler::serviceWait() {
  if (processingBatch_) {
    return false;
  }
  processingBatch_ = true;
  host_.deliverPendingRequests();
  executeReadyBatch();
  processingBatch_ = false;
  return true;
}

void BrowserGpuOwnerScheduler::setCanvasFrameActive(bool active) {
  if (canvasFrameActive_ == active || (active && processingBatch_)) {
    std::abort();
  }
  canvasFrameActive_ = active;
  if (!active && !ready_.empty()) {
    schedulePulse();
  }
}

}  // namespace donner::gpu::browser
