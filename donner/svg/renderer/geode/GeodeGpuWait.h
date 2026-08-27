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
#include <cstdint>
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

/// Which bounded wait exceeded its deadline and declared the device lost.
///
/// A hung device costs the full timeout wherever it is first waited on, and
/// the two waits have very different budgets and callers, so "the device was
/// declared lost" is not actionable on its own: a readback-map timeout points
/// at buffer-map delivery, a queue-drain timeout points at submitted work
/// never retiring. Recording which one tripped keeps that distinction in the
/// diagnostics a failure report is assembled from.
enum class GpuWaitSite : std::uint8_t {
  /// No bounded wait has timed out. A device lost with this site was reported
  /// by the driver's device-lost callback, not by a deadline.
  None,
  /// A buffer-map wait for GPU-to-CPU readback (snapshot or surface capture).
  ReadbackMap,
  /// A wait for the GPU queue to drain (teardown, inter-submit serialization).
  QueueIdle,
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
  /// True once the device has been declared lost. Never reset. Publish it
  /// only through \ref DeclareDeviceLost or
  /// \ref DeclareDeviceLostAfterWaitTimeout, never by storing directly: the
  /// declaring call is what decides the attribution below, and a direct store
  /// silently opts out of that decision.
  std::atomic<bool> lost{false};
  /// Bounded wait that declared the loss, or `None` when the driver reported
  /// it. Only the call that wins the `lost` transition writes this, so an
  /// empty site is a positive statement ("no deadline expired first"), not an
  /// unwritten field.
  ///
  /// Written just after `lost`, so a reader sampling between the two sees a
  /// lost device with an empty site for the width of two stores. That reads
  /// as a driver-reported loss, which is wrong but self-correcting: the site
  /// settles immediately and the next sample carries it. Consumers that
  /// publish this pair should therefore key on its value changing rather than
  /// latching the first sample they see.
  std::atomic<GpuWaitSite> timedOutSite{GpuWaitSite::None};
  /// Wall time the timed-out wait spent before giving up, in milliseconds.
  /// Written before `timedOutSite`, which publishes it, so a reader that sees
  /// a site also sees that site's elapsed time. Zero while `timedOutSite` is
  /// `None`.
  std::atomic<int> timedOutElapsedMs{0};
};

/**
 * Declare @p state lost with no wait to attribute it to.
 *
 * For losses the driver reports: there is no deadline behind them, so
 * `timedOutSite` stays `None` and says exactly that.
 *
 * @return True when this call performed the false-to-true transition, so a
 *   caller can log the cause exactly once.
 */
bool DeclareDeviceLost(GeodeDeviceLostState& state);

/**
 * Declare @p state lost because a bounded wait at @p site exceeded its
 * deadline after @p elapsed.
 *
 * The attribution is written only when this call is the one that declares the
 * loss. Two other writers reach the same flag and neither may claim the site:
 * a later bounded wait, which expires because the device is ALREADY hung (a
 * consequence of the loss, never its cause), and the driver's device-lost
 * callback, which has no wait to name. Deriving the claim from the flag's own
 * transition covers both without a check-then-set window - reading the flag
 * and then storing the site would let a driver-reported loss landing in
 * between be relabelled as a wait timeout, which is the one misattribution an
 * empty site exists to rule out.
 *
 * @param state Shared device-lost record.
 * @param site Which bounded wait expired.
 * @param elapsed Wall time that wait spent before giving up.
 * @return True when this call performed the false-to-true transition.
 */
bool DeclareDeviceLostAfterWaitTimeout(GeodeDeviceLostState& state, GpuWaitSite site,
                                       std::chrono::milliseconds elapsed);

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
