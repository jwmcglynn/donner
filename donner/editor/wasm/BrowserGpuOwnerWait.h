#pragma once
/// @file
/// Opt-in browser fixture that arranges a real GPU request during an application-owner wait.

#include <cstdint>
#include <memory>

namespace donner::gpu::browser {
class BrowserDevice;
}

namespace donner::editor::wasm {
struct OwnerWaitProbeResult {
  uint64_t completedSerial;
  double realWaitSliceMs;
  bool producerIsGpuOwner;
  double joinElapsedMs;
};

/// The caller must hold no document access or canvas frame and supply live device ownership.
/// The two-second loop deadline cannot preempt callbacks or finite nonwaiting SDK code.
/// SDK blocking futex slices share that deadline, including post-exit join synchronization.
/// A failed probe retains
/// one bounded shared request until module teardown; successful return requires producer joining.
OwnerWaitProbeResult ExerciseGpuOwnerLibcWaitForTesting(
    const std::shared_ptr<gpu::browser::BrowserDevice>& device);

/// Progress of the owner dispatch probe, read from any thread.
struct OwnerDispatchProbeProgress {
  uint32_t completed;  ///< Round trips the current or last run finished.
  bool running;        ///< True until the producer has released the device.
};

/// Starts a detached producer pthread that makes @p count real completion-query round trips to the
/// GPU owner, one at a time, and returns at once. The owner serves them from its event loop, so a
/// count above the owner's inline epoch budget (32 calls) cannot finish without a budget-renewing
/// owner pulse. The caller keeps the module alive until the run stops running.
/// @param device Live device ownership the producer holds until it finishes.
/// @param count Round trips to make; must be positive.
/// @return False when a previous run is still running or the producer cannot start.
bool StartGpuOwnerDispatchProbeForTesting(
    const std::shared_ptr<gpu::browser::BrowserDevice>& device, uint32_t count);

/// @return The dispatch probe's progress.
OwnerDispatchProbeProgress ReadGpuOwnerDispatchProbeForTesting();
}  // namespace donner::editor::wasm
