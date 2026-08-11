#pragma once
/// @file
/// Live-heap histogram by allocation size (the single-canvas presenter work investigation).
///
/// See `HeapSizeHistogram.cc` for what this measures and how to turn it on. The
/// header compiles and the accessors link in every configuration; without
/// `DONNER_HEAP_SIZE_HISTOGRAM` they report an empty histogram and
/// \ref HeapSizeHistogramEnabled returns false, so callers never need their own
/// preprocessor guard.

#include <cstddef>
#include <cstdint>

namespace donner {

/// Number of power-of-two size buckets. Bucket `k` covers `[2^k, 2^(k+1))`
/// bytes, so 32 buckets reach 2 GiB, past anything a 512 MiB wasm heap can
/// hold.
inline constexpr std::size_t kHeapSizeBucketCount = 32;

/// Requested byte size at or above which an allocation is recorded in
/// \ref HeapSizeHistogram::recentLargeBytes. Chosen to catch anything
/// canvas-scale while ignoring ordinary containers.
inline constexpr std::size_t kLargeAllocationBytes = 4u * 1024u * 1024u;

/// Number of recent large allocation sizes retained.
inline constexpr std::size_t kRecentLargeAllocationCount = 16;

/// Capacity of the live large-block table.
inline constexpr std::size_t kLiveLargeBlockCount = 48;

/// Who was on the stack when a large block was allocated.
///
/// A size histogram says "four blocks of about 19 MiB are live"; it cannot say
/// who holds them, and a category counter cannot either, because the whole
/// point of an unattributed block is that no subsystem is counting it. This
/// closes that gap without a backtrace: a thread-local tag, pushed by a scope
/// guard around the candidate call sites and by the frame-stage brackets, is
/// stored alongside every live large block. It costs one thread-local store per
/// guard and one extra field per tracked block, and it works in an optimized
/// wasm build where return addresses do not symbolize.
enum class AllocTag : std::uint8_t {
  /// No guard was active. A large block landing here is a call site the
  /// investigation has not covered yet.
  Untagged = 0,
  /// `CompositorController::renderFrame` on the render thread.
  WorkerRenderFrame = 1,
  /// Building the composited preview from the compositor's tile state.
  WorkerBuildPreview = 2,
  /// The end-of-frame full-canvas CPU or GPU snapshot.
  WorkerFinalSnapshot = 3,
  /// Render-thread work outside the three stages above.
  WorkerOther = 4,
  /// The app thread accepting a completed epoch, including texture upload.
  AppPollResult = 5,
  /// The app thread's ImGui frame body.
  AppUiFrame = 6,
  /// The app thread's host present (ImGui render, surface acquire, submit).
  AppHostFrame = 7,
  /// Browser input translated into editor events on the app thread.
  AppInput = 8,
  /// Rasterizing one CPU tile of the render result.
  RenderTileRaster = 9,
  /// A GPU texture readback staging buffer.
  GpuReadbackStaging = 10,
  /// A compositor layer or segment bitmap.
  CompositorBitmap = 11,
  /// Presentation-side texture upload staging.
  PresentationUpload = 12,
  /// ImGui's own draw-list vertex and index vectors.
  ImGuiDrawLists = 13,
};

/// Number of distinct \ref AllocTag values.
inline constexpr std::size_t kAllocTagCount = 14;

/// Stable short names for \ref AllocTag, indexed by the enum value.
[[nodiscard]] const char* AllocTagName(AllocTag tag);

/// Live large-block bytes attributed to each \ref AllocTag.
struct AllocTagTotals {
  /// Bytes in live large blocks tagged with this value.
  std::int64_t liveBytes[kAllocTagCount] = {};
  /// Live large blocks tagged with this value.
  std::int64_t liveBlocks[kAllocTagCount] = {};
  /// Highest \ref liveBytes seen for this tag.
  std::int64_t peakLiveBytes[kAllocTagCount] = {};
  /// Large allocations ever made under this tag.
  std::int64_t totalAllocations[kAllocTagCount] = {};
};

/// Read the per-tag live large-block totals.
[[nodiscard]] AllocTagTotals SampleAllocTagTotals();

/// Sets the calling thread's \ref AllocTag for the lifetime of the scope,
/// restoring the previous one on exit so guards nest.
///
/// Innermost wins: a guard inside a frame stage names the finer owner, and the
/// stage keeps everything the finer guards did not claim. Compiles to nothing
/// without `DONNER_HEAP_SIZE_HISTOGRAM`.
class ScopedAllocTag {
public:
  /// Push @p tag as the calling thread's current tag.
  explicit ScopedAllocTag(AllocTag tag);

  /// Restore the tag that was current when this guard was constructed.
  ~ScopedAllocTag();

  ScopedAllocTag(const ScopedAllocTag&) = delete;
  ScopedAllocTag& operator=(const ScopedAllocTag&) = delete;
  ScopedAllocTag(ScopedAllocTag&&) = delete;
  ScopedAllocTag& operator=(ScopedAllocTag&&) = delete;

private:
  AllocTag previous_;
};

/// Live allocation totals split by block size.
struct HeapSizeHistogram {
  /// Currently allocated bytes in each bucket, measured as usable size.
  std::int64_t liveBytes[kHeapSizeBucketCount] = {};
  /// Currently allocated block count in each bucket.
  std::int64_t liveBlocks[kHeapSizeBucketCount] = {};
  /// Highest \ref liveBytes seen in each bucket.
  std::int64_t peakLiveBytes[kHeapSizeBucketCount] = {};
  /// Requested sizes of the most recent large allocations, newest last. A
  /// bucket says a block is "16 to 32 MiB"; this says it is exactly 19,120,128
  /// bytes, which is a 2186-pixel-wide RGBA surface and therefore identifies
  /// its owner by arithmetic instead of by guesswork.
  std::int64_t recentLargeBytes[kRecentLargeAllocationCount] = {};
  /// Total large allocations seen since boot.
  std::int64_t largeAllocationCount = 0;
  /// Usable sizes of the large blocks that are live right now; zero entries are
  /// empty slots.
  std::int64_t liveLargeBytes[kLiveLargeBlockCount] = {};
  /// \ref AllocTag in force when the corresponding \ref liveLargeBytes entry
  /// was allocated, as its integer value.
  std::int64_t liveLargeTags[kLiveLargeBlockCount] = {};
  /// Large allocations that found the live-block table full. Non-zero means the
  /// per-tag totals under-count and \ref kLiveLargeBlockCount needs raising.
  std::int64_t tableOverflows = 0;
};

/// Read the histogram. Cheap: a load per bucket.
[[nodiscard]] HeapSizeHistogram SampleHeapSizeHistogram();

/// Whether the allocator wrappers are linked in. False in shipping builds.
[[nodiscard]] bool HeapSizeHistogramEnabled();

}  // namespace donner
