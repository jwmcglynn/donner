#pragma once
/// @file
/// Owner-thread admission policy for borrowed browser GPU requests.

#include <cstddef>

#include "donner/gpu/browser/BrowserGpuTaskQueue.h"

namespace donner::gpu::browser {

/**
 * Decides when the browser GPU owner starts the requests that other threads proxied to it.
 *
 * The scheduler is single-threaded: every method runs on the owner. It does not wait, allocate or
 * lock; the runtime glue supplies the clock, the pulse message and the proxy-queue drain through
 * \ref Host. Requests reach it in three ways:
 * - a proxied delivery that finds an empty FIFO may start that one request inline;
 * - a pulse renews the inline budget and executes the detached FIFO;
 * - a runtime wait executes the detached FIFO so dependencies of the waiting owner progress.
 *
 * Inline starts share one budget of \ref kMaximumInlineCalls calls or \ref kMaximumInlineMs of
 * callback time, and only a pulse renews it. No inline start or pulse batch runs while a canvas
 * frame is active.
 */
class BrowserGpuOwnerScheduler {
public:
  /// Inline starts allowed between two pulses.
  static constexpr std::size_t kMaximumInlineCalls = 32;
  /// Inline callback time allowed between two pulses.
  static constexpr double kMaximumInlineMs = 2.0;

  /// Runtime effects and diagnostics. Implementations must not reenter the scheduler, except that
  /// \ref deliverPendingRequests passes each pending request to \ref enqueue.
  class Host {
  public:
    virtual ~Host() = default;

    /// Monotonic owner clock in milliseconds.
    virtual double nowMs() = 0;

    /// Post one later pulse message; the scheduler never asks again until \ref pulse runs.
    virtual void schedulePulse() = 0;

    /// Run the proxied deliveries already waiting for the owner, each of which calls
    /// \ref enqueue. It must not wait for more.
    virtual void deliverPendingRequests() = 0;

    /// One nonempty FIFO batch finished. @param count Callbacks run. @param elapsedMs Batch time.
    virtual void recordBatch(std::size_t count, double elapsedMs) = 0;

    /** Inline starts finished.
     * @param count Callbacks run. @param elapsedMs Their time.
     * @param epochCalls Inline starts since the last pulse renewed the budget.
     */
    virtual void recordInline(std::size_t count, double elapsedMs, std::size_t epochCalls) = 0;

    /// A pulse renewed the budget. @param count Callbacks its batch ran.
    virtual void recordPulse(std::size_t count) = 0;
  };

  /// @param host Runtime glue, which must outlive the scheduler.
  explicit BrowserGpuOwnerScheduler(Host& host) : host_(host) {}

  BrowserGpuOwnerScheduler(const BrowserGpuOwnerScheduler&) = delete;
  BrowserGpuOwnerScheduler& operator=(const BrowserGpuOwnerScheduler&) = delete;

  /// Accept one proxied request and start it inline when nothing is ahead of it.
  /// @param task Borrowed until its callback runs.
  void enqueue(BrowserGpuTask& task);

  /// Handle a delivered pulse message: renew the inline budget and execute the detached FIFO.
  void pulse();

  /// Deliver and execute pending requests while the owner is blocked in a runtime wait.
  /// @return False when a batch is already executing, so nothing ran.
  bool serviceWait();

  /// Enter or leave a canvas frame. Aborts on a redundant transition or entry during a batch.
  /// @param active True from before canvas acquisition until after retirement.
  void setCanvasFrameActive(bool active);

  /// Whether a batch is executing on the owner.
  [[nodiscard]] bool dispatchActive() const { return processingBatch_; }

  /// Whether a canvas frame is active.
  [[nodiscard]] bool canvasFrameActive() const { return canvasFrameActive_; }

  /// Whether nothing is queued, executing or inside a canvas frame.
  [[nodiscard]] bool idle() const {
    return !processingBatch_ && !canvasFrameActive_ && ready_.empty();
  }

  /// Forget a pulse that will never be delivered because its port closed.
  void cancelScheduledPulse() { pulseScheduled_ = false; }

private:
  /// Ask for a pulse unless one is pending or a canvas frame defers it.
  void schedulePulse();

  /// Execute the detached FIFO, asking for a pulse if work remains.
  std::size_t executeReadyBatch();

  /// Whether the inline budget admits another start.
  [[nodiscard]] bool inlineBudgetRemains() const {
    return inlineCalls_ < kMaximumInlineCalls && inlineMs_ < kMaximumInlineMs;
  }

  Host& host_;
  BrowserGpuTaskQueue ready_;
  bool processingBatch_ = false;
  bool pulseScheduled_ = false;
  bool canvasFrameActive_ = false;
  std::size_t inlineCalls_ = 0;
  double inlineMs_ = 0.0;
};

}  // namespace donner::gpu::browser
