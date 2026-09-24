#include "donner/gpu/browser/BrowserShareReleaseQueue.h"

#include <utility>

namespace donner::gpu::browser {

bool BrowserShareReleaseQueue::post(const Release& release, const std::function<void()>& notify) {
  const std::lock_guard lock(mutex_);
  if (closed_) {
    return false;
  }
  pending_.push_back(release);
  if (notify) {
    // Run under the lock, which close() takes too, so the owner is still there to be woken.
    notify();
  }
  return true;
}

std::vector<BrowserShareReleaseQueue::Release> BrowserShareReleaseQueue::takeAll() {
  std::vector<Release> taken;
  const std::lock_guard lock(mutex_);
  taken.swap(pending_);
  return taken;
}

void BrowserShareReleaseQueue::close() {
  const std::lock_guard lock(mutex_);
  closed_ = true;
  pending_.clear();
}

bool BrowserShareReleaseQueue::closed() const {
  const std::lock_guard lock(mutex_);
  return closed_;
}

}  // namespace donner::gpu::browser
