#pragma once
/// @file
/// Coalesces browser frame notifications and invalidates callbacks across shutdown/restart.

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace donner::editor {

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
