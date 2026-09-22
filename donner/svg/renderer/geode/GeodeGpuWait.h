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

#include <chrono>
#include <functional>

#include "donner/gpu/DeviceLost.h"

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

/// Which bounded wait exceeded its deadline and declared the device lost. The runtime owns the
/// enumeration because the condition belongs to the backend root, not to the Geode wrappers over
/// it; see \ref gpu::DeviceLostWaitSite.
using GpuWaitSite = gpu::DeviceLostWaitSite;

/// Shared device-lost flag of one backend root, observed and published by every Geode context
/// over it and by a host embedder's own device-lost callback. Owned by the runtime; see
/// \ref gpu::DeviceLostState.
using GeodeDeviceLostState = gpu::DeviceLostState;

/// Declares a state lost with no wait to attribute it to; see \ref gpu::DeclareDeviceLost.
using gpu::DeclareDeviceLost;

/// Declares a state lost because a bounded wait gave up; see
/// \ref gpu::DeclareDeviceLostAfterWaitTimeout.
using gpu::DeclareDeviceLostAfterWaitTimeout;

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
 * @param timeout Total time budget for the wait. Taken in microseconds because the readback
 *   wait slices below a millisecond, and a budget rounded up to a whole millisecond would
 *   coarsen the poll cadence the readback path is tuned to.
 * @param pollInterval Sleep between polls while the condition is pending.
 * @param testHooks Optional clock/sleep overrides for deterministic tests.
 * @return `Complete` if @p pollOnce returned true, `TimedOut` otherwise.
 */
GpuWaitResult BoundedGpuWait(const std::function<bool()>& pollOnce,
                             std::chrono::microseconds timeout,
                             std::chrono::microseconds pollInterval = kGpuWaitPollInterval,
                             const GpuWaitTestHooks& testHooks = {});

}  // namespace donner::geode
