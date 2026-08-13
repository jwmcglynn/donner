#include "donner/base/AsyncifySuspendProbe.h"

#include <algorithm>

namespace donner {

namespace {

/// Per-thread accumulators. Thread-local rather than atomic because every
/// suspend is attributed to the thread that performed it and only that thread
/// reads its own totals; a shared counter would add cross-thread traffic to a
/// path that runs several times per frame.
struct ThreadState {
  FrameSuspendTotals frame;
  FrameSuspendTotals lifetime;
  /// Depth of nested \ref ScopedSuspendPoint scopes. Only the outermost scope
  /// contributes wall time, so a readback wait that internally polls the device
  /// is counted once rather than twice over the same interval.
  int depth = 0;
};

ThreadState& State() {
  thread_local ThreadState state;
  return state;
}

void Accumulate(FrameSuspendTotals& totals, SuspendKind kind, double elapsedMs) {
  const auto index = static_cast<std::size_t>(kind);
  ++totals.count;
  totals.totalMs += elapsedMs;
  totals.longestMs = std::max(totals.longestMs, elapsedMs);
  ++totals.countByKind[index];
  totals.msByKind[index] += elapsedMs;
}

}  // namespace

void BeginSuspendFrame() {
  State().frame = FrameSuspendTotals{};
}

FrameSuspendTotals EndSuspendFrame() {
  ThreadState& state = State();
  const FrameSuspendTotals totals = state.frame;
  state.frame = FrameSuspendTotals{};
  return totals;
}

FrameSuspendTotals PeekSuspendFrame() {
  return State().frame;
}

FrameSuspendTotals LifetimeSuspendTotals() {
  return State().lifetime;
}

ScopedSuspendPoint::ScopedSuspendPoint(SuspendKind kind)
    : kind_(kind), start_(std::chrono::steady_clock::now()) {
  ++State().depth;
}

ScopedSuspendPoint::~ScopedSuspendPoint() {
  ThreadState& state = State();
  const int depth = state.depth--;
  if (depth > 1) {
    // Nested inside another suspend scope; the outer scope already spans this
    // interval. Counting it again would double-charge the frame.
    return;
  }
  const double elapsedMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
  Accumulate(state.frame, kind_, elapsedMs);
  Accumulate(state.lifetime, kind_, elapsedMs);
}

}  // namespace donner
