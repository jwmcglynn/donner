#pragma once
/// @file
/// Browser calls a thread defers to its next round trip to the browser GPU owner.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/gpu/browser/BrowserBridge.h"

namespace donner::gpu::browser {

/// The bridge operations a thread defers, and the only ones. The browser bridge passes one of
/// these to defer a call; anything else is a direct round trip.
enum class BrowserDeferredOperation : uint8_t {
  WriteBuffer,            //!< `writeBuffer`.
  CreateBindGroup,        //!< `createBindGroup`.
  CreateBindGroupLayout,  //!< `createBindGroupLayout`.
  DestroyObject,          //!< `destroyObject`.
};

/// Bridge method name of each \ref BrowserDeferredOperation, in enumerator order.
inline constexpr std::array<std::string_view, 4> kBrowserDeferredOperationNames = {
    "writeBuffer", "createBindGroup", "createBindGroupLayout", "destroyObject"};

/// Bridge method name of \p operation. @param operation Deferred operation.
constexpr std::string_view BrowserDeferredOperationName(BrowserDeferredOperation operation) {
  return kBrowserDeferredOperationNames[static_cast<std::size_t>(operation)];
}

/**
 * How a thread reaches the browser GPU owner: whether its calls cross threads at all, and a
 * synchronous run of one owner callback.
 */
class BrowserOwnerChannel {
public:
  virtual ~BrowserOwnerChannel() = default;

  /// Whether a call from the calling thread must cross to the owner. False without a registered
  /// owner and on the owner itself, where calls run at once.
  [[nodiscard]] virtual bool crossesThreads() const = 0;

  /**
   * Runs \p callback on the owner as one owner callback and returns once it has finished.
   * @param callback Runs \p context's work. @param context Borrowed until return.
   * @param kind Diagnostic category of the round trip, as the owner counts it.
   */
  virtual void run(void (*callback)(void*), void* context, uint32_t kind) = 0;
};

/**
 * Browser calls whose answer the calling thread does not wait for, held until its next round trip
 * to the GPU owner carries them, in call order, ahead of that round trip's own call.
 *
 * Only the operations in \ref BrowserDeferredOperation are held. The runtime validates each
 * before it reaches the bridge, so only the browser side's own refusals are deferred: a lost or
 * foreign device, an identifier it does not hold, or a browser call that throws.
 *
 * - **Ordering:** \ref run runs every held call, in call order, before the round trip's own call,
 *   inside the same owner callback. Every later call from the thread goes through it.
 * - **Bounds:** at most \ref kMaxCalls calls and \ref kMaxBytes payload bytes are held; reaching
 *   either sends them on their own. A single write of \ref kMaxBytes or more is not held or copied:
 *   it runs at once, after the calls held before it.
 * - **Payload:** a held write owns a copy of its bytes until the round trip that runs it returns.
 * - **Liveness:** a held call whose device has begun releasing does not run; its copies are freed
 *   with the queue.
 * - **Refusals:** a refused write or bind group construction is stored into its own device's
 *   origin, which that device reads as a loss. A refused destruction is counted there for
 *   diagnostics only. A round trip whose own call belongs to a device with a call refused in the
 *   same trip does not run that call, so a submission cannot go ahead over a write that failed.
 * - **Thread exit:** calls still held when the thread's queue is destroyed are sent then, unless
 *   their device has been released, so held destructions are not leaked until device release.
 */
class BrowserDeferredCalls {
public:
  /// One held call.
  struct Call {
    BrowserDeferredOperation operation = BrowserDeferredOperation::WriteBuffer;  //!< Its kind.
    uint64_t objectId = 0;  //!< Identifier of the object it writes, builds or destroys.
    /// Runs it on the owner; owns copies of everything it reads.
    std::function<BridgeStatus()> execute;
    /// The device that made it; kept alive by the call.
    std::shared_ptr<BrowserDeferredOrigin> origin;
  };

  /// What one round trip's held calls reported.
  struct Report {
    /// First refused call. Meaningful when \ref refused is nonzero.
    BrowserDeferredOperation operation = BrowserDeferredOperation::WriteBuffer;
    uint64_t objectId = 0;                        //!< Its object identifier.
    BridgeStatus status = BridgeStatus::Success;  //!< Its status.
    std::size_t refused = 0;                      //!< Refused calls.
    std::size_t skipped = 0;  //!< Calls not run because their device was released.
    /// Whether the round trip's own call was not run, because its device had a call refused in
    /// the same trip.
    bool carriedCallSkipped = false;
  };

  /// Calls held before they must be sent on their own, bounding the owner callback that runs them.
  static constexpr std::size_t kMaxCalls = 256;
  /// Payload bytes held before the calls must be sent on their own, bounding their copies.
  static constexpr std::size_t kMaxBytes = 256 * 1024;

  /// Builds a queue that reaches the owner through \p channel. @param channel Must outlive it.
  explicit BrowserDeferredCalls(BrowserOwnerChannel& channel) : channel_(channel) {}

  /// Sends what is still held, unless its device was released. A thread's queue is destroyed at
  /// thread exit.
  ~BrowserDeferredCalls() {
    dropReleased();
    if (!calls_.empty()) {
      run([] {}, kCommandBatchKind);
    }
  }

  BrowserDeferredCalls(const BrowserDeferredCalls&) = delete;
  BrowserDeferredCalls& operator=(const BrowserDeferredCalls&) = delete;

  /**
   * Builds the execution of a buffer write that owns a copy of \p data, so the caller's bytes can
   * change or go away as soon as this returns.
   * @param data Bytes to write. @param write Writes the bytes it is handed.
   */
  static std::function<BridgeStatus()> OwningWrite(
      std::span<const uint8_t> data, std::function<BridgeStatus(std::span<const uint8_t>)> write) {
    return [owned = std::vector<uint8_t>(data.begin(), data.end()), write = std::move(write)] {
      return write(owned);
    };
  }

  /**
   * Defers \p execute, or runs it now: where calls do not cross threads, and for a payload too
   * large to hold, which runs at once after the calls held before it.
   * @param origin Device making the call. @param operation Its kind. @param objectId Its object.
   * @param execute The call, owning copies of what it reads. @param payloadBytes Bytes it copies.
   * @param direct The call over borrowed bytes, used instead of \p execute when it runs at once;
   *   null to use \p execute.
   * @return Success once held, or the call's own status when it ran now.
   */
  BridgeStatus defer(const std::shared_ptr<BrowserDeferredOrigin>& origin,
                     BrowserDeferredOperation operation, uint64_t objectId,
                     std::function<BridgeStatus()> execute, std::size_t payloadBytes = 0,
                     const std::function<BridgeStatus()>& direct = {}) {
    if (!channel_.crossesThreads()) {
      return direct ? direct() : execute();
    }
    if (payloadBytes >= kMaxBytes) {
      BridgeStatus status = BridgeStatus::Success;
      const Report report =
          run([&] { status = direct ? direct() : execute(); }, kCommandBatchKind, origin.get());
      return report.carriedCallSkipped ? BridgeStatus::DeviceLost : status;
    }
    calls_.push_back(Call{operation, objectId, std::move(execute), origin});
    bytes_ += payloadBytes;
    if (calls_.size() >= kMaxCalls || bytes_ >= kMaxBytes) {
      run([] {}, kCommandBatchKind);
    }
    return BridgeStatus::Success;
  }

  /**
   * Runs \p operation in one owner round trip after every held call, which run first, in call
   * order. Where calls do not cross threads, runs it at once.
   * @param operation The round trip's own call. @param kind Its diagnostic category.
   * @param carrying Device \p operation belongs to, or null. If one of its held calls is refused in
   *   this trip, \p operation is not run.
   * @return What the held calls reported.
   */
  template <typename Operation>
  Report run(Operation&& operation, uint32_t kind,
             const BrowserDeferredOrigin* carrying = nullptr) {
    Report report;
    if (!channel_.crossesThreads() || calls_.empty()) {
      if (channel_.crossesThreads()) {
        RunOnOwner(operation, kind);
      } else {
        operation();
      }
      return report;
    }
    sending_.swap(calls_);
    bytes_ = 0;
    RunOnOwner(
        [&] {
          Execute(sending_, carrying, report);
          if (!report.carriedCallSkipped) {
            operation();
          }
        },
        kind);
    // Keep the vector's capacity for the next round trip; the copies the calls held go now.
    sending_.clear();
    return report;
  }

  /// Sends the held calls now, with no call of its own; makes no round trip when nothing is held.
  Report flush() {
    if (calls_.empty()) {
      return Report{};
    }
    return run([] {}, kCommandBatchKind);
  }

  /// Drops held calls whose device has begun releasing.
  void dropReleased() {
    std::erase_if(calls_, [](const Call& call) {
      return !call.origin->live.load(std::memory_order_acquire);
    });
  }

  /// Whether no call is held.
  [[nodiscard]] bool empty() const { return calls_.empty(); }
  /// Calls held now.
  [[nodiscard]] std::size_t size() const { return calls_.size(); }
  /// Payload bytes held now.
  [[nodiscard]] std::size_t bytes() const { return bytes_; }
  /// Capacity the queue keeps for held calls across round trips.
  [[nodiscard]] std::size_t capacity() const { return calls_.capacity(); }

  /// Diagnostic category of a round trip that sends held calls on their own; the owner's
  /// `BrowserGpuOperationKind::CommandBatch`.
  static constexpr uint32_t kCommandBatchKind = 9;

private:
  template <typename Operation>
  void RunOnOwner(Operation& operation, uint32_t kind) {
    channel_.run([](void* context) { (*static_cast<Operation*>(context))(); }, &operation, kind);
  }
  template <typename Operation>
  void RunOnOwner(Operation&& operation, uint32_t kind) {
    RunOnOwner(operation, kind);
  }

  /// Runs \p calls in order on the owner. Allocates and locks nothing itself.
  static void Execute(const std::vector<Call>& calls, const BrowserDeferredOrigin* carrying,
                      Report& report) {
    for (const Call& call : calls) {
      BrowserDeferredOrigin& origin = *call.origin;
      if (!origin.live.load(std::memory_order_acquire)) {
        ++report.skipped;
        continue;
      }
      const BridgeStatus status = call.execute();
      if (status == BridgeStatus::Success) {
        continue;
      }
      if (call.operation == BrowserDeferredOperation::DestroyObject) {
        origin.destroyRefusals.fetch_add(1, std::memory_order_relaxed);
      } else {
        uint32_t none = static_cast<uint32_t>(BridgeStatus::Success);
        origin.refusal.compare_exchange_strong(none, static_cast<uint32_t>(status),
                                               std::memory_order_release,
                                               std::memory_order_relaxed);
        if (&origin == carrying) {
          report.carriedCallSkipped = true;
        }
      }
      if (report.refused++ == 0) {
        report.operation = call.operation;
        report.objectId = call.objectId;
        report.status = status;
      }
    }
  }

  BrowserOwnerChannel& channel_;
  std::vector<Call> calls_;
  std::vector<Call> sending_;
  std::size_t bytes_ = 0;
};

}  // namespace donner::gpu::browser
