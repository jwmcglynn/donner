#pragma once

/// @file
/// Test-only global allocator that tracks live requested C++ bytes during one SVG parse scope.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace donner::benchmarks::parse_allocations {

struct Snapshot {
  std::uint64_t allocationCalls = 0;
  std::uint64_t allocationBytes = 0;
  std::uint64_t freeCalls = 0;
  std::uint64_t liveBytes = 0;
  std::uint64_t peakLiveBytes = 0;
};

inline std::atomic_flag gTrackerLock = ATOMIC_FLAG_INIT;
inline bool gEnabled = false;
inline std::uint64_t gGeneration = 0;
inline Snapshot gSnapshot;

class TrackerLock {
public:
  TrackerLock() noexcept {
    while (gTrackerLock.test_and_set(std::memory_order_acquire)) {}
  }

  TrackerLock(const TrackerLock&) = delete;
  TrackerLock& operator=(const TrackerLock&) = delete;

  ~TrackerLock() noexcept { gTrackerLock.clear(std::memory_order_release); }
};

struct AllocationHeader {
  void* allocation = nullptr;
  std::size_t requestedSize = 0;
  std::uint64_t generation = 0;
};

inline std::uint64_t recordAllocation(std::size_t size) noexcept {
  const TrackerLock lock;
  if (gEnabled) {
    ++gSnapshot.allocationCalls;
    gSnapshot.allocationBytes += size;
    gSnapshot.liveBytes += size;
    gSnapshot.peakLiveBytes = std::max(gSnapshot.peakLiveBytes, gSnapshot.liveBytes);
    return gGeneration;
  }
  return 0;
}

inline void recordFree(const AllocationHeader& header) noexcept {
  const TrackerLock lock;
  if (header.generation != 0 && gEnabled && header.generation == gGeneration) {
    ++gSnapshot.freeCalls;
    gSnapshot.liveBytes -= header.requestedSize;
  }
}

class Scope {
public:
  Scope() {
    const TrackerLock lock;
    // One process-global allocation scope may be active at a time. Allocation/free accounting can
    // arrive from any thread, but overlapping scopes would have no unambiguous owner.
    if (gEnabled) {
      std::abort();
    }
    ++gGeneration;
    if (gGeneration == 0) {
      ++gGeneration;
    }
    gSnapshot = {};
    gEnabled = true;
  }

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

  ~Scope() { stop(); }

  Snapshot stop() {
    if (!stopped_) {
      const TrackerLock lock;
      gEnabled = false;
      snapshot_ = gSnapshot;
      stopped_ = true;
    }
    return snapshot_;
  }

private:
  bool stopped_ = false;
  Snapshot snapshot_;
};

inline void* allocateAligned(std::size_t size, std::size_t alignment) noexcept {
  const std::size_t actualSize = size == 0 ? 1 : size;
  alignment = std::max(alignment, alignof(AllocationHeader));
  if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
      alignment > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) ||
      actualSize > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader) - alignment) {
    return nullptr;
  }

  void* allocation = std::malloc(actualSize + sizeof(AllocationHeader) + alignment);
  if (allocation == nullptr) {
    return nullptr;
  }

  const std::uintptr_t start =
      reinterpret_cast<std::uintptr_t>(allocation) + sizeof(AllocationHeader);
  const std::uintptr_t aligned = (start + alignment - 1) & ~(alignment - 1);
  auto* header = reinterpret_cast<AllocationHeader*>(aligned) - 1;
  header->allocation = allocation;
  header->requestedSize = size;
  header->generation = recordAllocation(size);
  return reinterpret_cast<void*>(aligned);
}

inline void* allocate(std::size_t size) noexcept {
  return allocateAligned(size, alignof(std::max_align_t));
}

inline void deallocate(void* ptr) noexcept {
  if (ptr) {
    const AllocationHeader& header = *(static_cast<AllocationHeader*>(ptr) - 1);
    recordFree(header);
    std::free(header.allocation);
  }
}

}  // namespace donner::benchmarks::parse_allocations

void* operator new(std::size_t size) {
  if (void* ptr = donner::benchmarks::parse_allocations::allocate(size)) {
    return ptr;
  }
  std::abort();
}

void* operator new[](std::size_t size) {
  if (void* ptr = donner::benchmarks::parse_allocations::allocate(size)) {
    return ptr;
  }
  std::abort();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return donner::benchmarks::parse_allocations::allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return donner::benchmarks::parse_allocations::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  if (void* ptr = donner::benchmarks::parse_allocations::allocateAligned(
          size, static_cast<std::size_t>(alignment))) {
    return ptr;
  }
  std::abort();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  if (void* ptr = donner::benchmarks::parse_allocations::allocateAligned(
          size, static_cast<std::size_t>(alignment))) {
    return ptr;
  }
  std::abort();
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return donner::benchmarks::parse_allocations::allocateAligned(
      size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return donner::benchmarks::parse_allocations::allocateAligned(
      size, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete[](void* ptr) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}

void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  donner::benchmarks::parse_allocations::deallocate(ptr);
}
