#pragma once
/// @file
/// Coalesces browser frame notifications and invalidates callbacks across shutdown/restart.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <optional>

namespace donner::editor {

/// Preserve scheduled work across asynchronous event frames that cannot service an animation tick.
/// @param previousDeadlineMs Deadline owed before this frame, on the host's monotonic clock.
/// @param nextDeadlineMs Recomputed deadline after this frame, or no remaining idle work.
/// @param eventFrame Whether this frame services an event rather than a scheduled tick.
inline std::optional<double> NextScheduledFrameDeadline(std::optional<double> previousDeadlineMs,
                                                        std::optional<double> nextDeadlineMs,
                                                        bool eventFrame) {
  if (eventFrame && previousDeadlineMs) {
    return nextDeadlineMs ? std::min(*previousDeadlineMs, *nextDeadlineMs) : previousDeadlineMs;
  }
  return nextDeadlineMs;
}

/// Owner-thread counters; index zero is scheduled work and index one is an event frame.
struct FrameAttemptStats {
  std::array<uint64_t, 2> attempts{};
  std::array<uint64_t, 2> idle{};
  std::array<uint64_t, 2> rendered{};
  uint64_t blockedByFrame = 0;
  uint64_t blockedByGpu = 0;

  /// Record the existing admission decision without changing either guard.
  /// @param eventFrame Whether this is a completion-event callback.
  /// @param frameActive Whether another UI frame is active.
  /// @param gpuActive Whether a GPU owner callback is active.
  bool admit(bool eventFrame, bool frameActive, bool gpuActive) {
    ++attempts[eventFrame];
    blockedByFrame += frameActive;
    blockedByGpu += gpuActive;
    return !frameActive && !gpuActive;
  }

  /// Whether the most recent full UI frame was an event frame; unset before the first one.
  std::optional<bool> lastRenderedEventFrame;

  /// Count an admitted callback's outcome. @param eventFrame Event vs scheduled callback.
  /// @param didRender Whether the callback ran a full UI frame (not proof of surface presentation).
  void finish(bool eventFrame, bool didRender) {
    ++(didRender ? rendered : idle)[eventFrame];
    if (didRender) {
      lastRenderedEventFrame = eventFrame;
    }
  }
};

/// Start/stop and callback execution belong to the app thread; admission may run on another thread.
class FrameCallbackAdmission {
public:
  /// Start a new callback generation. A wrapped generation is rejected rather than reused.
  uint32_t start() {
    if (++nextGeneration_ == 0) {
      std::abort();
    }
    pending_.store(0);
    generation_.store(nextGeneration_);
    return nextGeneration_;
  }

  /// Invalidate borrowed callback data before its owner destroys it.
  void stop() { generation_.store(0); }

  /// Current generation, or zero after shutdown.
  uint32_t generation() const { return generation_.load(); }

  /// Whether callback data still belongs to this generation. @param generation Callback token.
  bool isCurrent(uint32_t generation) const {
    return generation != 0 && generation_.load() == generation;
  }

  /// Admit at most one notification until its actual frame finishes. @param generation Tick token.
  bool acquire(uint32_t generation) {
    if (!isCurrent(generation)) {
      return false;
    }
    uint32_t expected = 0;
    if (!pending_.compare_exchange_strong(expected, generation)) {
      return false;
    }
    if (!isCurrent(generation)) {
      complete(generation);
      return false;
    }
    return true;
  }

  /// Release admission, including failed posts; stale callbacks cannot clear a new generation.
  /// @param generation Callback token.
  void complete(uint32_t generation) { pending_.compare_exchange_strong(generation, 0); }

private:
  uint32_t nextGeneration_ = 0;
  std::atomic<uint32_t> generation_{0};
  std::atomic<uint32_t> pending_{0};
};

/// Coalesces event-frame requests into at most one queued frame without losing a request.
///
/// A request that lands before the queued frame starts is served by that frame. A request that
/// lands while the frame runs, including one whose admission attempt loses to the frame still
/// being pending, makes `finish()` ask for one more frame. Requests may come from any thread;
/// `begin()` and `finish()` belong to the thread that runs the frame. The serial and the admission
/// flag are sequentially consistent: a requester that loses the admission race and a finisher that
/// rechecks the serial cannot both miss each other's update.
class EventFrameWake {
public:
  /// Start a new generation, discarding any queued frame of an earlier one.
  uint32_t start() { return admission_.start(); }

  /// Reject every queued and future frame until the next `start()`.
  void stop() { admission_.stop(); }

  /// Current generation, or zero after shutdown.
  uint32_t generation() const { return admission_.generation(); }

  /// Whether a frame token still belongs to this generation. @param generation Frame token.
  bool isCurrent(uint32_t generation) const { return admission_.isCurrent(generation); }

  /// Record a request. @param generation Current generation.
  /// @return true when the caller must queue the event frame; false when one is already owed.
  bool request(uint32_t generation) {
    serial_.fetch_add(1);
    return admission_.acquire(generation);
  }

  /// Read when the queued frame starts. @return The request serial this frame serves.
  uint32_t begin() const { return serial_.load(); }

  /// Finish the frame that `begin()` started.
  /// @param generation Frame token.
  /// @param servedSerial Value `begin()` returned.
  /// @return true when the caller must queue another frame for requests it did not serve.
  bool finish(uint32_t generation, uint32_t servedSerial) {
    admission_.complete(generation);
    return admission_.isCurrent(generation) && serial_.load() != servedSerial &&
           admission_.acquire(generation);
  }

  /// Release a frame that will not run: a stale token or a failed post. @param generation Token.
  void cancel(uint32_t generation) { admission_.complete(generation); }

private:
  FrameCallbackAdmission admission_;
  std::atomic<uint32_t> serial_{0};
};

}  // namespace donner::editor
