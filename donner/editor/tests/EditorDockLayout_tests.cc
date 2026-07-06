#include "donner/editor/EditorDockLayout.h"

#include <gtest/gtest.h>

#include "donner/editor/ImGuiInternalIncludes.h"

namespace donner::editor {
namespace {

// Exercises the DockBuilder tree construction and lock gating against a real,
// headless ImGui context (no rendering backend needed). Docking must be enabled
// on the context for DockBuilder to operate.
//
// DockBuilderAddNode(DockSpace) calls DockSpace() internally, which dereferences
// the current window, so the layout can only be built while a host window is
// current inside a frame - exactly how the editor shell builds it.
class EditorDockLayoutTest : public ::testing::Test {
protected:
  static constexpr float kWidth = 1600.0f;
  static constexpr float kHeight = 900.0f;

  void SetUp() override {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(kWidth, kHeight);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.Fonts->Build();
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    context_ = nullptr;
  }

  static EditorDockLayoutParams DefaultParams(bool includeCompositorDebug) {
    EditorDockLayoutParams params;
    params.size = ImVec2(kWidth, kHeight);
    params.includeCompositorDebug = includeCompositorDebug;
    return params;
  }

  // Submits one frame: opens the host window, optionally (re)builds the default
  // layout, submits the DockSpace, and submits each panel window so it binds to
  // its node. Returns the node ids from the build (only meaningful when
  // `build` is true).
  EditorDockNodes RenderDockFrame(ImGuiID dockspaceId, bool locked, bool showCompositorDebug,
                                  bool build, const EditorDockLayoutParams& params = {}) {
    EditorDockNodes nodes;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##dock_host", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                     ImGuiWindowFlags_NoDocking);
    if (build) {
      nodes = BuildDefaultDockLayout(dockspaceId, params);
    }
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), EditorDockSpaceFlags(locked));
    ImGui::End();

    ImGui::Begin(kRenderPaneWindowName);
    ImGui::End();
    ImGui::Begin(kLayersWindowName);
    ImGui::End();
    ImGui::Begin(kInspectorWindowName);
    ImGui::End();
    if (showCompositorDebug) {
      ImGui::Begin(kCompositorDebugWindowName);
      ImGui::End();
    }
    ImGui::Render();
    return nodes;
  }

  static ImGuiID DockNodeIdForWindow(const char* name) {
    ImGuiWindow* window = ImGui::FindWindowByName(name);
    if (window == nullptr || window->DockNode == nullptr) {
      return 0;
    }
    return window->DockNode->ID;
  }

  ImGuiContext* context_ = nullptr;
};

TEST_F(EditorDockLayoutTest, BuildsCanvasCentralNodeWithStackedRightColumn) {
  const ImGuiID dockspaceId = EditorDockSpaceId();
  const EditorDockNodes nodes =
      RenderDockFrame(dockspaceId, /*locked=*/true, /*showCompositorDebug=*/true, /*build=*/true,
                      DefaultParams(/*includeCompositorDebug=*/true));

  EXPECT_EQ(nodes.root, dockspaceId);
  EXPECT_NE(nodes.central, 0u);
  EXPECT_NE(nodes.rightTop, 0u);
  EXPECT_NE(nodes.rightMid, 0u);
  EXPECT_NE(nodes.rightBottom, 0u);
  // Every panel lands in a distinct node.
  EXPECT_NE(nodes.central, nodes.rightTop);
  EXPECT_NE(nodes.rightTop, nodes.rightMid);
  EXPECT_NE(nodes.rightMid, nodes.rightBottom);

  ImGuiDockNode* central = ImGui::DockBuilderGetNode(nodes.central);
  ASSERT_NE(central, nullptr);
  EXPECT_TRUE(central->IsCentralNode());
  EXPECT_TRUE(central->IsNoTabBar());
  EXPECT_TRUE(central->IsLeafNode());
  EXPECT_EQ(ImGui::DockBuilderGetCentralNode(dockspaceId), central);

  // The root is split horizontally (canvas | right column).
  ImGuiDockNode* root = ImGui::DockBuilderGetNode(dockspaceId);
  ASSERT_NE(root, nullptr);
  EXPECT_TRUE(root->IsSplitNode());
  EXPECT_EQ(root->SplitAxis, ImGuiAxis_X);

  // Canvas occupies the majority of the width.
  EXPECT_GT(central->Size.x, kWidth * 0.5f);
}

TEST_F(EditorDockLayoutTest, DocksEachPanelIntoItsDefaultNode) {
  const ImGuiID dockspaceId = EditorDockSpaceId();
  const EditorDockLayoutParams params = DefaultParams(/*includeCompositorDebug=*/true);

  const EditorDockNodes nodes =
      RenderDockFrame(dockspaceId, /*locked=*/true, /*showCompositorDebug=*/true, /*build=*/true,
                      params);
  // A second frame lets the freshly-submitted windows settle into their nodes.
  RenderDockFrame(dockspaceId, /*locked=*/true, /*showCompositorDebug=*/true, /*build=*/false);

  EXPECT_EQ(DockNodeIdForWindow(kRenderPaneWindowName), nodes.central);
  EXPECT_EQ(DockNodeIdForWindow(kLayersWindowName), nodes.rightTop);
  EXPECT_EQ(DockNodeIdForWindow(kInspectorWindowName), nodes.rightMid);
  EXPECT_EQ(DockNodeIdForWindow(kCompositorDebugWindowName), nodes.rightBottom);
}

TEST_F(EditorDockLayoutTest, ExcludesCompositorDebugNodeWhenNotRequested) {
  const ImGuiID dockspaceId = EditorDockSpaceId();
  const EditorDockNodes nodes =
      RenderDockFrame(dockspaceId, /*locked=*/true, /*showCompositorDebug=*/false, /*build=*/true,
                      DefaultParams(/*includeCompositorDebug=*/false));
  RenderDockFrame(dockspaceId, /*locked=*/true, /*showCompositorDebug=*/false, /*build=*/false);

  EXPECT_NE(nodes.central, 0u);
  EXPECT_NE(nodes.rightTop, 0u);
  EXPECT_NE(nodes.rightMid, 0u);
  EXPECT_EQ(nodes.rightBottom, 0u);
  // The Compositor Debug window is never submitted and never bound to a node.
  EXPECT_EQ(DockNodeIdForWindow(kCompositorDebugWindowName), 0u);
}

TEST_F(EditorDockLayoutTest, ResetRestoresDefaultLayoutAfterRearrange) {
  const ImGuiID dockspaceId = EditorDockSpaceId();
  const EditorDockLayoutParams params = DefaultParams(/*includeCompositorDebug=*/true);

  const EditorDockNodes original =
      RenderDockFrame(dockspaceId, /*locked=*/false, /*showCompositorDebug=*/true, /*build=*/true,
                      params);
  RenderDockFrame(dockspaceId, /*locked=*/false, /*showCompositorDebug=*/true, /*build=*/false);

  // Simulate a user rearrange: move the Inspector into the canvas/central node.
  ImGui::DockBuilderDockWindow(kInspectorWindowName, original.central);
  ImGui::DockBuilderFinish(dockspaceId);
  RenderDockFrame(dockspaceId, /*locked=*/false, /*showCompositorDebug=*/true, /*build=*/false);
  EXPECT_EQ(DockNodeIdForWindow(kInspectorWindowName), original.central);

  // Reset rebuilds the default layout; the Inspector returns to its own node.
  const EditorDockNodes rebuilt =
      RenderDockFrame(dockspaceId, /*locked=*/false, /*showCompositorDebug=*/true, /*build=*/true,
                      params);
  RenderDockFrame(dockspaceId, /*locked=*/false, /*showCompositorDebug=*/true, /*build=*/false);
  EXPECT_EQ(DockNodeIdForWindow(kInspectorWindowName), rebuilt.rightMid);
  EXPECT_NE(DockNodeIdForWindow(kInspectorWindowName), original.central);
}

TEST_F(EditorDockLayoutTest, LockGatesResizeSplitAndUndock) {
  const ImGuiDockNodeFlags locked = EditorDockSpaceFlags(true);
  EXPECT_TRUE(locked & ImGuiDockNodeFlags_NoResize);
  EXPECT_TRUE(locked & ImGuiDockNodeFlags_NoDockingSplit);
  EXPECT_TRUE(locked & ImGuiDockNodeFlags_NoUndocking);

  const ImGuiDockNodeFlags unlocked = EditorDockSpaceFlags(false);
  EXPECT_FALSE(unlocked & ImGuiDockNodeFlags_NoResize);
  EXPECT_FALSE(unlocked & ImGuiDockNodeFlags_NoDockingSplit);
  EXPECT_FALSE(unlocked & ImGuiDockNodeFlags_NoUndocking);
  // The canvas central node stays clear of docked panels in both states.
  EXPECT_TRUE(locked & ImGuiDockNodeFlags_NoDockingOverCentralNode);
  EXPECT_TRUE(unlocked & ImGuiDockNodeFlags_NoDockingOverCentralNode);
}

TEST_F(EditorDockLayoutTest, LockFlagsPropagateToLiveDockNodes) {
  const ImGuiID dockspaceId = EditorDockSpaceId();
  const EditorDockLayoutParams params = DefaultParams(/*includeCompositorDebug=*/true);

  RenderDockFrame(dockspaceId, /*locked=*/true, /*showCompositorDebug=*/true, /*build=*/true,
                  params);
  RenderDockFrame(dockspaceId, /*locked=*/true, /*showCompositorDebug=*/true, /*build=*/false);
  ImGuiDockNode* rootLocked = ImGui::DockBuilderGetNode(dockspaceId);
  ASSERT_NE(rootLocked, nullptr);
  EXPECT_TRUE(rootLocked->MergedFlags & ImGuiDockNodeFlags_NoResize);

  RenderDockFrame(dockspaceId, /*locked=*/false, /*showCompositorDebug=*/true, /*build=*/false);
  RenderDockFrame(dockspaceId, /*locked=*/false, /*showCompositorDebug=*/true, /*build=*/false);
  ImGuiDockNode* rootUnlocked = ImGui::DockBuilderGetNode(dockspaceId);
  ASSERT_NE(rootUnlocked, nullptr);
  EXPECT_FALSE(rootUnlocked->MergedFlags & ImGuiDockNodeFlags_NoResize);
}

}  // namespace
}  // namespace donner::editor
