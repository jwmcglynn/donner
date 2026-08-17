#pragma once
/// @file
/// Bounded CPU-side waits on GPU progress, and the shared device-lost flag.
///
/// A hung GPU driver can leave a fence or buffer-map wait blocked forever, in
/// the worst case in uninterruptible kernel sleep. Donner cannot fix drivers,
/// but no thread the application relies on may block unboundedly on the GPU:
/// a hung device must surface as a detectable device-lost condition instead
/// of a hung process. Every native CPU-blocks-on-GPU wait in the Geode stack
/// routes through this header so the timeout policy lives in exactly one
/// place.
///
/// This header is deliberately WebGPU-free (chrono + functional only) so the
/// wait loop can be unit tested without a GPU via the injectable clock and
/// sleep hooks.

#include <atomic>
#include <chrono>
#include <functional>

namespace donner::geode {

/// Outcome of a bounded GPU wait.
enum class GpuWaitResult {
  /// The awaited condition was observed before the deadline.
  Complete,
  /// The deadline expired without the condition being observed. Callers
  /// treat this as evidence of a hung device and declare the device lost.
  TimedOut,
  /// The device was already marked lost; no wait was performed.
  DeviceLost,
};

/// Default bound for waits that previously blocked without limit (teardown
/// drains, inter-submit serialization, editor readback maps). Generous: a
/// healthy device completes these in microseconds to milliseconds, so the
/// bound only trips when the driver has effectively hung.
inline constexpr std::chrono::milliseconds kDefaultGpuWaitTimeout{5000};

/// Bound for snapshot readback map waits. Matches the long-standing readback
/// deadline: snapshot consumers tolerate up to 10 seconds under heavily
/// loaded parallel test runs before declaring the device unresponsive.
inline constexpr std::chrono::milliseconds kReadbackMapTimeout{10000};

/// Poll cadence for non-blocking wait loops. The 100 us cadence bounds
/// completion-detection latency without measurable CPU cost; the snapshot
/// readback path was tuned to this cadence and the perf ceilings assume it.
inline constexpr std::chrono::microseconds kGpuWaitPollInterval{100};

/// Test seams for \ref BoundedGpuWait. Production callers pass none; tests
/// inject a fake clock and a sleep recorder so timeout behavior is verified
/// deterministically and without real sleeping.
struct GpuWaitTestHooks {
  /// Clock override. Defaults to `std::chrono::steady_clock::now`.
  std::function<std::chrono::steady_clock::time_point()> now;
  /// Sleep override. Defaults to `std::this_thread::sleep_for`.
  std::function<void(std::chrono::microseconds)> sleep;
};

/// Shared device-lost flag.
///
/// Set exactly once, from either direction, so a driver-reported device-loss
/// callback and a bounded wait exceeding its deadline converge on one
/// observable condition:
/// - the WebGPU device-lost callback stores `true` when the driver reports a
///   real loss, and
/// - `GeodeDevice::markDeviceLost` stores `true` when a bounded GPU wait
///   times out.
///
/// Held via `shared_ptr` by every party that needs to observe or publish the
/// flag (the `GeodeDevice`, a host embedder's device-lost callback), because
/// WebGPU callbacks can outlive the object that registered them.
struct GeodeDeviceLostState {
  /// True once the device has been declared lost. Never reset.
  std::atomic<bool> lost{false};
};

/**
 * Repeatedly invoke @p pollOnce until it returns true or @p timeout expires.
 *
 * @p pollOnce must be non-blocking: one device poll (or callback-flag check)
 * that returns true when the awaited condition has been observed. The loop
 * never blocks inside the driver, so a hung device costs at most @p timeout
 * plus one @p pollInterval instead of hanging the calling thread forever.
 *
 * The happy path costs one @p pollOnce call and one clock read, so wrapping
 * an already-complete wait adds no measurable overhead.
 *
 * @param pollOnce Non-blocking poll; returns true when the wait is over.
 * @param timeout Total time budget for the wait.
 * @param pollInterval Sleep between polls while the condition is pending.
 * @param testHooks Optional clock/sleep overrides for deterministic tests.
 * @return `Complete` if @p pollOnce returned true, `TimedOut` otherwise.
 */
GpuWaitResult BoundedGpuWait(const std::function<bool()>& pollOnce,
                             std::chrono::milliseconds timeout,
                             std::chrono::microseconds pollInterval = kGpuWaitPollInterval,
                             const GpuWaitTestHooks& testHooks = {});

}  // namespace donner::geode
