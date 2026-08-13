#include "donner/base/MemoryAttribution.h"

#include <algorithm>
#include <array>
#include <atomic>

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#include <malloc.h>
#endif

namespace donner {

namespace {

/// One category's counters. Relaxed ordering throughout: these are
/// observational totals, and a publisher that is one frame stale is
/// indistinguishable from a publisher that had not run yet.
struct CategoryCounters {
  std::atomic<std::uint64_t> retained{0};
  std::atomic<std::uint64_t> retainedHighWater{0};
  std::atomic<std::uint64_t> transient{0};
  std::atomic<std::uint64_t> transientHighWater{0};
  std::atomic<std::uint64_t> entries{0};
};

/// One stage's counters. See \ref MemoryStage.
struct StageCounters {
  std::atomic<std::int64_t> net{0};
  std::atomic<std::int64_t> cumulativeNet{0};
  std::atomic<std::int64_t> maxNet{0};
  std::atomic<std::uint64_t> entries{0};
};

struct ProbeState {
  std::array<CategoryCounters, kMemoryCategoryCount> categories;
  std::array<StageCounters, kMemoryStageCount> stages;
  std::atomic<std::uint64_t> totalRetainedHighWater{0};
  std::atomic<std::uint64_t> wasmHeapHighWater{0};
  std::atomic<std::uint64_t> mallocLiveHighWater{0};
};

ProbeState& State() {
  static ProbeState state;
  return state;
}

void RaiseHighWater(std::atomic<std::uint64_t>& highWater, std::uint64_t value) {
  std::uint64_t previous = highWater.load(std::memory_order_relaxed);
  while (value > previous &&
         !highWater.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    // `previous` is refreshed by the failed exchange; retry against it.
  }
}

/// Bytes in allocated malloc blocks right now, or -1 where the platform does
/// not expose it.
std::int64_t LiveHeapBytes() {
#ifdef __EMSCRIPTEN__
  const struct mallinfo info = mallinfo();
  return info.uordblks < 0 ? std::int64_t{0} : static_cast<std::int64_t>(info.uordblks);
#else
  return -1;
#endif
}

/// Fill the process-wide fields, which are read from the platform rather than
/// published by a subsystem.
void FillProcessTotals(MemoryAttributionSample& sample) {
#ifdef __EMSCRIPTEN__
  sample.wasmHeapBytes = static_cast<std::uint64_t>(emscripten_get_heap_size());
  const struct mallinfo info = mallinfo();
  // `mallinfo` fields are `int`; the editor's ceiling is 512 MiB so they cannot
  // wrap, but clamp negatives rather than sign-extending them into the sample.
  const auto nonNegative = [](int value) {
    return value < 0 ? std::uint64_t{0} : static_cast<std::uint64_t>(value);
  };
  sample.mallocLiveBytes = nonNegative(info.uordblks);
  sample.mallocFreeBytes = nonNegative(info.fordblks);
  sample.mallocArenaBytes = nonNegative(info.arena) + nonNegative(info.hblkhd);
#else
  (void)sample;
#endif
}

MemoryAttributionSample Read(bool closeFrameWindow) {
  ProbeState& state = State();
  MemoryAttributionSample sample;
  std::uint64_t totalRetained = 0;
  for (std::size_t index = 0; index < kMemoryCategoryCount; ++index) {
    CategoryCounters& counters = state.categories[index];
    const std::uint64_t retained = counters.retained.load(std::memory_order_relaxed);
    const std::uint64_t transient = closeFrameWindow
                                        ? counters.transient.exchange(0, std::memory_order_relaxed)
                                        : counters.transient.load(std::memory_order_relaxed);
    if (closeFrameWindow) {
      RaiseHighWater(counters.transientHighWater, transient);
    }
    sample.retainedBytes[index] = retained;
    sample.retainedHighWaterBytes[index] =
        counters.retainedHighWater.load(std::memory_order_relaxed);
    sample.transientBytes[index] = transient;
    sample.transientHighWaterBytes[index] =
        counters.transientHighWater.load(std::memory_order_relaxed);
    sample.entryCounts[index] = counters.entries.load(std::memory_order_relaxed);
    totalRetained += retained;
  }

  sample.totalRetainedBytes = totalRetained;
  RaiseHighWater(state.totalRetainedHighWater, totalRetained);
  sample.totalRetainedHighWaterBytes = state.totalRetainedHighWater.load(std::memory_order_relaxed);

  FillProcessTotals(sample);
  RaiseHighWater(state.wasmHeapHighWater, sample.wasmHeapBytes);
  RaiseHighWater(state.mallocLiveHighWater, sample.mallocLiveBytes);
  sample.wasmHeapHighWaterBytes = state.wasmHeapHighWater.load(std::memory_order_relaxed);
  sample.mallocLiveHighWaterBytes = state.mallocLiveHighWater.load(std::memory_order_relaxed);
  return sample;
}

}  // namespace

const char* MemoryCategoryName(MemoryCategory category) {
  switch (category) {
    case MemoryCategory::CompositorSegmentBitmaps: return "compositorSegmentBitmaps";
    case MemoryCategory::CompositorSegmentTextures: return "compositorSegmentTextures";
    case MemoryCategory::CompositorLayerBitmaps: return "compositorLayerBitmaps";
    case MemoryCategory::CompositorLayerTextures: return "compositorLayerTextures";
    case MemoryCategory::RenderResultTiles: return "renderResultTiles";
    case MemoryCategory::WorkerFrameSnapshot: return "workerFrameSnapshot";
    case MemoryCategory::PresentationTiles: return "presentationTiles";
    case MemoryCategory::PresentationOverviewTiles: return "presentationOverviewTiles";
    case MemoryCategory::PresentationRetired: return "presentationRetired";
    case MemoryCategory::LayerThumbnails: return "layerThumbnails";
  }
  return "unknown";
}

void SetRetainedBytes(MemoryCategory category, std::uint64_t bytes) {
  CategoryCounters& counters = State().categories[static_cast<std::size_t>(category)];
  counters.retained.store(bytes, std::memory_order_relaxed);
  RaiseHighWater(counters.retainedHighWater, bytes);
}

void SetEntryCount(MemoryCategory category, std::uint64_t count) {
  State().categories[static_cast<std::size_t>(category)].entries.store(count,
                                                                       std::memory_order_relaxed);
}

void AddTransientBytes(MemoryCategory category, std::uint64_t bytes) {
  State().categories[static_cast<std::size_t>(category)].transient.fetch_add(
      bytes, std::memory_order_relaxed);
}

MemoryAttributionSample SampleMemoryAttribution() {
  return Read(/*closeFrameWindow=*/true);
}

MemoryAttributionSample PeekMemoryAttribution() {
  return Read(/*closeFrameWindow=*/false);
}

const char* MemoryStageName(MemoryStage stage) {
  switch (stage) {
    case MemoryStage::WorkerRenderFrame: return "workerRenderFrame";
    case MemoryStage::WorkerBuildPreview: return "workerBuildPreview";
    case MemoryStage::WorkerFinalSnapshot: return "workerFinalSnapshot";
    case MemoryStage::WorkerOther: return "workerOther";
    case MemoryStage::AppPollResult: return "appPollResult";
    case MemoryStage::AppUiFrame: return "appUiFrame";
    case MemoryStage::AppHostFrame: return "appHostFrame";
    case MemoryStage::AppInput: return "appInput";
  }
  return "unknown";
}

MemoryStageSample SampleMemoryStages() {
  ProbeState& state = State();
  MemoryStageSample sample;
  for (std::size_t index = 0; index < kMemoryStageCount; ++index) {
    StageCounters& counters = state.stages[index];
    sample.netBytes[index] = counters.net.exchange(0, std::memory_order_relaxed);
    sample.cumulativeNetBytes[index] = counters.cumulativeNet.load(std::memory_order_relaxed);
    sample.maxNetBytes[index] = counters.maxNet.load(std::memory_order_relaxed);
    sample.entries[index] = counters.entries.load(std::memory_order_relaxed);
  }
  return sample;
}

AllocTag AllocTagForStage(MemoryStage stage) {
  switch (stage) {
    case MemoryStage::WorkerRenderFrame: return AllocTag::WorkerRenderFrame;
    case MemoryStage::WorkerBuildPreview: return AllocTag::WorkerBuildPreview;
    case MemoryStage::WorkerFinalSnapshot: return AllocTag::WorkerFinalSnapshot;
    case MemoryStage::WorkerOther: return AllocTag::WorkerOther;
    case MemoryStage::AppPollResult: return AllocTag::AppPollResult;
    case MemoryStage::AppUiFrame: return AllocTag::AppUiFrame;
    case MemoryStage::AppHostFrame: return AllocTag::AppHostFrame;
    case MemoryStage::AppInput: return AllocTag::AppInput;
  }
  return AllocTag::Untagged;
}

ScopedHeapDelta::ScopedHeapDelta(MemoryStage stage)
    : stage_(stage), startLiveBytes_(LiveHeapBytes()), tag_(AllocTagForStage(stage)) {
  State().stages[static_cast<std::size_t>(stage)].entries.fetch_add(1, std::memory_order_relaxed);
}

ScopedHeapDelta::~ScopedHeapDelta() {
  if (startLiveBytes_ < 0) {
    return;  // Platform does not expose live heap bytes; nothing to attribute.
  }
  const std::int64_t delta = LiveHeapBytes() - startLiveBytes_;
  StageCounters& counters = State().stages[static_cast<std::size_t>(stage_)];
  counters.net.fetch_add(delta, std::memory_order_relaxed);
  counters.cumulativeNet.fetch_add(delta, std::memory_order_relaxed);
  std::int64_t previousMax = counters.maxNet.load(std::memory_order_relaxed);
  while (delta > previousMax &&
         !counters.maxNet.compare_exchange_weak(previousMax, delta, std::memory_order_relaxed)) {
    // `previousMax` is refreshed by the failed exchange; retry against it.
  }
}

}  // namespace donner
