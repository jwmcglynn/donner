/// @file
///
/// Integration tests for `SandboxSession` — verifies the long-lived child
/// lifecycle, handshake, stub responses, crash recovery, and clean shutdown.

#include "donner/editor/sandbox/SandboxSession.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/editor/sandbox/SandboxHost.h"
#include "donner/editor/sandbox/SessionCodec.h"
#include "donner/editor/sandbox/SessionProtocol.h"

namespace donner::editor::sandbox {
namespace {

/// Helper: builds a complete session-framed request (the caller of
/// SandboxSession::submit is responsible for pre-encoding the frame).
std::vector<uint8_t> MakeRequest(uint64_t requestId, SessionOpcode opcode,
                                 std::vector<uint8_t> payload = {}) {
  SessionFrame frame;
  frame.requestId = requestId;
  frame.opcode = opcode;
  frame.payload = std::move(payload);
  return EncodeFrame(frame);
}

class SandboxSessionTest : public ::testing::Test {
protected:
  std::string BackendPath() {
    return Runfiles::instance().Rlocation("donner/editor/sandbox/donner_editor_backend");
  }

  SandboxSessionOptions MakeOptions(bool autoRespawn = true) {
    SandboxSessionOptions opts;
    opts.childBinaryPath = BackendPath();
    opts.autoRespawn = autoRespawn;
    return opts;
  }

  /// Submits a handshake and returns the response.
  WireResponse DoHandshake(SandboxSession& session, uint64_t requestId = 1) {
    WireRequest req;
    req.bytes = MakeRequest(requestId, SessionOpcode::kHandshake);
    auto future = session.submit(std::move(req));
    return future.get();
  }
};

namespace {
/// Returns a string describing `resp` for assertion-failure output. Surfaces
/// the child's stderr tail so flakes aren't opaque.
std::string Describe(const WireResponse& resp) {
  std::string out = "status=" + std::to_string(static_cast<int>(resp.status));
  out += " bytes=" + std::to_string(resp.bytes.size());
  if (!resp.diagnostics.empty()) {
    out += " diagnostics=[" + resp.diagnostics + "]";
  }
  return out;
}
}  // namespace

#define EXPECT_WIRE_OK(resp) EXPECT_EQ((resp).status, SandboxStatus::kOk) << Describe(resp)
#define ASSERT_WIRE_OK(resp) ASSERT_EQ((resp).status, SandboxStatus::kOk) << Describe(resp)

TEST_F(SandboxSessionTest, Handshake) {
  SandboxSession session(MakeOptions());
  ASSERT_TRUE(session.childAlive());

  WireResponse resp = DoHandshake(session);
  EXPECT_WIRE_OK(resp);
  // Payload should contain: u32 version + u64 pid.
  ASSERT_GE(resp.bytes.size(), 12u);

  uint32_t version = 0;
  std::memcpy(&version, resp.bytes.data(), 4);
  EXPECT_EQ(version, kSessionProtocolVersion);

  uint64_t pid = 0;
  std::memcpy(&pid, resp.bytes.data() + 4, 8);
  EXPECT_GT(pid, 0u);
}

TEST_F(SandboxSessionTest, PersistentChild) {
  SandboxSession session(MakeOptions());

  uint64_t firstPid = 0;

  for (int i = 0; i < 5; ++i) {
    WireResponse resp = DoHandshake(session, static_cast<uint64_t>(i + 1));
    ASSERT_WIRE_OK(resp) << "iteration " << i;
    ASSERT_GE(resp.bytes.size(), 12u);

    uint64_t pid = 0;
    std::memcpy(&pid, resp.bytes.data() + 4, 8);

    if (i == 0) {
      firstPid = pid;
    } else {
      EXPECT_EQ(pid, firstPid) << "Child PID changed on iteration " << i;
    }
  }
}

TEST_F(SandboxSessionTest, LoadBytesStub) {
  SandboxSession session(MakeOptions());

  // First do a handshake.
  DoHandshake(session);

  // Submit kLoadBytes with a dummy payload.
  WireRequest req;
  req.bytes = MakeRequest(10, SessionOpcode::kLoadBytes, {0x3C, 0x73, 0x76, 0x67});
  auto future = session.submit(std::move(req));
  WireResponse resp = future.get();

  EXPECT_WIRE_OK(resp);
  // Should be a kFrame placeholder response — at least the frameId + minimal fields.
  EXPECT_GE(resp.bytes.size(), 8u);
}

TEST_F(SandboxSessionTest, ConcurrentInFlight) {
  SandboxSession session(MakeOptions());
  DoHandshake(session);

  // Submit two requests back-to-back.
  WireRequest req1;
  req1.bytes = MakeRequest(20, SessionOpcode::kSetViewport, {0, 0, 0, 0, 0, 0, 0, 0});
  auto future1 = session.submit(std::move(req1));

  WireRequest req2;
  req2.bytes = MakeRequest(21, SessionOpcode::kSetTool, {0, 0, 0, 0});
  auto future2 = session.submit(std::move(req2));

  WireResponse resp1 = future1.get();
  WireResponse resp2 = future2.get();

  EXPECT_WIRE_OK(resp1);
  EXPECT_WIRE_OK(resp2);
}

TEST_F(SandboxSessionTest, CleanShutdown) {
  {
    SandboxSession session(MakeOptions());
    DoHandshake(session);
    // Destructor sends shutdown.
  }
  // If we reach here without hanging, clean shutdown succeeded.
  SUCCEED();
}

TEST_F(SandboxSessionTest, UnknownOpcodeReturnsError) {
  SandboxSession session(MakeOptions());
  DoHandshake(session);

  // Send an opcode in the request range that the backend doesn't know.
  WireRequest req;
  req.bytes = MakeRequest(50, static_cast<SessionOpcode>(199));
  auto future = session.submit(std::move(req));
  WireResponse resp = future.get();

  EXPECT_WIRE_OK(resp);
  // The payload should be a kError response with kUnknownOpcode.
  ASSERT_GE(resp.bytes.size(), 4u);
  uint32_t errorKind = 0;
  std::memcpy(&errorKind, resp.bytes.data(), 4);
  EXPECT_EQ(errorKind, static_cast<uint32_t>(SessionErrorKind::kUnknownOpcode));
}

TEST_F(SandboxSessionTest, ExportStub) {
  SandboxSession session(MakeOptions());
  DoHandshake(session);

  WireRequest req;
  req.bytes = MakeRequest(60, SessionOpcode::kExport, {0, 0, 0, 0});
  auto future = session.submit(std::move(req));
  WireResponse resp = future.get();

  EXPECT_WIRE_OK(resp);
  // kExportResponse with format=0, bytesLength=0.
  ASSERT_GE(resp.bytes.size(), 8u);
}

}  // namespace
}  // namespace donner::editor::sandbox
