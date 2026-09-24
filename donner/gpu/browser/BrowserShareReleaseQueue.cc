#include "donner/gpu/browser/BrowserShareReleaseQueue.h"

#include <utility>

namespace donner::gpu::browser {

namespace {

/// The calling thread's queue, closed when the thread destroys its thread-local objects.
struct ThreadQueue {
  std::shared_ptr<BrowserShareReleaseQueue> queue = std::make_shared<BrowserShareReleaseQueue>();

  ~ThreadQueue() { queue->close(); }
};

/// The calling thread's holder, made on first use.
ThreadQueue& ThisThreadQueue() {
  thread_local ThreadQueue holder;
  return holder;
}

}  // namespace

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

std::shared_ptr<BrowserShareReleaseQueue> BrowserShareReleaseQueue::ForThisThread() {
  return ThisThreadQueue().queue;
}

void BrowserShareReleaseQueue::DrainThisThread(const std::function<void(const Release&)>& run) {
  for (const Release& release : ThisThreadQueue().queue->takeAll()) {
    run(release);
  }
}

}  // namespace donner::gpu::browser
