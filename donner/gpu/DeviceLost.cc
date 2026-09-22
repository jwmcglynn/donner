#include "donner/gpu/DeviceLost.h"

#include <cstdio>
#include <ostream>

namespace donner::gpu {

std::ostream& operator<<(std::ostream& os, DeviceLostWaitSite site) {
  switch (site) {
    case DeviceLostWaitSite::None: return os << "None";
    case DeviceLostWaitSite::ReadbackMap: return os << "ReadbackMap";
    case DeviceLostWaitSite::QueueIdle: return os << "QueueIdle";
  }
  return os << "DeviceLostWaitSite(" << static_cast<int>(site) << ")";
}

bool DeclareDeviceLost(DeviceLostState& state) {
  return !state.lost.exchange(true, std::memory_order_acq_rel);
}

bool DeclareDeviceLostAfterWaitTimeout(DeviceLostState& state, DeviceLostWaitSite site,
                                       std::chrono::milliseconds elapsed) {
  if (!DeclareDeviceLost(state)) {
    return false;
  }
  // Winning the transition makes this call the only writer of the attribution, so the stores
  // below race with nothing. Publish the elapsed time first and the site last: `timedOutSite` is
  // what a reader tests, so releasing it last is what makes the pair observable together.
  state.timedOutElapsedMs.store(static_cast<int>(elapsed.count()), std::memory_order_relaxed);
  DeviceLostWaitSite unattributed = DeviceLostWaitSite::None;
  state.timedOutSite.compare_exchange_strong(unattributed, site, std::memory_order_release,
                                             std::memory_order_relaxed);
  return true;
}

void LogDeclaredDeviceLoss(const char* reason) {
  std::fprintf(stderr, "[gpu] Device declared lost: %s\n", reason ? reason : "(no reason)");
}

}  // namespace donner::gpu
