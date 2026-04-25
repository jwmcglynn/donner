/// @file
///
/// Unit tests for `donner::editor::ShouldPostDragMove`. The helper is
/// the host-side guard that prevents the editor's drag pipeline from
/// pile-driving SandboxSession's request FIFO with `kMove` events
/// faster than the backend can drain.
///
/// The bug this guard exists to prevent (and these tests pin):
///
///   Without the in-flight gate, every ImGui frame during a drag
///   overwrites `pendingFrame` with a freshly-issued
///   `backend->pointerEvent(...)` future. The session-backed client
///   places each request into a FIFO that the backend serves
///   serially. Discarding the previous future does NOT cancel the
///   request — it just abandons the response. Net result: backend
///   processes events 1..N in order and the host only ever sees
///   N's frame, after the queue drains. With a 30 ms backend
///   round-trip, dragging for 100 ms queues ~6 events; the user sees
///   no preview updates until ~180 ms later, by which time they've
///   released. Symptom: "drag freezes mid-stroke, snaps to release
///   position."
///
/// The gate's contract here is a single line: while a previous
/// `kMove` future is in flight (`pendingFrameInFlight == true`),
/// `ShouldPostDragMove` returns `false` regardless of cursor delta.

#include "donner/editor/DragCoalesce.h"

#include <gtest/gtest.h>

#include <optional>

#include "donner/base/Vector2.h"

namespace donner::editor {
namespace {

// First post is always allowed: there's no prior position recorded
// and no in-flight future yet.
TEST(ShouldPostDragMove, FirstPostAllowedWhenNothingInFlight) {
  EXPECT_TRUE(ShouldPostDragMove(Vector2d(100.0, 100.0), std::optional<Vector2d>(),
                                 /*pendingFrameInFlight=*/false));
}

// First post should still be SUPPRESSED if a prior request is in
// flight — the host hasn't had a chance to consume the first
// future yet, so we don't add a second.
TEST(ShouldPostDragMove, FirstPostSuppressedWhenAlreadyInFlight) {
  EXPECT_FALSE(ShouldPostDragMove(Vector2d(100.0, 100.0), std::optional<Vector2d>(),
                                  /*pendingFrameInFlight=*/true));
}

// Sub-pixel jitter (< 0.25 px) is filtered even when the wire is free.
TEST(ShouldPostDragMove, SubPixelJitterSuppressed) {
  EXPECT_FALSE(ShouldPostDragMove(Vector2d(100.0, 100.1),
                                  std::optional<Vector2d>(Vector2d(100.0, 100.0)),
                                  /*pendingFrameInFlight=*/false));
  EXPECT_FALSE(ShouldPostDragMove(Vector2d(100.2, 100.0),
                                  std::optional<Vector2d>(Vector2d(100.0, 100.0)),
                                  /*pendingFrameInFlight=*/false));
}

// Just over the epsilon should fire (catches the boundary).
TEST(ShouldPostDragMove, JustOverEpsilonAllowed) {
  // 0.25 px epsilon → 0.26 px Euclidean delta clears it.
  EXPECT_TRUE(ShouldPostDragMove(Vector2d(100.26, 100.0),
                                 std::optional<Vector2d>(Vector2d(100.0, 100.0)),
                                 /*pendingFrameInFlight=*/false));
}

// **The regression-pin test.** Cursor moved a meaningful distance
// from the last posted point — but a previous future is still in
// flight, so the host MUST NOT post another. Reverting the new gate
// (deleting the `pendingFrameInFlight` early-return) flips this to
// `true` and the test goes red.
TEST(ShouldPostDragMove, MeaningfulMoveSuppressedWhilePendingInFlight) {
  EXPECT_FALSE(ShouldPostDragMove(Vector2d(150.0, 200.0),
                                  std::optional<Vector2d>(Vector2d(100.0, 100.0)),
                                  /*pendingFrameInFlight=*/true));
}

// And once the wire frees up, the same coalesced position is allowed
// to fire — confirming the gate's only role is "wait, then send the
// LATEST" rather than "drop this position permanently."
TEST(ShouldPostDragMove, MeaningfulMoveAllowedWhenWireFrees) {
  EXPECT_TRUE(ShouldPostDragMove(Vector2d(150.0, 200.0),
                                 std::optional<Vector2d>(Vector2d(100.0, 100.0)),
                                 /*pendingFrameInFlight=*/false));
}

// Simulate a fast-cursor sweep against a slow backend: positions
// 100→101→…→105 issued every frame while a single in-flight kMove
// holds the wire. None should fire. After the wire frees, the LAST
// requested position fires (the previous lastPostedPoint hasn't
// moved because the suppressed posts didn't update it).
TEST(ShouldPostDragMove, FastCursorAgainstSlowBackend) {
  std::optional<Vector2d> lastPosted = Vector2d(100.0, 100.0);

  // Wire is busy for 5 successive ImGui frames worth of cursor motion.
  for (double dx = 1.0; dx <= 5.0; dx += 1.0) {
    EXPECT_FALSE(ShouldPostDragMove(Vector2d(100.0 + dx, 100.0), lastPosted,
                                    /*pendingFrameInFlight=*/true))
        << "wire busy → drop this post; dx=" << dx;
  }
  EXPECT_EQ(lastPosted, Vector2d(100.0, 100.0))
      << "suppressed posts must not advance lastPosted — that's what "
         "lets the LATEST cursor position win when the wire frees";

  // Wire frees. The accumulated 5 px delta clears the epsilon, so we
  // fire with the latest cursor position.
  EXPECT_TRUE(ShouldPostDragMove(Vector2d(105.0, 100.0), lastPosted,
                                 /*pendingFrameInFlight=*/false));
}

}  // namespace
}  // namespace donner::editor
