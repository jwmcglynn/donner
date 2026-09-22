#pragma once
/// @file
/// \c donner::gpu::DeviceLostState - the sticky device-loss condition of one backend root.
///
/// A hung GPU driver can leave a fence or buffer-map wait blocked forever, in the worst case in
/// uninterruptible kernel sleep. Donner cannot fix drivers, but no thread the application relies
/// on may block unboundedly on the GPU: a hung device must surface as a detectable device-lost
/// condition instead of a hung process. The condition belongs to the backend root rather than to
/// any one runtime device, because several runtime devices routinely drive one root and a root
/// that stopped answering has stopped answering all of them.
///
/// This header is deliberately free of the runtime's descriptors and handles (atomics and chrono
/// only) so a wait loop can publish a loss without pulling the device in.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace donner::gpu {

/// Which bounded wait gave up and declared the device lost.
///
/// A hung device costs the full budget wherever it is first waited on, and the waits have very
/// different budgets and callers, so "the device was declared lost" is not actionable on its own:
/// a readback-map timeout points at buffer-map delivery, a queue-drain timeout points at
/// submitted work never retiring. Recording which one gave up keeps that distinction in the
/// diagnostics a failure report is assembled from.
enum class DeviceLostWaitSite : std::uint8_t {
  /// No bounded wait has given up. A device lost with this site was reported by the backend's
  /// device-lost callback, not by a deadline.
  None,
  /// A buffer-map wait for GPU-to-CPU readback (snapshot or surface capture).
  ReadbackMap,
  /// A wait for the GPU queue to drain (teardown, inter-submit serialization, submission
  /// completion).
  QueueIdle,
};

/**
 * Sticky device-loss condition of one backend root.
 *
 * Set exactly once, from either direction, so a driver-reported device-loss callback and a
 * bounded wait exceeding its deadline converge on one observable condition. Held via
 * `shared_ptr` by every party that needs to observe or publish it - each runtime device over the
 * root, and a host embedder's own device-lost callback - because backend callbacks can outlive
 * the object that registered them.
 */
struct DeviceLostState {
  /// True once the device has been declared lost. Never reset. Publish it only through
  /// \ref DeclareDeviceLost or \ref DeclareDeviceLostAfterWaitTimeout, never by storing directly:
  /// the declaring call is what decides the attribution below, and a direct store silently opts
  /// out of that decision.
  std::atomic<bool> lost{false};
  /// Bounded wait that declared the loss, or `None` when the backend reported it. Only the call
  /// that wins the `lost` transition writes this, so an empty site is a positive statement ("no
  /// deadline expired first"), not an unwritten field.
  ///
  /// Written just after `lost`, so a reader sampling between the two sees a lost device with an
  /// empty site for the width of two stores. That reads as a driver-reported loss, which is wrong
  /// but self-correcting: the site settles immediately and the next sample carries it. Consumers
  /// that publish this pair should therefore key on its value changing rather than latching the
  /// first sample they see.
  std::atomic<DeviceLostWaitSite> timedOutSite{DeviceLostWaitSite::None};
  /// Wall time the timed-out wait spent before giving up, in milliseconds. Written before
  /// `timedOutSite`, which publishes it, so a reader that sees a site also sees that site's
  /// elapsed time. Zero while `timedOutSite` is `None`.
  std::atomic<int> timedOutElapsedMs{0};
};

/**
 * Declare @p state lost with no wait to attribute it to.
 *
 * For losses the backend reports: there is no deadline behind them, so `timedOutSite` stays
 * `None` and says exactly that.
 *
 * @param state Shared device-lost record.
 * @return True when this call performed the false-to-true transition, so a caller can log the
 *   cause exactly once.
 */
bool DeclareDeviceLost(DeviceLostState& state);

/**
 * Declare @p state lost because a bounded wait at @p site gave up after @p elapsed.
 *
 * The attribution is written only when this call is the one that declares the loss. Two other
 * writers reach the same flag and neither may claim the site: a later bounded wait, which gives
 * up because the device is ALREADY hung (a consequence of the loss, never its cause), and the
 * backend's device-lost callback, which has no wait to name. Deriving the claim from the flag's
 * own transition covers both without a check-then-set window - reading the flag and then storing
 * the site would let a driver-reported loss landing in between be relabelled as a wait timeout,
 * which is the one misattribution an empty site exists to rule out.
 *
 * @param state Shared device-lost record.
 * @param site Which bounded wait gave up.
 * @param elapsed Wall time that wait spent before giving up.
 * @return True when this call performed the false-to-true transition.
 */
bool DeclareDeviceLostAfterWaitTimeout(DeviceLostState& state, DeviceLostWaitSite site,
                                       std::chrono::milliseconds elapsed);

/**
 * Logs the cause of a device loss.
 *
 * Call it only from the caller that won the declaration, so the line appears exactly once per
 * backend root no matter how many observers later read the condition.
 *
 * @param reason Human-readable cause; null is logged as an absent reason rather than skipped,
 *   because the loss itself is the news.
 */
void LogDeclaredDeviceLoss(const char* reason);

}  // namespace donner::gpu
