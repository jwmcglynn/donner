#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace donner::benchmarks::allocations {

struct Snapshot {
  std::uint64_t allocationCalls = 0;
  std::uint64_t allocationBytes = 0;
  std::uint64_t freeCalls = 0;
  std::uint64_t liveBytes = 0;
  std::uint64_t peakLiveBytes = 0;
};

inline std::atomic<bool> gEnabled = false;
inline std::atomic<std::uint64_t> gGeneration = 0;
inline std::atomic<std::uint64_t> gAllocationCalls = 0;
inline std::atomic<std::uint64_t> gAllocationBytes = 0;
inline std::atomic<std::uint64_t> gFreeCalls = 0;
inline std::atomic<std::uint64_t> gLiveBytes = 0;
inline std::atomic<std::uint64_t> gPeakLiveBytes = 0;

struct AllocationHeader {
  void* allocation = nullptr;
  std::size_t requestedSize = 0;
  std::uint64_t generation = 0;
};

inline void updatePeak(std::uint64_t liveBytes) noexcept {
  std::uint64_t peak = gPeakLiveBytes.load(std::memory_order_relaxed);
  while (peak < liveBytes &&
         !gPeakLiveBytes.compare_exchange_weak(peak, liveBytes, std::memory_order_relaxed)) {}
}

inline std::uint64_t recordAllocation(std::size_t size) noexcept {
  if (gEnabled.load(std::memory_order_relaxed)) {
    gAllocationCalls.fetch_add(1, std::memory_order_relaxed);
    gAllocationBytes.fetch_add(size, std::memory_order_relaxed);
    const std::uint64_t liveBytes = gLiveBytes.fetch_add(size, std::memory_order_relaxed) + size;
    updatePeak(liveBytes);
    return gGeneration.load(std::memory_order_relaxed);
  }
  return 0;
}

inline void recordFree(const AllocationHeader& header) noexcept {
  if (header.generation != 0 && gEnabled.load(std::memory_order_relaxed) &&
      header.generation == gGeneration.load(std::memory_order_relaxed)) {
    gFreeCalls.fetch_add(1, std::memory_order_relaxed);
    gLiveBytes.fetch_sub(header.requestedSize, std::memory_order_relaxed);
  }
}

class Scope {
public:
  Scope() {
    gEnabled.store(false, std::memory_order_release);
    std::uint64_t generation = gGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    if (generation == 0) {
      generation = gGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    gAllocationCalls.store(0, std::memory_order_relaxed);
    gAllocationBytes.store(0, std::memory_order_relaxed);
    gFreeCalls.store(0, std::memory_order_relaxed);
    gLiveBytes.store(0, std::memory_order_relaxed);
    gPeakLiveBytes.store(0, std::memory_order_relaxed);
    gEnabled.store(true, std::memory_order_release);
  }

  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

  ~Scope() { stop(); }

  Snapshot stop() {
    if (!stopped_) {
      gEnabled.store(false, std::memory_order_release);
      snapshot_ = {gAllocationCalls.load(std::memory_order_relaxed),
                   gAllocationBytes.load(std::memory_order_relaxed),
                   gFreeCalls.load(std::memory_order_relaxed),
                   gLiveBytes.load(std::memory_order_relaxed),
                   gPeakLiveBytes.load(std::memory_order_relaxed)};
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

}  // namespace donner::benchmarks::allocations

void* operator new(std::size_t size) {
  if (void* ptr = donner::benchmarks::allocations::allocate(size)) {
    return ptr;
  }
  std::abort();
}

void* operator new[](std::size_t size) {
  if (void* ptr = donner::benchmarks::allocations::allocate(size)) {
    return ptr;
  }
  std::abort();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return donner::benchmarks::allocations::allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return donner::benchmarks::allocations::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  if (void* ptr = donner::benchmarks::allocations::allocateAligned(
          size, static_cast<std::size_t>(alignment))) {
    return ptr;
  }
  std::abort();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  if (void* ptr = donner::benchmarks::allocations::allocateAligned(
          size, static_cast<std::size_t>(alignment))) {
    return ptr;
  }
  std::abort();
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return donner::benchmarks::allocations::allocateAligned(size,
                                                          static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return donner::benchmarks::allocations::allocateAligned(size,
                                                          static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete[](void* ptr) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}

void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
  donner::benchmarks::allocations::deallocate(ptr);
}
