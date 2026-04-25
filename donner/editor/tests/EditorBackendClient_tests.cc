/// @file
///
/// Parametric tests for `EditorBackendClient` — runs the same scenarios against
/// both the session-backed (desktop) and in-process implementations, asserting
/// equivalent `FrameResult`s.

#include "donner/editor/EditorBackendClient.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/tests/Runfiles.h"
#include "donner/editor/sandbox/EditorApiCodec.h"
#include "donner/editor/sandbox/SandboxHost.h"
#include "donner/editor/sandbox/SandboxSession.h"
#include "donner/editor/sandbox/SessionCodec.h"
#include "donner/editor/sandbox/SessionProtocol.h"

namespace donner::editor {
namespace {

/// Simple SVG for testing.
constexpr std::string_view kSimpleSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="r1" x="10" y="10" width="80" height="80" fill="red"/>
</svg>)SVG";

/// SVG with multiple children for tree tests.
constexpr std::string_view kMultiRectSvg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
  <rect id="a" x="0" y="0" width="30" height="30" fill="red"/>
  <rect id="b" x="35" y="0" width="30" height="30" fill="green"/>
  <rect id="c" x="70" y="0" width="30" height="30" fill="blue"/>
</svg>)SVG";

/// Invalid SVG for parse-error testing.
constexpr std::string_view kInvalidSvg = R"SVG(<not-svg>broken<)SVG";

enum class ClientKind {
  kSession,
  kInProcess,
};

std::string ClientKindName(const ::testing::TestParamInfo<ClientKind>& info) {
  switch (info.param) {
    case ClientKind::kSession: return "Session";
    case ClientKind::kInProcess: return "InProcess";
  }
  return "Unknown";
}

class EditorBackendClientTest : public ::testing::TestWithParam<ClientKind> {
protected:
  void SetUp() override {
    switch (GetParam()) {
      case ClientKind::kSession: {
        sandbox::SandboxSessionOptions opts;
        opts.childBinaryPath =
            Runfiles::instance().Rlocation("donner/editor/sandbox/donner_editor_backend");
        opts.autoRespawn = true;
        session_ = std::make_unique<sandbox::SandboxSession>(std::move(opts));

        // Perform handshake before creating the client.
        sandbox::SessionFrame handshake;
        handshake.requestId = 1;
        handshake.opcode = sandbox::SessionOpcode::kHandshake;
        handshake.payload = sandbox::EncodeHandshake(
            sandbox::HandshakePayload{sandbox::kSessionProtocolVersion, ""});
        sandbox::WireRequest req;
        req.bytes = sandbox::EncodeFrame(handshake);
        auto future = session_->submit(std::move(req));
        auto resp = future.get();
        ASSERT_EQ(resp.status, sandbox::SandboxStatus::kOk);

        client_ = EditorBackendClient::MakeSessionBacked(*session_);
        break;
      }
      case ClientKind::kInProcess: {
        client_ = EditorBackendClient::MakeInProcess();
        break;
      }
    }
  }

  void TearDown() override {
    client_.reset();
    session_.reset();
  }

  EditorBackendClient& client() { return *client_; }

private:
  std::unique_ptr<sandbox::SandboxSession> session_;
  std::unique_ptr<EditorBackendClient> client_;
};

TEST_P(EditorBackendClientTest, LoadValidSvg) {
  std::vector<uint8_t> bytes(kSimpleSvg.begin(), kSimpleSvg.end());
  auto future = client().loadBytes(bytes, "test://simple.svg");
  FrameResult result = future.get();

  EXPECT_TRUE(result.ok);
  EXPECT_GT(result.frameId, 0u);
  EXPECT_FALSE(result.bitmap.empty()) << "Bitmap should be non-empty after loading valid SVG";
  EXPECT_EQ(result.parseDiagnostics.size(), 0u);
}

TEST_P(EditorBackendClientTest, LoadInvalidSvg) {
  std::vector<uint8_t> bytes(kInvalidSvg.begin(), kInvalidSvg.end());
  auto future = client().loadBytes(bytes, std::nullopt);
  FrameResult result = future.get();

  // The backend loads what it can — it shouldn't crash. Result.ok reflects
  // whether the round-trip completed, not parse success.
  EXPECT_TRUE(result.ok);
  // The bitmap may be empty or non-empty depending on fallback behavior.
}

TEST_P(EditorBackendClientTest, SetViewport) {
  // Load a document first.
  std::vector<uint8_t> bytes(kSimpleSvg.begin(), kSimpleSvg.end());
  auto loadFuture = client().loadBytes(bytes, std::nullopt);
  FrameResult loadResult = loadFuture.get();
  ASSERT_TRUE(loadResult.ok);

  // Set a different viewport (square to match the 100x100 SVG aspect ratio).
  auto vpFuture = client().setViewport(200, 200);
  FrameResult vpResult = vpFuture.get();

  EXPECT_TRUE(vpResult.ok);
  EXPECT_GT(vpResult.frameId, loadResult.frameId);
  EXPECT_FALSE(vpResult.bitmap.empty());
  EXPECT_EQ(vpResult.bitmap.dimensions.x, 200);
  EXPECT_EQ(vpResult.bitmap.dimensions.y, 200);
}

TEST_P(EditorBackendClientTest, UndoRedoNoOp) {
  // Load a document first.
  std::vector<uint8_t> bytes(kSimpleSvg.begin(), kSimpleSvg.end());
  auto loadFuture = client().loadBytes(bytes, std::nullopt);
  FrameResult loadResult = loadFuture.get();
  ASSERT_TRUE(loadResult.ok);

  // Undo on an un-edited doc — should be a no-op.
  auto undoFuture = client().undo();
  FrameResult undoResult = undoFuture.get();
  EXPECT_TRUE(undoResult.ok);

  // Redo on an un-edited doc — should also be a no-op.
  auto redoFuture = client().redo();
  FrameResult redoResult = redoFuture.get();
  EXPECT_TRUE(redoResult.ok);
}

TEST_P(EditorBackendClientTest, PointerEventSelectsRect) {
  // Load an SVG with a rect.
  std::vector<uint8_t> bytes(kSimpleSvg.begin(), kSimpleSvg.end());
  auto loadFuture = client().loadBytes(bytes, std::nullopt);
  FrameResult loadResult = loadFuture.get();
  ASSERT_TRUE(loadResult.ok);

  // Click inside the rect (center at 50, 50).
  PointerEventPayload ev;
  ev.phase = sandbox::PointerPhase::kDown;
  ev.documentPoint = Vector2d(50.0, 50.0);
  ev.buttons = 1;
  ev.modifiers = 0;

  auto ptrFuture = client().pointerEvent(ev);
  FrameResult ptrResult = ptrFuture.get();

  EXPECT_TRUE(ptrResult.ok);
  EXPECT_EQ(ptrResult.selection.selections.size(), 1u)
      << "Clicking inside the rect should select it";
}

TEST_P(EditorBackendClientTest, PointerEventMissClearsSelection) {
  // Load an SVG with a rect.
  std::vector<uint8_t> bytes(kSimpleSvg.begin(), kSimpleSvg.end());
  auto loadFuture = client().loadBytes(bytes, std::nullopt);
  FrameResult loadResult = loadFuture.get();
  ASSERT_TRUE(loadResult.ok);

  // First select the rect.
  PointerEventPayload selectEv;
  selectEv.phase = sandbox::PointerPhase::kDown;
  selectEv.documentPoint = Vector2d(50.0, 50.0);
  selectEv.buttons = 1;
  auto selectFuture = client().pointerEvent(selectEv);
  FrameResult selectResult = selectFuture.get();
  ASSERT_EQ(selectResult.selection.selections.size(), 1u);

  // Click outside the rect.
  PointerEventPayload missEv;
  missEv.phase = sandbox::PointerPhase::kDown;
  missEv.documentPoint = Vector2d(5.0, 5.0);
  missEv.buttons = 1;
  auto missFuture = client().pointerEvent(missEv);
  FrameResult missResult = missFuture.get();

  EXPECT_TRUE(missResult.ok);
  EXPECT_EQ(missResult.selection.selections.size(), 0u)
      << "Clicking outside should clear selection";
}

TEST_P(EditorBackendClientTest, TreeSummaryPopulated) {
  std::vector<uint8_t> bytes(kMultiRectSvg.begin(), kMultiRectSvg.end());
  auto future = client().loadBytes(bytes, std::nullopt);
  FrameResult result = future.get();
  ASSERT_TRUE(result.ok);

  // Tree should contain at least root <svg> + 3 <rect> children.
  ASSERT_GE(result.tree.nodes.size(), 4u);
  EXPECT_EQ(result.tree.rootIndex, 0u);
  EXPECT_GT(result.tree.generation, 0u);

  // Root should be svg.
  EXPECT_EQ(result.tree.nodes[0].tagName, "svg");
  EXPECT_EQ(result.tree.nodes[0].depth, 0u);
  EXPECT_EQ(result.tree.nodes[0].parentIndex, 0xFFFFFFFFu);

  // Children should be rects at depth 1.
  EXPECT_EQ(result.tree.nodes[1].tagName, "rect");
  EXPECT_EQ(result.tree.nodes[1].idAttr, "a");
  EXPECT_EQ(result.tree.nodes[1].depth, 1u);
  EXPECT_EQ(result.tree.nodes[1].parentIndex, 0u);

  EXPECT_EQ(result.tree.nodes[2].tagName, "rect");
  EXPECT_EQ(result.tree.nodes[2].idAttr, "b");

  EXPECT_EQ(result.tree.nodes[3].tagName, "rect");
  EXPECT_EQ(result.tree.nodes[3].idAttr, "c");

  // None should be selected initially.
  for (const auto& node : result.tree.nodes) {
    EXPECT_FALSE(node.selected) << "Node " << node.displayName << " should not be selected";
  }
}

TEST_P(EditorBackendClientTest, SelectElementMarksSelected) {
  std::vector<uint8_t> bytes(kMultiRectSvg.begin(), kMultiRectSvg.end());
  auto loadFuture = client().loadBytes(bytes, std::nullopt);
  FrameResult loadResult = loadFuture.get();
  ASSERT_TRUE(loadResult.ok);
  ASSERT_GE(loadResult.tree.nodes.size(), 4u);

  // Select the second rect ("b") via selectElement — Replace mode.
  const auto& targetNode = loadResult.tree.nodes[2];  // "b"
  ASSERT_EQ(targetNode.idAttr, "b");

  auto selFuture = client().selectElement(targetNode.entityId, targetNode.entityGeneration, 0);
  FrameResult selResult = selFuture.get();
  ASSERT_TRUE(selResult.ok);
  ASSERT_GE(selResult.tree.nodes.size(), 4u);

  // The "b" rect should now be selected.
  EXPECT_TRUE(selResult.tree.nodes[2].selected)
      << "Node 'b' should be selected after selectElement";

  // Other nodes should not be selected.
  EXPECT_FALSE(selResult.tree.nodes[0].selected);
  EXPECT_FALSE(selResult.tree.nodes[1].selected);
  EXPECT_FALSE(selResult.tree.nodes[3].selected);
}

TEST_P(EditorBackendClientTest, SelectElementToggleMode) {
  std::vector<uint8_t> bytes(kMultiRectSvg.begin(), kMultiRectSvg.end());
  auto loadFuture = client().loadBytes(bytes, std::nullopt);
  FrameResult loadResult = loadFuture.get();
  ASSERT_TRUE(loadResult.ok);
  ASSERT_GE(loadResult.tree.nodes.size(), 4u);

  // Select "a" first (Replace).
  const auto& nodeA = loadResult.tree.nodes[1];
  auto selA = client().selectElement(nodeA.entityId, nodeA.entityGeneration, 0);
  FrameResult rA = selA.get();
  ASSERT_TRUE(rA.ok);
  EXPECT_TRUE(rA.tree.nodes[1].selected);

  // Toggle "b" (mode=1) — should add to selection.
  const auto& nodeB = loadResult.tree.nodes[2];
  auto selB = client().selectElement(nodeB.entityId, nodeB.entityGeneration, 1);
  FrameResult rB = selB.get();
  ASSERT_TRUE(rB.ok);
  // Note: toggle on an unselected element adds it. Both should be selected.
  EXPECT_TRUE(rB.tree.nodes[1].selected) << "'a' should still be selected";
  EXPECT_TRUE(rB.tree.nodes[2].selected) << "'b' should now be selected (toggle-on)";

  // Toggle "a" again (mode=1) — should deselect it.
  auto selA2 = client().selectElement(nodeA.entityId, nodeA.entityGeneration, 1);
  FrameResult rA2 = selA2.get();
  ASSERT_TRUE(rA2.ok);
  EXPECT_FALSE(rA2.tree.nodes[1].selected) << "'a' should be deselected (toggle-off)";
  EXPECT_TRUE(rA2.tree.nodes[2].selected) << "'b' should remain selected";
}

INSTANTIATE_TEST_SUITE_P(AllClients, EditorBackendClientTest,
                         ::testing::Values(ClientKind::kSession, ClientKind::kInProcess),
                         ClientKindName);

}  // namespace
}  // namespace donner::editor
