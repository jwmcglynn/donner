#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace donner::benchmarks::allocations {

struct Snapshot {
  std::uint64_t allocationCalls = 0;
  std::uint64_t allocationBytes = 0;
  std::uint64_t freeCalls = 0;
};

inline std::atomic<bool> gEnabled = false;
inline std::atomic<std::uint64_t> gAllocationCalls = 0;
inline std::atomic<std::uint64_t> gAllocationBytes = 0;
inline std::atomic<std::uint64_t> gFreeCalls = 0;

inline void recordAllocation(std::size_t size) noexcept {
  if (gEnabled.load(std::memory_order_relaxed)) {
    gAllocationCalls.fetch_add(1, std::memory_order_relaxed);
    gAllocationBytes.fetch_add(size, std::memory_order_relaxed);
  }
}

inline void recordFree() noexcept {
  if (gEnabled.load(std::memory_order_relaxed)) {
    gFreeCalls.fetch_add(1, std::memory_order_relaxed);
  }
}

class Scope {
public:
  Scope() {
    gAllocationCalls.store(0, std::memory_order_relaxed);
    gAllocationBytes.store(0, std::memory_order_relaxed);
    gFreeCalls.store(0, std::memory_order_relaxed);
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
                   gFreeCalls.load(std::memory_order_relaxed)};
      stopped_ = true;
    }
    return snapshot_;
  }

private:
  bool stopped_ = false;
  Snapshot snapshot_;
};

inline void* allocate(std::size_t size) noexcept {
  void* result = std::malloc(size == 0 ? 1 : size);
  if (result) {
    recordAllocation(size);
  }
  return result;
}

inline void* allocateAligned(std::size_t size, std::size_t alignment) noexcept {
  void* result = nullptr;
  if (posix_memalign(&result, alignment, size == 0 ? 1 : size) != 0) {
    return nullptr;
  }
  recordAllocation(size);
  return result;
}

inline void deallocate(void* ptr) noexcept {
  if (ptr) {
    recordFree();
  }
  std::free(ptr);
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
