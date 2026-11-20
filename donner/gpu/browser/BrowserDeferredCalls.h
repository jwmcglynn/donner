#pragma once
/// @file
/// Browser calls a thread defers to its next round trip to the browser GPU owner.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
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
 * destruction. The runtime validates each of them before it reaches the bridge, so only the
 * browser side's own refusals are deferred: a lost or foreign device, an identifier it does not
 * hold, or a browser call that throws.
 *
 * Ordering: \ref runAfter runs every held call, in call order, before the round trip's own call,
 * inside the same owner callback. Every later call from the thread, direct or recorded, goes
 * through it, so the owner sees the thread's calls in the order they were made.
 *
 * Refusals: a refused held call does not stop the calls after it, as each was an independent call
 * when it was made. It is stored into the refusal sink of the logical device that made it, which
 * that device reads as a loss, so its next operation fails closed instead of the refusal being
 * dropped or charged to whichever call happened to carry it. The first refusal of a round trip is
 * also returned, with its operation and identifier, for a diagnostic.
 *
 * Bounds: at most \ref kMaxCalls calls and \ref kMaxBytes payload bytes are held; \ref add reports
 * reaching either, and the caller then sends the held calls on their own.
 */
class BrowserDeferredCalls {
public:
  /// One held call.
  struct Call {
    const char* operation = nullptr;  //!< Operation name for diagnostics.
    uint64_t objectId = 0;            //!< Identifier of the object it writes, builds or destroys.
    /// Runs it on the owner; owns copies of everything it reads.
    std::function<BridgeStatus()> execute;
    /// Where a refusal is stored, for the logical device that made the call. Must outlive the
    /// round trip that carries the call.
    std::atomic<uint32_t>* refusalSink = nullptr;
  };

  /// The refusals among the calls one round trip carried.
  struct Refusal {
    const char* operation = nullptr;              //!< First refused call, or null for none.
    uint64_t objectId = 0;                        //!< Its object identifier.
    BridgeStatus status = BridgeStatus::Success;  //!< Its status.
    std::size_t count = 0;                        //!< Refused calls.
  };

  /// Calls held before they must be sent on their own, bounding the owner callback that runs them.
  static constexpr std::size_t kMaxCalls = 256;
  /// Payload bytes held before the calls must be sent on their own, bounding their copies.
  static constexpr std::size_t kMaxBytes = 256 * 1024;

  /**
   * Builds the execution of a buffer write that owns a copy of \p data, so the caller's bytes can
   * change or go away as soon as this returns. The copy lives in the held call until the round
   * trip that runs it has returned.
   * @param data Bytes to write. @param write Writes the bytes it is handed.
   */
  static std::function<BridgeStatus()> OwningWrite(
      std::span<const uint8_t> data, std::function<BridgeStatus(std::span<const uint8_t>)> write) {
    return [owned = std::vector<uint8_t>(data.begin(), data.end()), write = std::move(write)] {
      return write(owned);
    };
  }

  /**
   * Holds \p call after the calls already held.
   * @param call The call. @param payloadBytes Bytes its execution holds a copy of.
   * @return Whether the held calls have reached a bound and must be sent now.
   */
  bool add(Call call, std::size_t payloadBytes = 0) {
    calls_.push_back(std::move(call));
    bytes_ += payloadBytes;
    return calls_.size() >= kMaxCalls || bytes_ >= kMaxBytes;
  }

  /// Whether no call is held.
  [[nodiscard]] bool empty() const { return calls_.empty(); }

  /// Calls held now.
  [[nodiscard]] std::size_t size() const { return calls_.size(); }

  /// Payload bytes held now.
  [[nodiscard]] std::size_t bytes() const { return bytes_; }

  /**
   * Runs \p operation in one round trip after every held call, which run first, in call order.
   * @param operation The round trip's own call. @param dispatch Runs the callable it is handed on
   *   the owner, synchronously, as one owner callback.
   * @return The refusals among the held calls.
   */
  template <typename Operation, typename Dispatch>
  Refusal runAfter(Operation&& operation, Dispatch&& dispatch) {
    Refusal refusal;
    if (calls_.empty()) {
      dispatch(operation);
      return refusal;
    }
    const std::vector<Call> calls = take();
    dispatch([&] {
      Execute(calls, refusal);
      operation();
    });
    return refusal;
  }

  /// Hands over every held call, in call order, for one round trip, and holds nothing after.
  std::vector<Call> take() {
    bytes_ = 0;
    return std::exchange(calls_, {});
  }

  /**
   * Runs \p calls in order, on the owner, storing each refusal into its call's sink. Allocates and
   * locks nothing itself.
   * @param calls Calls from \ref take. @param refusal Accumulates the refusals.
   */
  static void Execute(const std::vector<Call>& calls, Refusal& refusal) {
    for (const Call& call : calls) {
      const BridgeStatus status = call.execute();
      if (status == BridgeStatus::Success) {
        continue;
      }
      if (call.refusalSink != nullptr) {
        uint32_t none = static_cast<uint32_t>(BridgeStatus::Success);
        call.refusalSink->compare_exchange_strong(none, static_cast<uint32_t>(status),
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed);
      }
      if (refusal.count++ == 0) {
        refusal.operation = call.operation;
        refusal.objectId = call.objectId;
        refusal.status = status;
      }
    }
  }

private:
  std::vector<Call> calls_;
  std::size_t bytes_ = 0;
};

}  // namespace donner::gpu::browser
