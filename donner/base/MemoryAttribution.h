#pragma once
/// @file
/// Per-frame attribution of retained bytes by category (the single-canvas presenter work).
///
/// The browser editor links with `-sMAXIMUM_MEMORY=512MB`. When linear memory
/// reaches that ceiling `emscripten_resize_heap` fails and the module aborts,
/// so "how many bytes does a frame retain, and in which subsystem" is a
/// correctness question in the browser, not a tuning question. A single
/// `emscripten_get_heap_size()` number cannot answer it: linear memory is a
/// high-water mark that never shrinks, so it reports the worst instant since
/// boot without saying which subsystem produced it.
///
/// This probe splits that number three ways:
///
///   - **Linear memory** (`wasmHeapBytes`): the wasm `memory.size`, i.e. the
///     allocator's sbrk high water. This is what hits `MAXIMUM_MEMORY`.
///   - **Live malloc bytes** (`mallocLiveBytes`): what is actually reachable.
///     A gap between this and linear memory is allocator retention -
///     fragmentation or a large transient peak - not a leak.
///   - **Per-category retained bytes**: what each subsystem believes it is
///     holding, published by that subsystem. Their sum bounds the part of
///     `mallocLiveBytes` this codebase can explain.
///
/// Two kinds of counter, because two kinds of growth need different questions
/// asked of them:
///
///   - \ref SetRetainedBytes is a level. The owner republishes its current
///     total whenever it changes; the probe tracks the high water. Unbounded
///     retention shows up as a level that climbs and never falls.
///   - \ref AddTransientBytes is a per-frame flow. The owner adds what it
///     allocated during the frame; \ref SampleMemoryAttribution closes the
///     window. A per-frame allocation that should have been a reuse shows up
///     as a flow that stays high while the level stays flat, which is exactly
///     the shape that inflates linear memory without leaking.
///
/// Counters are process-wide atomics rather than thread-locals: the render
/// thread owns the compositor categories and the app thread owns the
/// presentation categories, and the publisher must read both.

#include <cstddef>
#include <cstdint>

#include "donner/base/HeapSizeHistogram.h"

namespace donner {

/// A subsystem that retains or allocates pixel-scale memory. Kept small and
/// stable: values are published to the stats surface as array indices.
enum class MemoryCategory : std::uint8_t {
  /// CPU pixel buffers for the compositor's cached static segments.
  CompositorSegmentBitmaps = 0,
  /// GPU textures for the compositor's cached static segments.
  CompositorSegmentTextures = 1,
  /// CPU pixel buffers for promoted compositor layers.
  CompositorLayerBitmaps = 2,
  /// GPU textures for promoted compositor layers.
  CompositorLayerTextures = 3,
  /// CPU tile payload in the epoch the render thread handed to the app thread.
  RenderResultTiles = 4,
  /// Full-canvas snapshot the worker takes at the end of a frame, CPU or GPU.
  WorkerFrameSnapshot = 5,
  /// Presentation textures for the active (viewport-bounded) tile set.
  PresentationTiles = 6,
  /// Presentation textures for the retained zoom-out overview tile set.
  PresentationOverviewTiles = 7,
  /// Presentation textures waiting out the backend's frames-in-flight window.
  PresentationRetired = 8,
  /// Layers-panel thumbnail textures.
  LayerThumbnails = 9,
};

/// Number of distinct \ref MemoryCategory values.
inline constexpr std::size_t kMemoryCategoryCount = 10;

/// Stable short names for \ref MemoryCategory, indexed by the enum value. Used
/// as the key set of the published stats object so a reader does not have to
/// keep an index table in sync.
[[nodiscard]] const char* MemoryCategoryName(MemoryCategory category);

/// One frame's view of where the process's bytes are.
struct MemoryAttributionSample {
  /// Current retained bytes per category, indexed by \ref MemoryCategory.
  std::uint64_t retainedBytes[kMemoryCategoryCount] = {};
  /// Highest \ref retainedBytes seen since boot, per category.
  std::uint64_t retainedHighWaterBytes[kMemoryCategoryCount] = {};
  /// Bytes allocated during the frame just closed, per category.
  std::uint64_t transientBytes[kMemoryCategoryCount] = {};
  /// Highest single-frame \ref transientBytes seen since boot, per category.
  std::uint64_t transientHighWaterBytes[kMemoryCategoryCount] = {};
  /// Live object count per category (textures, tiles), where the owner tracks one.
  std::uint64_t entryCounts[kMemoryCategoryCount] = {};

  /// Sum of \ref retainedBytes across categories.
  std::uint64_t totalRetainedBytes = 0;
  /// Highest \ref totalRetainedBytes seen since boot.
  std::uint64_t totalRetainedHighWaterBytes = 0;

  /// wasm linear memory size in bytes; 0 where the platform has no such notion.
  std::uint64_t wasmHeapBytes = 0;
  /// Highest \ref wasmHeapBytes seen since boot.
  std::uint64_t wasmHeapHighWaterBytes = 0;
  /// Bytes in allocated malloc blocks, from `mallinfo`; 0 where unavailable.
  std::uint64_t mallocLiveBytes = 0;
  /// Highest \ref mallocLiveBytes seen since boot.
  std::uint64_t mallocLiveHighWaterBytes = 0;
  /// Bytes in the allocator's free lists, from `mallinfo`.
  std::uint64_t mallocFreeBytes = 0;
  /// Total space the allocator has taken from the system, from `mallinfo`.
  std::uint64_t mallocArenaBytes = 0;
};

/// Publish @p bytes as the current retained total for @p category, replacing
/// whatever the category last published.
void SetRetainedBytes(MemoryCategory category, std::uint64_t bytes);

/// Publish @p count as the current live object count for @p category.
void SetEntryCount(MemoryCategory category, std::uint64_t count);

/// Add @p bytes to the frame's allocation flow for @p category.
void AddTransientBytes(MemoryCategory category, std::uint64_t bytes);

/// Read every counter, fold the per-frame flows into their high waters, and
/// open a new frame window. Call once per frame from the publishing thread.
[[nodiscard]] MemoryAttributionSample SampleMemoryAttribution();

/// Read every counter without closing the frame window.
[[nodiscard]] MemoryAttributionSample PeekMemoryAttribution();

/// A bracketed stage of the frame, measured by what it does to the allocator
/// rather than by what a subsystem says it holds.
///
/// The category counters above only see memory a subsystem knows it owns. When
/// live heap grows and no category moves, the bytes are somewhere no owner is
/// reporting - a backend's staging buffer, a queue, a container nobody thought
/// of - and the only way to find them is to ask which part of the frame the
/// allocator grew during. These stages partition the frame so that question has
/// an answer.
enum class MemoryStage : std::uint8_t {
  /// `CompositorController::renderFrame` on the render thread.
  WorkerRenderFrame = 0,
  /// Building the composited preview from the compositor's tile state.
  WorkerBuildPreview = 1,
  /// The end-of-frame full-canvas CPU or GPU snapshot.
  WorkerFinalSnapshot = 2,
  /// Everything else in one render-thread iteration.
  WorkerOther = 3,
  /// The app thread accepting a completed epoch, including texture upload.
  AppPollResult = 4,
  /// The app thread's ImGui frame body.
  AppUiFrame = 5,
  /// The app thread's host present (ImGui render, surface acquire, submit).
  AppHostFrame = 6,
  /// Browser input translated into editor events on the app thread.
  AppInput = 7,
};

/// Number of distinct \ref MemoryStage values.
inline constexpr std::size_t kMemoryStageCount = 8;

/// Stable short names for \ref MemoryStage, indexed by the enum value.
[[nodiscard]] const char* MemoryStageName(MemoryStage stage);

/// Net allocator movement attributed to each stage.
struct MemoryStageSample {
  /// Net live-heap change during the frame just closed, per stage. Negative
  /// means the stage freed more than it allocated.
  std::int64_t netBytes[kMemoryStageCount] = {};
  /// Net live-heap change since boot, per stage. A stage that is in balance
  /// hovers near zero however long the session runs; a stage that retains
  /// climbs without bound, and this is the number that names it.
  std::int64_t cumulativeNetBytes[kMemoryStageCount] = {};
  /// Largest single-entry net growth seen for the stage, in bytes.
  std::int64_t maxNetBytes[kMemoryStageCount] = {};
  /// Times the stage was entered since boot.
  std::uint64_t entries[kMemoryStageCount] = {};
};

/// Read the stage counters and open a new frame window for them.
[[nodiscard]] MemoryStageSample SampleMemoryStages();

/// The \ref AllocTag that names the same part of the frame as @p stage, so a
/// stage bracket and a large-block tag never disagree about what to call it.
[[nodiscard]] AllocTag AllocTagForStage(MemoryStage stage);

/// Brackets one stage of the frame and attributes the allocator movement across
/// it.
///
/// Nesting is allowed but double-counts the inner interval into both stages, so
/// bracket disjoint stages when the totals are meant to add up. The measurement
/// is a `mallinfo` call on entry and exit; that walks the allocator's bins, so
/// it belongs at frame-stage granularity and never inside a loop.
///
/// The bracket also pushes the stage's \ref AllocTag, so every large block
/// allocated inside it is attributed to the stage without a second guard at the
/// call site. A finer \ref ScopedAllocTag nested inside still wins, because tags
/// are innermost-first.
class ScopedHeapDelta {
public:
  /// Open a stage interval attributed to @p stage.
  explicit ScopedHeapDelta(MemoryStage stage);

  /// Close the interval and fold its net allocator movement into the totals.
  ~ScopedHeapDelta();

  ScopedHeapDelta(const ScopedHeapDelta&) = delete;
  ScopedHeapDelta& operator=(const ScopedHeapDelta&) = delete;
  ScopedHeapDelta(ScopedHeapDelta&&) = delete;
  ScopedHeapDelta& operator=(ScopedHeapDelta&&) = delete;

private:
  MemoryStage stage_;
  std::int64_t startLiveBytes_;
  ScopedAllocTag tag_;
};

}  // namespace donner
