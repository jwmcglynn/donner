#pragma once
/// @file
/// Opt-in browser fixture that arranges a real GPU request during an application-owner wait.

#include <cstdint>
#include <memory>

namespace donner::gpu {
class Device;
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
OwnerWaitProbeResult ExerciseGpuOwnerLibcWaitForTesting(const std::shared_ptr<gpu::Device>& device);
}  // namespace donner::editor::wasm
