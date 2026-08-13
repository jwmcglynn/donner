#pragma once
/// @file
/// Per-frame attribution of ASYNCIFY suspend cost (the single-canvas presenter work).
///
/// The browser editor links with whole-module `-sASYNCIFY`. Any call that
/// reaches a JS-side `Asyncify.handleSleep` / `handleAsync` unwinds the wasm
/// stack, returns to the browser event loop, and rewinds when the awaited
/// promise settles. With the whole application on one thread those suspend
/// points sit directly under the UI frame, so their wall cost is UI frame cost
/// and has to be attributed before a frame budget means anything.
///
/// There is no wasm-visible Asyncify counter to read, and the JS runtime's
/// `Asyncify` object is a closure-renamed module-scope binding, so the probe
/// instead brackets the call sites that can suspend. Bracketing is exact for
/// wall time because Asyncify preserves the shadow stack across an unwind:
/// entering a \ref ScopedSuspendPoint runs its constructor once before the
/// call, the unwind returns out of the enclosing function without running
/// destructors, the rewind restores the stack pointer and locals, and the
/// destructor finally runs once on the normal path out. The measured interval
/// therefore spans the whole suspend, not just the pre-suspend CPU work.
///
/// Counters are thread-local: each thread attributes the suspends it performs,
/// and the frame publisher reads only its own thread's totals.

#include <chrono>
#include <cstdint>

namespace donner {

/// Why a call site can suspend. Kept small and stable: it is published to the
/// stats surface as an array index.
enum class SuspendKind : std::uint8_t {
  /// `yieldBetweenTiles`: a deliberate one-turn yield at a compositor tile
  /// boundary so the thread's event loop can service canvas-size commits and
  /// WebGPU callbacks mid-pass.
  TileYield = 0,
  /// Waiting for a GPU readback (`mapAsync` completion) to land.
  GpuReadback = 1,
  /// `device.poll` / `instance.waitAny`: emdawnwebgpu yields the Asyncify
  /// worker for roughly one browser task per call regardless of the `wait`
  /// argument.
  DeviceWait = 2,
  /// Device and adapter acquisition, and anything else that suspends outside
  /// the steady-state frame path.
  Startup = 3,
};

/// Number of distinct \ref SuspendKind values.
inline constexpr std::size_t kSuspendKindCount = 4;

/// Suspend totals accumulated between \ref BeginSuspendFrame and
/// \ref EndSuspendFrame on the calling thread.
struct FrameSuspendTotals {
  /// Suspend points entered during the frame.
  std::uint32_t count = 0;
  /// Summed wall time inside those suspend points, in milliseconds.
  double totalMs = 0.0;
  /// Longest single suspend during the frame, in milliseconds.
  double longestMs = 0.0;
  /// Per-kind entry counts, indexed by \ref SuspendKind.
  std::uint32_t countByKind[kSuspendKindCount] = {};
  /// Per-kind wall time in milliseconds, indexed by \ref SuspendKind.
  double msByKind[kSuspendKindCount] = {};
};

/// Start a new attribution window on the calling thread, discarding whatever
/// was accumulated since the previous \ref EndSuspendFrame.
void BeginSuspendFrame();

/// Close the attribution window opened by \ref BeginSuspendFrame and return
/// what it accumulated. Safe to call without a matching begin; the totals are
/// then everything since the thread's last reset.
[[nodiscard]] FrameSuspendTotals EndSuspendFrame();

/// Read the current window's totals without closing it.
[[nodiscard]] FrameSuspendTotals PeekSuspendFrame();

/// Lifetime totals for the calling thread, never reset by frame boundaries.
/// Used by the boot report, where there is no frame to attribute against.
[[nodiscard]] FrameSuspendTotals LifetimeSuspendTotals();

/// Brackets one call that may suspend the wasm stack under ASYNCIFY.
///
/// Declare it in the narrowest scope that contains the suspending call:
///
/// ```
/// {
///   const ScopedSuspendPoint suspend(SuspendKind::DeviceWait);
///   device.poll(false, nullptr);
/// }
/// ```
///
/// Cheap enough to leave in release builds: two monotonic clock reads and a
/// handful of thread-local adds. It is compiled on every platform so the
/// desktop build exercises the same code the browser build measures.
class ScopedSuspendPoint {
public:
  /// Open a suspend interval attributed to @p kind.
  explicit ScopedSuspendPoint(SuspendKind kind);

  /// Close the interval and fold its wall time into the thread's totals.
  ~ScopedSuspendPoint();

  ScopedSuspendPoint(const ScopedSuspendPoint&) = delete;
  ScopedSuspendPoint& operator=(const ScopedSuspendPoint&) = delete;
  ScopedSuspendPoint(ScopedSuspendPoint&&) = delete;
  ScopedSuspendPoint& operator=(ScopedSuspendPoint&&) = delete;

private:
  SuspendKind kind_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace donner
