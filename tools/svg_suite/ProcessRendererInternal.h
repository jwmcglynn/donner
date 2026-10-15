#pragma once
/// @file
/// Internal cleanup ordering shared with deterministic syscall-failure tests. POSIX hosts only.

#include <sys/types.h>

namespace donner::svg_suite::internal {
enum class ChildObservation { Observed, OwnershipLost, Failed };

/// Production supplies direct POSIX operations; tests record calls without signaling real PIDs.
/// The caller exclusively owns reaping child. An observation failure never permits another signal.
template <typename Operations>
bool FinishChild(pid_t child, bool ownershipLost, int& status, Operations& operations) {
  if (ownershipLost) {
    return false;
  }
  operations.killProcessOrGroup(-child);
  // The direct child can still be before setpgid when its deadline expires.
  operations.killProcessOrGroup(child);
  const auto observed = operations.observeExit(child);
  if (observed != ChildObservation::Observed) {
    if (observed == ChildObservation::Failed) {
      operations.reapChild(child, status);
    }
    return false;
  }
  // The leader is dead and its PID is reserved. Close a race with child-side group creation.
  operations.killProcessOrGroup(-child);
  return operations.reapChild(child, status);
}
}  // namespace donner::svg_suite::internal
