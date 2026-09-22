#include "donner/svg/renderer/geode/GeodeHandleRetirement.h"

#include <utility>

namespace donner::geode {

void GeodeHandleRetirement::retire(std::vector<gpu::Buffer>& buffers,
                                   std::vector<gpu::BindGroup>& bindGroups) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (gpu::Buffer& buffer : buffers) {
    buffers_.push_back(std::move(buffer));
  }
  for (gpu::BindGroup& bindGroup : bindGroups) {
    bindGroups_.push_back(std::move(bindGroup));
  }
  buffers.clear();
  bindGroups.clear();
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
  closed_.store(true, std::memory_order_release);
  release();
}

size_t GeodeHandleRetirement::heldCountForTesting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return buffers_.size() + bindGroups_.size();
}

}  // namespace donner::geode
