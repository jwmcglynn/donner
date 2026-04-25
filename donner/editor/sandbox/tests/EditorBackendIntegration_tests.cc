/// @file
///
/// Integration tests for the `donner_editor_backend` binary — verifies that
/// kLoadBytes with a valid SVG produces a kFrame response with a non-empty
/// render wire, and that kSetViewport works.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/editor/sandbox/SandboxHost.h"
#include "donner/editor/sandbox/SandboxSession.h"
#include "donner/editor/sandbox/SessionCodec.h"
#include "donner/editor/sandbox/SessionProtocol.h"

namespace donner::editor::sandbox {
namespace {

/// Helper: builds a complete session-framed request.
std::vector<uint8_t> MakeRequest(uint64_t requestId, SessionOpcode opcode,
                                 std::vector<uint8_t> payload = {}) {
  SessionFrame frame;
  frame.requestId = requestId;
  frame.opcode = opcode;
  frame.payload = std::move(payload);
  return EncodeFrame(frame);
}

class EditorBackendIntegrationTest : public ::testing::Test {
protected:
  std::string BackendPath() {
    return Runfiles::instance().Rlocation("donner/editor/sandbox/donner_editor_backend");
  }

  SandboxSessionOptions MakeOptions() {
    SandboxSessionOptions opts;
    opts.childBinaryPath = BackendPath();
    opts.autoRespawn = false;
    return opts;
  }

  WireResponse DoHandshake(SandboxSession& session, uint64_t requestId = 1) {
    WireRequest req;
    req.bytes = MakeRequest(requestId, SessionOpcode::kHandshake,
                            EncodeHandshake({kSessionProtocolVersion, "test"}));
    auto future = session.submit(std::move(req));
    return future.get();
  }
};

TEST_F(EditorBackendIntegrationTest, LoadBytesProducesRenderWire) {
  SandboxSession session(MakeOptions());
  ASSERT_TRUE(session.childAlive());

  WireResponse hsResp = DoHandshake(session);
  ASSERT_EQ(hsResp.status, SandboxStatus::kOk);

  // Send kLoadBytes with a minimal valid SVG.
  const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)"
                          R"(<rect x="10" y="10" width="80" height="80" fill="red"/>)"
                          R"(</svg>)";

  LoadBytesPayload loadPayload;
  loadPayload.bytes = svg;
  std::vector<uint8_t> encoded = EncodeLoadBytes(loadPayload);

  WireRequest req;
  req.bytes = MakeRequest(10, SessionOpcode::kLoadBytes, std::move(encoded));
  auto future = session.submit(std::move(req));
  WireResponse resp = future.get();

  ASSERT_EQ(resp.status, SandboxStatus::kOk);

  // Decode the kFrame response.
  FramePayload frame;
  ASSERT_TRUE(DecodeFrame(resp.bytes, frame)) << "Failed to decode FramePayload";

  EXPECT_GT(frame.frameId, 0u);
  EXPECT_TRUE(frame.hasFinalBitmap) << "kLoadBytes should produce a finalBitmap";
  EXPECT_EQ(frame.statusKind, FrameStatusKind::kRendered);
}

TEST_F(EditorBackendIntegrationTest, SetViewportUpdatesRendering) {
  SandboxSession session(MakeOptions());
  ASSERT_TRUE(session.childAlive());
  DoHandshake(session);

  // Load a document first.
  const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50">)"
                          R"(<circle cx="25" cy="25" r="20" fill="blue"/>)"
                          R"(</svg>)";

  LoadBytesPayload loadPayload;
  loadPayload.bytes = svg;

  WireRequest loadReq;
  loadReq.bytes = MakeRequest(10, SessionOpcode::kLoadBytes, EncodeLoadBytes(loadPayload));
  session.submit(std::move(loadReq)).get();

  // Now set viewport and verify we get a valid frame back.
  SetViewportPayload vp;
  vp.width = 800;
  vp.height = 600;

  WireRequest vpReq;
  vpReq.bytes = MakeRequest(11, SessionOpcode::kSetViewport, EncodeSetViewport(vp));
  auto future = session.submit(std::move(vpReq));
  WireResponse resp = future.get();

  ASSERT_EQ(resp.status, SandboxStatus::kOk);

  FramePayload frame;
  ASSERT_TRUE(DecodeFrame(resp.bytes, frame));
  EXPECT_TRUE(frame.hasFinalBitmap);
}

TEST_F(EditorBackendIntegrationTest, UndoRedoRoundTrip) {
  SandboxSession session(MakeOptions());
  ASSERT_TRUE(session.childAlive());
  DoHandshake(session);

  // Load a document.
  const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="50" height="50">)"
                          R"(<rect x="5" y="5" width="40" height="40" fill="green"/>)"
                          R"(</svg>)";

  LoadBytesPayload loadPayload;
  loadPayload.bytes = svg;

  WireRequest loadReq;
  loadReq.bytes = MakeRequest(10, SessionOpcode::kLoadBytes, EncodeLoadBytes(loadPayload));
  session.submit(std::move(loadReq)).get();

  // Send undo — should succeed even with nothing to undo.
  WireRequest undoReq;
  undoReq.bytes = MakeRequest(20, SessionOpcode::kUndo);
  auto undoFuture = session.submit(std::move(undoReq));
  WireResponse undoResp = undoFuture.get();
  ASSERT_EQ(undoResp.status, SandboxStatus::kOk);

  FramePayload frame;
  ASSERT_TRUE(DecodeFrame(undoResp.bytes, frame));
  EXPECT_TRUE(frame.hasFinalBitmap);

  // Send redo.
  WireRequest redoReq;
  redoReq.bytes = MakeRequest(21, SessionOpcode::kRedo);
  auto redoFuture = session.submit(std::move(redoReq));
  WireResponse redoResp = redoFuture.get();
  ASSERT_EQ(redoResp.status, SandboxStatus::kOk);

  FramePayload redoFrame;
  ASSERT_TRUE(DecodeFrame(redoResp.bytes, redoFrame));
  EXPECT_TRUE(redoFrame.hasFinalBitmap);
}

// Regression: pinch-zoom on a slow renderer (geode) used to leave the
// editor unresponsive for >5s because every pinch event posted a fresh
// `kSetViewport` and the backend processed each one serially before
// reaching whatever pointer event the user fired afterwards.
//
// The fix is server-side request coalescing: whenever the backend
// sees a batch of buffered requests on its read pipe, all but the
// LATEST `kSetViewport` are skipped and the held requestIds get the
// same final-frame response. After the fix, a burst of viewport
// updates followed by a click resolves the click within roughly one
// render's worth of work — not "burst-size × per-frame work."
//
// Test shape: post 100 `kSetViewport` requests at increasing sizes
// without awaiting each, then post a `kPointerEvent` (kDown), and
// time how long until the pointer event's response arrives. With
// coalescing it should be < 1 second on tiny_skia even for a
// non-trivial document; without it, the same machine takes multiple
// seconds, and on geode it's the multi-second user-visible stall.
TEST_F(EditorBackendIntegrationTest, BurstSetViewportThenPointerEventCoalesces) {
  SandboxSession session(MakeOptions());
  ASSERT_TRUE(session.childAlive());
  DoHandshake(session);

  // Load a document that's non-trivial enough that each rasterize
  // takes a few ms (so a burst stalls measurably without coalescing)
  // but small enough not to dominate CI runtime.
  const std::string svg =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">)"
      R"(<rect x="10" y="10" width="180" height="180" fill="red"/>)"
      R"(<circle cx="100" cy="100" r="60" fill="blue"/>)"
      R"(<rect id="target" x="80" y="80" width="40" height="40" fill="green"/>)"
      R"(</svg>)";

  LoadBytesPayload loadPayload;
  loadPayload.bytes = svg;
  WireRequest loadReq;
  loadReq.bytes = MakeRequest(10, SessionOpcode::kLoadBytes, EncodeLoadBytes(loadPayload));
  session.submit(std::move(loadReq)).get();

  constexpr int kBurstSize = 100;
  std::vector<std::future<WireResponse>> setViewportFutures;
  setViewportFutures.reserve(kBurstSize);

  const auto burstStart = std::chrono::steady_clock::now();

  // Fire all kBurstSize setViewport requests without awaiting any.
  // Sizes vary from 200..299 to make each one a distinct mutation
  // (otherwise some implementations may already short-circuit
  // identical viewport sizes).
  for (int i = 0; i < kBurstSize; ++i) {
    SetViewportPayload vp;
    vp.width = 200 + i;
    vp.height = 200 + i;
    WireRequest req;
    req.bytes =
        MakeRequest(100u + static_cast<uint64_t>(i), SessionOpcode::kSetViewport, EncodeSetViewport(vp));
    setViewportFutures.push_back(session.submit(std::move(req)));
  }

  // Now post the pointer event the user is presumed to have fired
  // immediately after their pinch gesture ended.
  PointerEventPayload ptr;
  ptr.phase = PointerPhase::kDown;
  ptr.documentX = 100.0;
  ptr.documentY = 100.0;
  ptr.buttons = 1;
  ptr.modifiers = 0;
  WireRequest ptrReq;
  ptrReq.bytes = MakeRequest(1000, SessionOpcode::kPointerEvent, EncodePointerEvent(ptr));
  auto ptrFuture = session.submit(std::move(ptrReq));

  // Time how long until the click's response arrives.
  WireResponse ptrResp = ptrFuture.get();
  const auto ptrLatency = std::chrono::steady_clock::now() - burstStart;
  const double ptrLatencyMs =
      std::chrono::duration<double, std::milli>(ptrLatency).count();

  ASSERT_EQ(ptrResp.status, SandboxStatus::kOk);
  FramePayload ptrFrame;
  ASSERT_TRUE(DecodeFrame(ptrResp.bytes, ptrFrame));

  // The full burst pattern is the user-visible stall path: 100
  // setViewport requests + 1 pointer event. With per-request
  // serial processing and a non-trivial render tree, the total
  // wall-clock is N × per-render. With coalescing it's ~2 ×
  // per-render. Pick a budget that's loose enough for slow CI
  // but tight enough to fail at HEAD with no coalescing on
  // tiny_skia (~1 ms/frame × 100 = 100 ms; needed bound: < 500 ms
  // to comfortably fail the "every request gets handled" path
  // without flake on a noisy runner).
  EXPECT_LT(ptrLatencyMs, 500.0)
      << "Click response after a 100-frame setViewport burst should "
         "complete within 500 ms — backend is expected to coalesce "
         "stale setViewports rather than process all of them. "
         "Observed: " << ptrLatencyMs << " ms.";

  // Drain the rest of the setViewport futures so the session shutdown
  // doesn't race with their threads.
  for (auto& f : setViewportFutures) {
    f.get();
  }
}

}  // namespace
}  // namespace donner::editor::sandbox
