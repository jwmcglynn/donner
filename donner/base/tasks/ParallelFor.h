#pragma once
/// @file
/// Bounded, adaptive splitting of independent work over an index range.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <type_traits>
#include <utility>

#include "donner/base/Utils.h"
#include "donner/base/tasks/FalseSharing.h"
#include "donner/base/tasks/TaskScope.h"

namespace donner::tasks {
namespace detail {

/// All tasks in one loop share the body and join the same scope.
template <typename F>
struct ParallelRangeSplitter {
  const F& body;
  std::size_t grain;
  std::size_t initialPieces;

  void run(TaskScope& scope, std::size_t first, std::size_t last, std::size_t pieces) const {
    const auto owner = std::this_thread::get_id();
    while (pieces > 1 && (last - first) / 2 >= grain) {
      const std::size_t middle = first + (last - first) / 2;
      const std::size_t rightPieces = pieces / 2;
      if (!scope.trySpawn([this, &scope, owner, middle, last, rightPieces] {
            // A range taken by another thread can expose more work to thieves.
            run(scope, middle, last,
                std::this_thread::get_id() == owner ? rightPieces : initialPieces);
          })) {
        // The whole remaining interval still belongs to this thread.
        break;
      }
      last = middle;
      pieces -= rightPieces;
    }
    std::invoke(body, std::size_t(first), std::size_t(last));
  }
};

/// Caps the number of leaf ranges without rounding the length past SIZE_MAX.
constexpr std::size_t ParallelForGrain(std::size_t length, std::size_t requested) {
  constexpr std::size_t kMaxChunks = 4096;
  return std::max(std::max<std::size_t>(1, requested),
                  length / kMaxChunks + (length % kMaxChunks != 0));
}

enum class ParallelForStrategy { Adaptive, SharedCursor };

/// Keep contended claims off the cache line containing read-mostly loop state.
struct alignas(kFalseSharingBytes) ParallelChunkCursor {
  std::atomic<std::size_t> next{0};
};

/// Claim bounded chunk ordinals so speculative claims after the last chunk cannot wrap an index.
template <typename F>
void SharedCursorLoop(TaskScheduler& scheduler, std::size_t begin, std::size_t end,
                      std::size_t grain, std::size_t pieces, const F& body) {
  const std::size_t length = end - begin;
  const std::size_t chunks = length / grain + (length % grain != 0);
  ParallelChunkCursor cursor;
  const auto consume = [&] {
    for (;;) {
      ParallelForAccess::Notify(scheduler, SchedulerStep::ParallelForBeforeClaim);
      const std::size_t ordinal = cursor.next.fetch_add(1, std::memory_order_relaxed);
      ParallelForAccess::Notify(scheduler, SchedulerStep::ParallelForAfterClaim);
      if (ordinal >= chunks) {
        return;
      }
      const std::size_t first = begin + ordinal * grain;
      std::invoke(body, std::size_t(first), first + std::min(grain, end - first));
    }
  };
  TaskScope scope(scheduler);
  if (!scope.trySpawn([&] {
        const std::size_t runners = std::min(chunks, pieces);
        for (std::size_t i = 1; i < runners; ++i) {
          if (!scope.trySpawn(consume)) {
            break;
          }
        }
        consume();
      })) {
    std::invoke(body, std::size_t(begin), std::size_t(end));
  }
}

/// Selects the partitioner for tests and comparative measurements.
template <typename F>
void ParallelForWithStrategy(ParallelForStrategy strategy, TaskScheduler& scheduler,
                             std::size_t begin, std::size_t end, std::size_t grain, const F& body) {
  static_assert(std::is_invocable_v<const F&, std::size_t, std::size_t>,
                "ParallelFor must be able to invoke its shared body as const");
  UTILS_RELEASE_ASSERT_MSG(begin <= end, "ParallelFor requires begin <= end");
  if (begin == end) {
    return;
  }
  const std::size_t length = end - begin;
  const std::size_t effectiveGrain = ParallelForGrain(length, grain);
  const std::size_t pieces = scheduler.schedulingWidth();
  if (pieces == 0 || length / 2 < effectiveGrain) {
    std::invoke(body, std::size_t(begin), std::size_t(end));
    return;
  }
  if (strategy == ParallelForStrategy::SharedCursor) {
    SharedCursorLoop(scheduler, begin, end, effectiveGrain, pieces, body);
    return;
  }
  const ParallelRangeSplitter<F> splitter{body, effectiveGrain, pieces};
  // The scope joins before the splitter it borrows goes out of scope.
  TaskScope scope(scheduler);
  if (!scope.trySpawn([&] { splitter.run(scope, begin, end, pieces); })) {
    std::invoke(body, std::size_t(begin), std::size_t(end));
  }
}

}  // namespace detail

/**
 * Invokes one shared, const-callable body on disjoint subranges and joins them before returning.
 * An empty range returns immediately. Reversed bounds fail a release assertion. With zero workers
 * in ordinary mode, a nonempty range is passed to the body once on the calling thread.
 *
 * Worker count and steal timing can change subrange boundaries and visitation order. Deterministic
 * per-index computations that write disjoint outputs give the same results across schedules.
 * A reduction grouped by callback boundaries can produce different results. Seeded adaptive
 * execution uses a fixed four-piece split budget on one thread. The grain, range and capacity
 * bounds still apply, and the seed can change visitation order.
 *
 * @param scheduler The scheduler used to run the work.
 * @param begin The first index in the half-open range.
 * @param end The exclusive upper bound.
 * @param grain Requested grain; zero means one. The effective grain bounds the loop to 4096 chunks.
 * @param body Called with the first and exclusive last index of each subrange. Concurrent calls
 *   must write disjoint outputs or provide their own synchronization.
 */
template <typename F>
void ParallelFor(TaskScheduler& scheduler, std::size_t begin, std::size_t end, std::size_t grain,
                 F&& body) {
  detail::ParallelForWithStrategy(detail::ParallelForStrategy::Adaptive, scheduler, begin, end,
                                  grain, std::as_const(body));
}

}  // namespace donner::tasks
