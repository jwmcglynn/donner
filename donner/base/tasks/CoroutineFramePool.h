#pragma once
/// @file
/// Bounded reuse for coroutine-frame allocations.

#include <atomic>
#include <cstddef>
#include <mutex>

namespace donner::tasks {

class TaskSchedulerTestPeer;

/// The scheduler must outlive every frame allocated from its pool.
class CoroutineFramePool {
public:
  /// @param maxRetainedBytes Maximum requested heap bytes held free, including trailers and
  /// padding, before allocator overhead.
  explicit CoroutineFramePool(std::size_t maxRetainedBytes) : maxRetainedBytes_(maxRetainedBytes) {}
  ~CoroutineFramePool();

  CoroutineFramePool(const CoroutineFramePool&) = delete;
  CoroutineFramePool& operator=(const CoroutineFramePool&) = delete;

  [[nodiscard]] void* allocate(std::size_t frameBytes);
  static void Release(void* frame, std::size_t frameBytes) noexcept;
  void trackFrame() noexcept;
  void untrackFrame() noexcept;
  void assertNoLiveFrames();

private:
  friend class TaskSchedulerTestPeer;

  struct Allocation {
    CoroutineFramePool& pool;
    const std::size_t frameBytes;
    Allocation* next = nullptr;

    [[nodiscard]] static std::size_t TrailerOffset(std::size_t frameBytes);
    [[nodiscard]] static std::size_t Size(std::size_t frameBytes);
    [[nodiscard]] void* frame();
    void free() noexcept;
  };

  void release(Allocation* allocation) noexcept;

  std::mutex mutex_;
  Allocation* free_ = nullptr;
  const std::size_t maxRetainedBytes_;
  std::size_t retainedBytes_ = 0;
  std::size_t liveAllocations_ = 0;
  std::atomic<std::size_t> livePromises_{0};
};

}  // namespace donner::tasks
