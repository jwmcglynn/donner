/// @file
///
/// Integration tests for the `donner_editor_backend` binary — verifies that
/// kLoadBytes with a valid SVG produces a kFrame response with a non-empty
/// render wire, and that kSetViewport works.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

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

}  // namespace
}  // namespace donner::editor::sandbox
