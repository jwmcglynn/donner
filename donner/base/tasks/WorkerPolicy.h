#pragma once
/// @file
/// Platform defaults for scheduler worker threads.

#include <algorithm>
#include <cstddef>
#include <iosfwd>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::tasks {

/// Requested Apple scheduling class. Other platforms retain inherited scheduling.
enum class WorkerQos { kInteractive, kUserInitiated, kDefault };

/// Prints a worker QoS for diagnostics.
inline std::ostream& operator<<(std::ostream& os, WorkerQos qos) {
  switch (qos) {
    case WorkerQos::kInteractive: return os << "interactive";
    case WorkerQos::kUserInitiated: return os << "user-initiated";
    case WorkerQos::kDefault: return os << "default";
  }
  UTILS_UNREACHABLE();
}

/// Number of workers for the CPUs available to the caller, reserving one for the caller itself.
std::size_t DefaultWorkerCount();

namespace detail {

/// Reserve a caller and choose between one and 32 workers even if detection returns zero.
constexpr std::size_t WorkerCountForProcessors(std::size_t available) {
  return std::clamp<std::size_t>(available > 0 ? available - 1 : 0, 1, 32);
}

#if defined(__linux__)
/// Caps the affinity count by CPU bandwidth limits visible in this cgroup hierarchy.
std::size_t CgroupLimitedProcessors(std::size_t affinity, std::istream& membership,
                                    std::istream& mountInfo);
#endif

/// Returns the platform error code for a refused scheduling hint, or zero.
int TrySetCurrentWorkerQos(WorkerQos qos);

}  // namespace detail
}  // namespace donner::tasks
