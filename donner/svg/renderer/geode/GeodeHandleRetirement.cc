#include "donner/svg/renderer/geode/GeodeHandleRetirement.h"

#include <utility>

namespace donner::geode {

namespace {

/// Moves the handles of device \p deviceId from \p from to the end of \p to, drops null handles,
/// and leaves the others in \p from. @return Number of handles left in \p from.
template <typename Handle>
std::size_t TakeHandlesOf(uint64_t deviceId, std::vector<Handle>& from, std::vector<Handle>& to) {
  std::vector<Handle> foreign;
  for (Handle& handle : from) {
    if (!handle.isValid()) {
      continue;
    }
    if (handle.deviceId() == deviceId) {
      to.push_back(std::move(handle));
    } else {
      foreign.push_back(std::move(handle));
    }
  }
  from = std::move(foreign);
  return from.size();
}

}  // namespace

std::size_t GeodeHandleRetirement::retire(std::vector<gpu::Buffer>& buffers,
                                          std::vector<gpu::BindGroup>& bindGroups) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::size_t before = buffers_.size() + bindGroups_.size();
  const std::size_t remaining = TakeHandlesOf(runtimeDeviceId_, buffers, buffers_) +
                                TakeHandlesOf(runtimeDeviceId_, bindGroups, bindGroups_);
  if (buffers_.size() + bindGroups_.size() > before && wakeCallback_) {
    wakeCallback_();
  }
  return remaining;
}

void GeodeHandleRetirement::setWakeCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  wakeCallback_ = std::move(callback);
}

bool GeodeHandleRetirement::hasPending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !buffers_.empty() || !bindGroups_.empty();
}

void GeodeHandleRetirement::release() {
  std::vector<gpu::Buffer> buffers;
  std::vector<gpu::BindGroup> bindGroups;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    buffers.swap(buffers_);
    bindGroups.swap(bindGroups_);
  }
  // Released as the vectors go out of scope, outside the lock and on the calling thread: a release
  // reaches the runtime device, and a retirement from another thread must not wait on it.
}

void GeodeHandleRetirement::close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_.store(true, std::memory_order_release);
    wakeCallback_ = {};
  }
  release();
}

GeodeHandleRetirement::HeldCounts GeodeHandleRetirement::heldCountsForTesting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return HeldCounts{buffers_.size(), bindGroups_.size()};
}

}  // namespace donner::geode
