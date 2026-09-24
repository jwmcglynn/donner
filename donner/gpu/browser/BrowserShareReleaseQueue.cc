#include "donner/gpu/browser/BrowserShareReleaseQueue.h"

#include <utility>

namespace donner::gpu::browser {

namespace {

/// Set once the calling thread has destroyed its queue's holder. Trivially destructible, so it
/// can still be read by any thread-local object the thread destroys after the holder, which is
/// every one made before it.
thread_local bool tThreadQueueGone = false;

/// The calling thread's queue, closed when the thread destroys its thread-local objects.
struct ThreadQueue {
  std::shared_ptr<BrowserShareReleaseQueue> queue = std::make_shared<BrowserShareReleaseQueue>();

  ~ThreadQueue() {
    queue->close();
    tThreadQueueGone = true;
  }
};

/// Whether the calling thread has made its holder. Trivially destructible, like the flag above.
thread_local bool tThreadQueueMade = false;

/// The calling thread's holder, made on first use, or null once the thread has destroyed it.
ThreadQueue* ThisThreadQueue() {
  if (tThreadQueueGone) {
    return nullptr;
  }
  thread_local ThreadQueue holder;
  tThreadQueueMade = true;
  return &holder;
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
  ThreadQueue* holder = ThisThreadQueue();
  return holder != nullptr ? holder->queue : nullptr;
}

void BrowserShareReleaseQueue::DrainThisThread(const std::function<void(const Release&)>& run) {
  // A thread that never made a share has no queue for another thread to post to, so there is
  // nothing to run and no reason to make one here.
  if (!tThreadQueueMade) {
    return;
  }
  ThreadQueue* holder = ThisThreadQueue();
  if (holder == nullptr) {
    return;
  }
  for (const Release& release : holder->queue->takeAll()) {
    run(release);
  }
}

}  // namespace donner::gpu::browser
