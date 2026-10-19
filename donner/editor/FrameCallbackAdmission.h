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
  std::array<uint64_t, 2> attempts{};  ///< Admission attempts.
  std::array<uint64_t, 2> idle{};      ///< Admitted callbacks that did not run a full UI frame.
  std::array<uint64_t, 2> rendered{};  ///< Admitted callbacks that ran a full UI frame.
  uint64_t blockedByFrame = 0;         ///< Attempts made while another UI frame was active.
  uint64_t blockedByGpu = 0;           ///< Attempts made while a GPU owner callback was active.

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

  /// Count an admitted callback's outcome. @param eventFrame Event vs scheduled callback.
  /// @param didRender Whether the callback ran a full UI frame (not proof of surface presentation).
  void finish(bool eventFrame, bool didRender) { ++(didRender ? rendered : idle)[eventFrame]; }
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

}  // namespace donner::editor
