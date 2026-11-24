#include "donner/base/tasks/CoroutineFramePool.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <new>

#include "donner/base/Utils.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DONNER_TASKS_FRAME_ASAN 1
#endif
#endif
#if !defined(DONNER_TASKS_FRAME_ASAN) && defined(__SANITIZE_ADDRESS__)
#define DONNER_TASKS_FRAME_ASAN 1
#endif
#if defined(DONNER_TASKS_FRAME_ASAN)
#include <sanitizer/asan_interface.h>
#endif

namespace donner::tasks {
namespace {

std::align_val_t FrameAlignment(std::size_t bytes) {
  // The size-only coroutine allocation ABI omits alignment. A spilled frame object
  // with alignment A requires at least A bytes, including padding or its own extent.
  return std::align_val_t(std::max(alignof(std::max_align_t), std::bit_floor(bytes)));
}

}  // namespace

std::size_t CoroutineFramePool::Allocation::TrailerOffset(std::size_t frameBytes) {
  constexpr std::size_t mask = alignof(Allocation) - 1;
  UTILS_RELEASE_ASSERT_MSG(
      frameBytes <= std::numeric_limits<std::size_t>::max() - mask - sizeof(Allocation),
      "Coroutine frame allocation size overflowed");
  return (frameBytes + mask) & ~mask;
}

std::size_t CoroutineFramePool::Allocation::Size(std::size_t frameBytes) {
  return TrailerOffset(frameBytes) + sizeof(Allocation);
}

void* CoroutineFramePool::Allocation::frame() {
  auto address = reinterpret_cast<std::uintptr_t>(this);
  return reinterpret_cast<void*>(address - TrailerOffset(frameBytes));
}

void CoroutineFramePool::Allocation::free() noexcept {
  ::operator delete(frame(), FrameAlignment(frameBytes));
}

CoroutineFramePool::~CoroutineFramePool() {
  while (free_ != nullptr) {
    Allocation* allocation = free_;
    free_ = allocation->next;
    allocation->free();
  }
}

void* CoroutineFramePool::allocate(std::size_t frameBytes) {
  const std::size_t bytes = Allocation::Size(frameBytes);
  {
    std::unique_lock lock(mutex_);
    for (Allocation** link = &free_; *link != nullptr; link = &(*link)->next) {
      Allocation* allocation = *link;
      if (allocation->frameBytes == frameBytes) {
        *link = allocation->next;
        retainedBytes_ -= bytes;
        ++liveAllocations_;
        void* frame = allocation->frame();
#if defined(DONNER_TASKS_FRAME_ASAN)
        ASAN_UNPOISON_MEMORY_REGION(frame, frameBytes);
#endif
        return frame;
      }
    }
  }
  auto* frame = static_cast<std::byte*>(::operator new(bytes, FrameAlignment(frameBytes)));
  // The trailer leaves the frame at the aligned base. Retention counts the requested
  // bytes, including trailer padding; the allocator's alignment overhead is separate.
  ::new (frame + Allocation::TrailerOffset(frameBytes)) Allocation{*this, frameBytes};
  std::unique_lock lock(mutex_);
  ++liveAllocations_;
  return frame;
}

void CoroutineFramePool::Release(void* frame, std::size_t frameBytes) noexcept {
  auto* trailer = static_cast<std::byte*>(frame) + Allocation::TrailerOffset(frameBytes);
  auto* allocation = std::launder(reinterpret_cast<Allocation*>(trailer));
  allocation->pool.release(allocation);
}

void CoroutineFramePool::release(Allocation* allocation) noexcept {
  {
    std::unique_lock lock(mutex_);
    --liveAllocations_;
    const std::size_t bytes = Allocation::Size(allocation->frameBytes);
    if (bytes <= maxRetainedBytes_ - retainedBytes_) {
      allocation->next = free_;
      free_ = allocation;
      retainedBytes_ += bytes;
#if defined(DONNER_TASKS_FRAME_ASAN)
      ASAN_POISON_MEMORY_REGION(allocation->frame(), allocation->frameBytes);
#endif
      return;
    }
  }
  allocation->free();
}

void CoroutineFramePool::trackFrame() noexcept {
  livePromises_.fetch_add(1, std::memory_order_relaxed);
}

void CoroutineFramePool::untrackFrame() noexcept {
  livePromises_.fetch_sub(1, std::memory_order_relaxed);
}

void CoroutineFramePool::assertNoLiveFrames() {
  std::unique_lock lock(mutex_);
  UTILS_RELEASE_ASSERT_MSG(
      liveAllocations_ == 0 && livePromises_.load(std::memory_order_relaxed) == 0,
      "A task scheduler was destroyed with live coroutine frames");
}

}  // namespace donner::tasks
