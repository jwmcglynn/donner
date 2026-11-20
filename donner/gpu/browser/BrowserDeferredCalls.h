#pragma once
/// @file
/// Browser calls a thread defers to its next round trip to the browser GPU owner.

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "donner/gpu/browser/BrowserBridge.h"

namespace donner::gpu::browser {

/**
 * Browser calls whose answer the calling thread does not wait for, held until its next round trip
 * to the GPU owner carries them, in call order, ahead of that round trip's own call.
 *
 * A call qualifies when nothing its caller does next depends on the browser's answer: a buffer
 * write, a bind group or layout construction whose identifier the runtime minted, or an object
 * destruction. Because every later call from the same thread, direct or recorded, carries the
 * held calls ahead of it, the owner still runs this thread's calls in the order they were made.
 *
 * A refused held call does not stop the calls after it, as each was an independent call when it
 * was made. The refusal is reported once the round trip ends; a lost or foreign device refuses the
 * thread's next call too, so those failures still surface to a caller.
 */
class BrowserDeferredCalls {
public:
  /// One held call.
  struct Call {
    const char* operation = nullptr;        //!< Operation name for diagnostics.
    std::function<BridgeStatus()> execute;  //!< Runs it on the owner; owns all it reads.
  };

  /// The refusals among the calls one round trip carried.
  struct Refusal {
    const char* operation = nullptr;              //!< First refused call, or null for none.
    BridgeStatus status = BridgeStatus::Success;  //!< Its status.
    std::size_t count = 0;                        //!< Refused calls.
  };

  /// Calls held before they must be sent on their own, bounding the owner callback that runs them.
  static constexpr std::size_t kMaxCalls = 256;
  /// Payload bytes held before the calls must be sent on their own, bounding their copies.
  static constexpr std::size_t kMaxBytes = 256 * 1024;

  /**
   * Holds \p execute after the calls already held.
   * @param operation Operation name for diagnostics.
   * @param execute The call, owning copies of everything it reads.
   * @param payloadBytes Bytes \p execute holds a copy of.
   * @return Whether the held calls have reached a bound and must be sent now.
   */
  bool add(const char* operation, std::function<BridgeStatus()> execute,
           std::size_t payloadBytes = 0) {
    calls_.push_back(Call{operation, std::move(execute)});
    bytes_ += payloadBytes;
    return calls_.size() >= kMaxCalls || bytes_ >= kMaxBytes;
  }

  /// Whether no call is held.
  [[nodiscard]] bool empty() const { return calls_.empty(); }

  /// Hands over every held call, in call order, for one round trip, and holds nothing after.
  std::vector<Call> take() {
    bytes_ = 0;
    return std::exchange(calls_, {});
  }

  /**
   * Runs \p calls in order, on the owner. Allocates and locks nothing itself.
   * @param calls Calls from \ref take. @param refusal Accumulates the refusals.
   */
  static void Execute(const std::vector<Call>& calls, Refusal& refusal) {
    for (const Call& call : calls) {
      if (const BridgeStatus status = call.execute(); status != BridgeStatus::Success) {
        if (refusal.count++ == 0) {
          refusal.operation = call.operation;
          refusal.status = status;
        }
      }
    }
  }

private:
  std::vector<Call> calls_;
  std::size_t bytes_ = 0;
};

}  // namespace donner::gpu::browser
