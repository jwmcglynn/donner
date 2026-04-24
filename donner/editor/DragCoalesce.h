#pragma once
/// @file
///
/// Drag coalesce policy: decides whether the editor should forward the
/// current mouse position to the backend as a `kMove` pointer event, or
/// drop it and wait for the in-flight response.
///
/// Two filters compose:
///
/// 1. **Backend cadence gate** — if a previous `kMove` future is still
///    in flight, suppress new posts. Otherwise the host overwrites
///    `pendingFrame` every ImGui frame and discards every promise's
///    future except the latest, so the user sees no preview updates
///    until the backend's FIFO drains. Symptom: drag freezes mid-
///    stroke and snaps to the release position. Reproduces under the
///    session-backed client when a single round-trip is slower than
///    one ImGui frame (e.g. Geode + IPC, ~30–60 ms on macOS).
///
/// 2. **Sub-pixel jitter gate** — if the cursor hasn't moved by more
///    than `kDragMoveScreenEpsilonPx`, skip the post. Saves a backend
///    round-trip on no-op moves the OS reports while the user holds
///    the mouse still.
///
/// Both gates must pass for `ShouldPostDragMove` to return true.

#include <optional>
#include <type_traits>

namespace donner::editor {

/// Cursor must move by more than this many CSS pixels (in screen
/// space) before another `kMove` is posted. Picked to be smaller than
/// 1 px so retina-display sub-pixel deltas still propagate, but big
/// enough that OS jitter while the user holds the mouse still is
/// filtered.
inline constexpr double kDragMoveScreenEpsilonPx = 0.25;

/// Coalesce decision. Templated on the cursor type so the same helper
/// works against `donner::Vector2d`, `ImVec2`, or any other 2D vector
/// that exposes `x` / `y` and supports `(a - b).lengthSquared()` —
/// either as a member or via a free function.
template <typename Vec>
[[nodiscard]] bool ShouldPostDragMove(const Vec& screenPoint,
                                      const std::optional<Vec>& lastPostedScreenPoint,
                                      bool pendingFrameInFlight) {
  if (pendingFrameInFlight) {
    return false;
  }
  if (!lastPostedScreenPoint.has_value()) {
    return true;
  }
  const auto delta = screenPoint - *lastPostedScreenPoint;
  const double dxSquared = static_cast<double>(delta.x) * static_cast<double>(delta.x);
  const double dySquared = static_cast<double>(delta.y) * static_cast<double>(delta.y);
  constexpr double epsSquared = kDragMoveScreenEpsilonPx * kDragMoveScreenEpsilonPx;
  return (dxSquared + dySquared) > epsSquared;
}

}  // namespace donner::editor
