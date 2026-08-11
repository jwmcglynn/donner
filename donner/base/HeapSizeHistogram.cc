/// @file
/// Live-heap histogram by allocation size, for the Design 0064 phase 2 memory
/// investigation.
///
/// The category counters in `MemoryAttribution.h` only see memory a subsystem
/// knows it owns, and the stage brackets only see which part of the frame the
/// allocator grew during. When live heap climbs and neither says why, the
/// remaining question is "how big are the blocks", because the answer
/// immediately separates "a few full-canvas pixel buffers" from "a million
/// small nodes" and points at a small set of call sites.
///
/// Enabled by linking with:
///
/// ```
/// -DDONNER_HEAP_SIZE_HISTOGRAM
/// -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
/// ```
///
/// Off by default: the wrappers add two atomics to every allocation, which is
/// acceptable for an investigation build and not for a shipping one.

#include "donner/base/HeapSizeHistogram.h"

#ifdef DONNER_HEAP_SIZE_HISTOGRAM

#include <malloc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
void* __real_malloc(std::size_t size);
void __real_free(void* pointer);
void* __real_calloc(std::size_t count, std::size_t size);
void* __real_realloc(void* pointer, std::size_t size);
}

namespace donner {

namespace {

/// Bucket index for @p bytes: bucket `k` holds sizes in `[2^k, 2^(k+1))`, so
/// the index is the position of the high bit. Sizes at or above `2^(N-1)` all
/// land in the last bucket.
std::size_t BucketFor(std::size_t bytes) {
  std::size_t bucket = 0;
  while (bytes > 1 && bucket + 1 < kHeapSizeBucketCount) {
    bytes >>= 1;
    ++bucket;
  }
  return bucket;
}

struct Bucket {
  std::atomic<std::int64_t> liveBytes{0};
  std::atomic<std::int64_t> liveBlocks{0};
  std::atomic<std::int64_t> peakLiveBytes{0};
};

Bucket* Buckets() {
  // Deliberately a plain array with static storage rather than a function-local
  // static: the wrappers run before and after every other initializer in the
  // program, including the guard variable a function-local static would need.
  static Bucket buckets[kHeapSizeBucketCount];
  return buckets;
}

/// Ring of recent large requested sizes. Written by any thread; a torn read is
/// a wrong number in a diagnostic list, which is cheaper than a lock on every
/// allocation.
std::atomic<std::int64_t> g_recentLarge[kRecentLargeAllocationCount];
std::atomic<std::int64_t> g_largeCount{0};

void NoteRequestedSize(std::size_t requested) {
  if (requested < kLargeAllocationBytes) {
    return;
  }
  const std::int64_t index = g_largeCount.fetch_add(1, std::memory_order_relaxed);
  g_recentLarge[static_cast<std::size_t>(index) % kRecentLargeAllocationCount].store(
      static_cast<std::int64_t>(requested), std::memory_order_relaxed);
}

/// Currently-live large blocks, as (pointer, usable bytes) pairs. A recent-size
/// ring answers "what sizes are being allocated"; this answers "what sizes are
/// still held", which is the question a retention bug turns on. Linear scan of
/// a fixed table: large blocks are rare enough that this never shows up next to
/// the cost of allocating one.
struct LiveLargeBlock {
  std::atomic<std::uintptr_t> pointer{0};
  std::atomic<std::int64_t> bytes{0};
  std::atomic<std::int64_t> tag{0};
};

LiveLargeBlock* LiveLargeBlocks() {
  static LiveLargeBlock blocks[kLiveLargeBlockCount];
  return blocks;
}

/// The calling thread's current \ref AllocTag. A plain `thread_local` scalar
/// with a constant initializer: the wrappers run before the program's dynamic
/// initializers, so anything needing a guard variable would recurse.
thread_local AllocTag t_currentTag = AllocTag::Untagged;

/// Large allocations that arrived while the live-block table was full. A
/// non-zero value here is the reason to distrust the per-tag totals.
std::atomic<std::int64_t> g_tableOverflows{0};

struct TagCounters {
  std::atomic<std::int64_t> liveBytes{0};
  std::atomic<std::int64_t> liveBlocks{0};
  std::atomic<std::int64_t> peakLiveBytes{0};
  std::atomic<std::int64_t> totalAllocations{0};
};

TagCounters* Tags() {
  static TagCounters tags[kAllocTagCount];
  return tags;
}

void RecordTag(AllocTag tag, std::int64_t usable, std::int64_t sign) {
  TagCounters& counters = Tags()[static_cast<std::size_t>(tag)];
  const std::int64_t live =
      counters.liveBytes.fetch_add(sign * usable, std::memory_order_relaxed) + sign * usable;
  counters.liveBlocks.fetch_add(sign, std::memory_order_relaxed);
  if (sign > 0) {
    counters.totalAllocations.fetch_add(1, std::memory_order_relaxed);
    std::int64_t peak = counters.peakLiveBytes.load(std::memory_order_relaxed);
    while (live > peak &&
           !counters.peakLiveBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
      // `peak` is refreshed by the failed exchange; retry against it.
    }
  }
}

void TrackLiveLarge(void* pointer, std::int64_t usable, std::int64_t sign) {
  if (usable < static_cast<std::int64_t>(kLargeAllocationBytes)) {
    return;
  }
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  LiveLargeBlock* blocks = LiveLargeBlocks();
  if (sign > 0) {
    const AllocTag tag = t_currentTag;
    for (std::size_t index = 0; index < kLiveLargeBlockCount; ++index) {
      std::uintptr_t empty = 0;
      if (blocks[index].pointer.compare_exchange_strong(empty, address,
                                                        std::memory_order_relaxed)) {
        blocks[index].bytes.store(usable, std::memory_order_relaxed);
        blocks[index].tag.store(static_cast<std::int64_t>(tag), std::memory_order_relaxed);
        // Counted only once the slot is claimed, so that every increment has a
        // matching decrement when the block is freed. A table overflow shows up
        // as tag totals that under-count the size histogram, which is a visible
        // and checkable discrepancy; a `+1` with no possible `-1` would instead
        // show up as a tag that appears to retain forever.
        RecordTag(tag, usable, 1);
        return;
      }
    }
    g_tableOverflows.fetch_add(1, std::memory_order_relaxed);
    return;  // Table full; the size histogram still counts the bytes.
  }
  for (std::size_t index = 0; index < kLiveLargeBlockCount; ++index) {
    std::uintptr_t held = address;
    if (blocks[index].pointer.compare_exchange_strong(held, 0, std::memory_order_relaxed)) {
      // Credit the release to the tag that allocated the block, not to whatever
      // is on the stack now: a block freed under a different tag would
      // otherwise drive one tag's live total negative and another's up forever.
      RecordTag(static_cast<AllocTag>(blocks[index].tag.load(std::memory_order_relaxed)), usable,
                -1);
      blocks[index].bytes.store(0, std::memory_order_relaxed);
      blocks[index].tag.store(0, std::memory_order_relaxed);
      return;
    }
  }
  // The block was allocated while the table was full, so its tag is unknown and
  // no tag total counted it on the way in. Nothing to undo.
}

void Record(void* pointer, std::int64_t sign) {
  if (pointer == nullptr) {
    return;
  }
  const std::size_t usable = malloc_usable_size(pointer);
  Bucket& bucket = Buckets()[BucketFor(usable)];
  const std::int64_t live = bucket.liveBytes.fetch_add(sign * static_cast<std::int64_t>(usable),
                                                       std::memory_order_relaxed) +
                            sign * static_cast<std::int64_t>(usable);
  bucket.liveBlocks.fetch_add(sign, std::memory_order_relaxed);
  TrackLiveLarge(pointer, static_cast<std::int64_t>(usable), sign);
  if (sign > 0) {
    std::int64_t peak = bucket.peakLiveBytes.load(std::memory_order_relaxed);
    while (live > peak &&
           !bucket.peakLiveBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
      // `peak` is refreshed by the failed exchange; retry against it.
    }
  }
}

}  // namespace

HeapSizeHistogram SampleHeapSizeHistogram() {
  HeapSizeHistogram histogram;
  for (std::size_t index = 0; index < kHeapSizeBucketCount; ++index) {
    histogram.liveBytes[index] = Buckets()[index].liveBytes.load(std::memory_order_relaxed);
    histogram.liveBlocks[index] = Buckets()[index].liveBlocks.load(std::memory_order_relaxed);
    histogram.peakLiveBytes[index] = Buckets()[index].peakLiveBytes.load(std::memory_order_relaxed);
  }
  for (std::size_t index = 0; index < kRecentLargeAllocationCount; ++index) {
    histogram.recentLargeBytes[index] = g_recentLarge[index].load(std::memory_order_relaxed);
  }
  histogram.largeAllocationCount = g_largeCount.load(std::memory_order_relaxed);
  for (std::size_t index = 0; index < kLiveLargeBlockCount; ++index) {
    histogram.liveLargeBytes[index] =
        LiveLargeBlocks()[index].bytes.load(std::memory_order_relaxed);
    histogram.liveLargeTags[index] = LiveLargeBlocks()[index].tag.load(std::memory_order_relaxed);
  }
  histogram.tableOverflows = g_tableOverflows.load(std::memory_order_relaxed);
  return histogram;
}

AllocTagTotals SampleAllocTagTotals() {
  AllocTagTotals totals;
  for (std::size_t index = 0; index < kAllocTagCount; ++index) {
    totals.liveBytes[index] = Tags()[index].liveBytes.load(std::memory_order_relaxed);
    totals.liveBlocks[index] = Tags()[index].liveBlocks.load(std::memory_order_relaxed);
    totals.peakLiveBytes[index] = Tags()[index].peakLiveBytes.load(std::memory_order_relaxed);
    totals.totalAllocations[index] = Tags()[index].totalAllocations.load(std::memory_order_relaxed);
  }
  return totals;
}

bool HeapSizeHistogramEnabled() {
  return true;
}

ScopedAllocTag::ScopedAllocTag(AllocTag tag) : previous_(t_currentTag) {
  t_currentTag = tag;
}

ScopedAllocTag::~ScopedAllocTag() {
  t_currentTag = previous_;
}

}  // namespace donner

extern "C" {

void* __wrap_malloc(std::size_t size) {
  void* pointer = __real_malloc(size);
  donner::NoteRequestedSize(size);
  donner::Record(pointer, 1);
  return pointer;
}

void __wrap_free(void* pointer) {
  donner::Record(pointer, -1);
  __real_free(pointer);
}

void* __wrap_calloc(std::size_t count, std::size_t size) {
  void* pointer = __real_calloc(count, size);
  donner::NoteRequestedSize(count * size);
  donner::Record(pointer, 1);
  return pointer;
}

void* __wrap_realloc(void* pointer, std::size_t size) {
  donner::Record(pointer, -1);
  void* result = __real_realloc(pointer, size);
  donner::NoteRequestedSize(size);
  donner::Record(result, 1);
  return result;
}

}  // extern "C"

#else

namespace donner {

HeapSizeHistogram SampleHeapSizeHistogram() {
  return HeapSizeHistogram{};
}

AllocTagTotals SampleAllocTagTotals() {
  return AllocTagTotals{};
}

bool HeapSizeHistogramEnabled() {
  return false;
}

ScopedAllocTag::ScopedAllocTag(AllocTag tag) : previous_(tag) {}

ScopedAllocTag::~ScopedAllocTag() {
  // Reading the field keeps the "unused private field" diagnostic honest
  // without an attribute: in this configuration the guard is genuinely inert,
  // but the member still has to exist so the class layout does not change with
  // the feature flag.
  (void)previous_;
}

}  // namespace donner

#endif  // DONNER_HEAP_SIZE_HISTOGRAM

namespace donner {

// Outside the feature guard: the browser bridge publishes this name table in
// every configuration, and a reader of a shipping build should see the tags
// with zeroes rather than an absent key set.
const char* AllocTagName(AllocTag tag) {
  switch (tag) {
    case AllocTag::Untagged: return "untagged";
    case AllocTag::WorkerRenderFrame: return "workerRenderFrame";
    case AllocTag::WorkerBuildPreview: return "workerBuildPreview";
    case AllocTag::WorkerFinalSnapshot: return "workerFinalSnapshot";
    case AllocTag::WorkerOther: return "workerOther";
    case AllocTag::AppPollResult: return "appPollResult";
    case AllocTag::AppUiFrame: return "appUiFrame";
    case AllocTag::AppHostFrame: return "appHostFrame";
    case AllocTag::AppInput: return "appInput";
    case AllocTag::RenderTileRaster: return "renderTileRaster";
    case AllocTag::GpuReadbackStaging: return "gpuReadbackStaging";
    case AllocTag::CompositorBitmap: return "compositorBitmap";
    case AllocTag::PresentationUpload: return "presentationUpload";
    case AllocTag::ImGuiDrawLists: return "imguiDrawLists";
  }
  return "unknown";
}

}  // namespace donner
