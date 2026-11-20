#pragma once
/// @file
/// A browser GPU owner channel that runs each round trip at once on the calling thread.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "donner/gpu/browser/BrowserDeferredCalls.h"

namespace donner::gpu::browser {

/// Runs every round trip at once and counts them, for testing a deferred-call queue.
class FakeOwnerChannel final : public BrowserOwnerChannel {
public:
  /// What \ref crossesThreads reports.
  bool crosses = true;

  bool crossesThreads() const override { return crosses; }

  void run(void (*callback)(void*), void* context, uint32_t kind) override {
    roundTrips_.fetch_add(1);
    lastKind_.store(kind);
    callback(context);
  }

  /// Round trips run so far.
  [[nodiscard]] std::size_t roundTrips() const { return roundTrips_.load(); }
  /// Diagnostic category of the most recent round trip.
  [[nodiscard]] uint32_t lastKind() const { return lastKind_.load(); }

private:
  std::atomic<std::size_t> roundTrips_{0};
  std::atomic<uint32_t> lastKind_{0};
};

}  // namespace donner::gpu::browser
