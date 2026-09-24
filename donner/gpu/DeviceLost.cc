#include "donner/gpu/DeviceLost.h"

#include <algorithm>
#include <cstdio>
#include <ostream>

namespace donner::gpu {

std::ostream& operator<<(std::ostream& os, DeviceLostWaitSite site) {
  switch (site) {
    case DeviceLostWaitSite::None: return os << "None";
    case DeviceLostWaitSite::ReadbackMap: return os << "ReadbackMap";
    case DeviceLostWaitSite::QueueIdle: return os << "QueueIdle";
    case DeviceLostWaitSite::Present: return os << "Present";
  }
  return os << "DeviceLostWaitSite(" << static_cast<int>(site) << ")";
}

DeviceLossRelease::~DeviceLossRelease() = default;

void DeviceLostState::addLossRelease(std::weak_ptr<DeviceLossRelease> release) {
  {
    std::lock_guard<std::mutex> lock(releaseMutex_);
    if (!releasesRun_) {
      std::erase_if(releases_,
                    [](const std::weak_ptr<DeviceLossRelease>& held) { return held.expired(); });
      const bool alreadyHeld =
          std::ranges::any_of(releases_, [&release](const std::weak_ptr<DeviceLossRelease>& held) {
            return !held.owner_before(release) && !release.owner_before(held);
          });
      if (!alreadyHeld) {
        releases_.push_back(std::move(release));
      }
      return;
    }
  }
  // Declared already: nothing will run it later, so it runs now, outside the lock like the rest.
  if (const std::shared_ptr<DeviceLossRelease> held = release.lock()) {
    held->releaseOnLoss();
  }
}

void DeviceLostState::runLossReleases() {
  std::vector<std::weak_ptr<DeviceLossRelease>> releases;
  {
    std::lock_guard<std::mutex> lock(releaseMutex_);
    releasesRun_ = true;
    releases.swap(releases_);
  }
  for (const std::weak_ptr<DeviceLossRelease>& release : releases) {
    if (const std::shared_ptr<DeviceLossRelease> held = release.lock()) {
      held->releaseOnLoss();
    }
  }
}

bool DeclareDeviceLost(DeviceLostState& state) {
  if (state.lost.exchange(true, std::memory_order_acq_rel)) {
    return false;
  }
  state.runLossReleases();
  return true;
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
