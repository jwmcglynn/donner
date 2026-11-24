#include "donner/base/tasks/ParkerPool.h"

#include <algorithm>
#include <utility>

#include "donner/base/Utils.h"

namespace donner::tasks {

namespace {

/// Pools alive in the process. Relaxed: a test reads it once the threads it cares about are joined.
std::atomic<std::size_t> gLivePools{0};

/// The last generation handed out. Relaxed: only uniqueness matters.
std::atomic<std::uint64_t> gLastGeneration{0};

/// One thread's slots: a pool reference and a slot per scheduler it registered with.
class ThreadRegistry {
public:
  struct Entry {
    std::uint64_t generation;
    std::shared_ptr<ParkerPool> pool;
    ParkerPool::Slot slot;
  };

  ThreadRegistry() = default;
  ThreadRegistry(const ThreadRegistry&) = delete;
  ThreadRegistry& operator=(const ThreadRegistry&) = delete;

  /// Returns every slot and drops every reference, at thread exit.
  ~ThreadRegistry() {
    for (Entry& entry : entries_) {
      entry.pool->release(entry.slot.index);
    }
  }

  Parker<>* parkerFor(std::uint64_t generation, const std::shared_ptr<ParkerPool>& pool) {
    for (const Entry& entry : entries_) {
      if (entry.generation == generation) {
        return entry.slot.parker;
      }
    }
    // A new registration: first drop the pools whose schedulers are gone.
    std::erase_if(entries_, [](const Entry& entry) {
      if (!entry.pool->closed()) {
        return false;
      }
      entry.pool->release(entry.slot.index);
      return true;
    });
    const ParkerPool::Slot slot = pool->acquire();
    entries_.push_back(Entry{generation, pool, slot});
    return slot.parker;
  }

private:
  std::vector<Entry> entries_;
};

thread_local ThreadRegistry tRegistry;

}  // namespace

ParkerPool::ParkerPool() {
  gLivePools.fetch_add(1, std::memory_order_relaxed);
}

ParkerPool::~ParkerPool() {
  UTILS_RELEASE_ASSERT_MSG(inUse_ == 0, "A parker pool was destroyed with a slot in use");
  gLivePools.fetch_sub(1, std::memory_order_relaxed);
}

ParkerPool::Slot ParkerPool::acquire() {
  std::lock_guard lock(mutex_);
  if (free_.empty()) {
    const std::size_t first = blocks_.size() * kBlockSlots;
    blocks_.push_back(std::make_unique<Block>());
    // Hand out the lowest index first.
    for (std::size_t i = kBlockSlots; i > 0; --i) {
      free_.push_back(first + i - 1);
    }
  }
  const std::size_t index = free_.back();
  free_.pop_back();
  ++inUse_;
  return Slot{index, &blocks_[index / kBlockSlots]->slots[index % kBlockSlots].parker};
}

void ParkerPool::release(std::size_t index) {
  std::lock_guard lock(mutex_);
  UTILS_RELEASE_ASSERT_MSG(index < blocks_.size() * kBlockSlots && inUse_ > 0 &&
                               std::find(free_.begin(), free_.end(), index) == free_.end(),
                           "A parker slot was returned that is not in use");
  free_.push_back(index);
  --inUse_;
}

void ParkerPool::close() {
  // Release: called after every worker is joined, so a thread that sees the pool closed sees a
  // pool none of whose slots a scope will use again.
  closed_.store(true, std::memory_order_release);
}

bool ParkerPool::closed() const {
  // Acquire: pairs with the release in close().
  return closed_.load(std::memory_order_acquire);
}

std::size_t ParkerPool::slotCount() const {
  std::lock_guard lock(mutex_);
  return blocks_.size() * kBlockSlots;
}

std::size_t ParkerPool::slotsInUse() const {
  std::lock_guard lock(mutex_);
  return inUse_;
}

Parker<>* ParkerPool::ParkerForThread(std::uint64_t generation,
                                      const std::shared_ptr<ParkerPool>& pool) {
  return tRegistry.parkerFor(generation, pool);
}

std::uint64_t ParkerPool::NextGeneration() {
  return gLastGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::size_t ParkerPool::LiveCountForTesting() {
  return gLivePools.load(std::memory_order_relaxed);
}

}  // namespace donner::tasks
