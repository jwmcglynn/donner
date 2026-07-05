#include "donner/editor/SidebarPresenter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/DocumentState.h"

namespace donner::editor {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Pair;

constexpr std::string_view kInspectorSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80">
         <rect x="10" y="20" width="100" height="50" fill="red" id="target"/>
         <rect id="peer" x="0" y="0" width="10" height="10"/>
       </svg>)";

const std::string* FindInspectorValue(std::span<const std::pair<std::string, std::string>> entries,
                                      std::string_view name) {
  for (const auto& [entryName, entryValue] : entries) {
    if (entryName == name) {
      return &entryValue;
    }
  }

  return nullptr;
}

TEST(SidebarPresenterTest, RefreshSnapshotCapturesXmlAttributesAndComputedStyle) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_TRUE(presenter.inspectorHasSelectionForTesting());

  const auto xmlAttributes = presenter.inspectorXmlAttributesForTesting();
  EXPECT_THAT(xmlAttributes,
              ElementsAre(Pair("x", "10"), Pair("y", "20"), Pair("width", "100"),
                          Pair("height", "50"), Pair("fill", "red"), Pair("id", "target")));

  const auto computedStyle = presenter.inspectorComputedStyleForTesting();
  EXPECT_THAT(computedStyle,
              ElementsAre(Pair("display", ::testing::_), Pair("visibility", ::testing::_),
                          Pair("opacity", ::testing::_), Pair("fill", ::testing::_),
                          Pair("fill-opacity", ::testing::_), Pair("stroke", ::testing::_),
                          Pair("stroke-width", ::testing::_), Pair("stroke-opacity", ::testing::_),
                          Pair("color", ::testing::_)));

  const std::string* displayValue = FindInspectorValue(computedStyle, "display");
  ASSERT_NE(displayValue, nullptr);
  EXPECT_EQ(*displayValue, "inline (default)");

  const std::string* fillValue = FindInspectorValue(computedStyle, "fill");
  ASSERT_NE(fillValue, nullptr);
  EXPECT_EQ(*fillValue, "PaintServer(solid rgba(255, 0, 0, 255)) (set)");

  const std::string* strokeValue = FindInspectorValue(computedStyle, "stroke");
  ASSERT_NE(strokeValue, nullptr);
  EXPECT_EQ(*strokeValue, "PaintServer(none) (default)");

  const std::string* colorValue = FindInspectorValue(computedStyle, "color");
  ASSERT_NE(colorValue, nullptr);
  EXPECT_EQ(*colorValue, "rgba(0, 0, 0, 255) (default)");
}

TEST(SidebarPresenterTest, RefreshSnapshotKeepsAttributesWhenCleanSourceTextIsMissing) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));

  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_TRUE(presenter.inspectorHasSelectionForTesting());
  const auto xmlAttributes = presenter.inspectorXmlAttributesForTesting();
  EXPECT_THAT(xmlAttributes,
              testing::Contains(testing::Pair(std::string("id"), std::string("target"))));
  EXPECT_THAT(xmlAttributes,
              testing::Contains(testing::Pair(std::string("fill"), std::string("red"))));
  EXPECT_THAT(xmlAttributes,
              testing::Contains(testing::Pair(std::string("width"), std::string("100"))));
}

TEST(SidebarPresenterTest, RefreshSnapshotClearsStateWhenNoDocumentLoaded) {
  EditorApp app;

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_FALSE(presenter.hasTreeSnapshotForTesting());
  EXPECT_FALSE(presenter.inspectorHasSelectionForTesting());
  EXPECT_TRUE(presenter.inspectorTitleForTesting().empty());
  EXPECT_TRUE(presenter.inspectorXmlAttributesForTesting().empty());
  EXPECT_TRUE(presenter.inspectorComputedStyleForTesting().empty());
}

TEST(SidebarPresenterTest, RefreshSnapshotCapturesTitleWithoutIdAndPrefixedAttributes) {
  constexpr std::string_view kImageSvg =
      R"(<svg xmlns="http://www.w3.org/2000/svg"
              xmlns:xlink="http://www.w3.org/1999/xlink" width="120" height="80">
           <image xlink:href="texture.png" width="80" height="60"/>
         </svg>)";
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kImageSvg));
  app.setCleanSourceText(kImageSvg);

  auto image = app.document().document().querySelector("image");
  ASSERT_TRUE(image.has_value());
  app.setSelection(*image);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_TRUE(presenter.hasTreeSnapshotForTesting());
  EXPECT_TRUE(presenter.inspectorHasSelectionForTesting());
  EXPECT_EQ(presenter.inspectorTitleForTesting(), "Selected: <image>");

  const auto xmlAttributes = presenter.inspectorXmlAttributesForTesting();
  EXPECT_THAT(xmlAttributes, testing::Contains(testing::Pair(std::string("xlink:href"),
                                                             std::string("texture.png"))));
  EXPECT_THAT(xmlAttributes,
              testing::Contains(testing::Pair(std::string("width"), std::string("80"))));
  EXPECT_THAT(xmlAttributes,
              testing::Contains(testing::Pair(std::string("height"), std::string("60"))));
}

TEST(SidebarPresenterTest, RefreshSnapshotAllowsConcurrentDom) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_TRUE(presenter.inspectorHasSelectionForTesting());
  EXPECT_FALSE(presenter.inspectorXmlAttributesForTesting().empty());
  EXPECT_FALSE(presenter.inspectorComputedStyleForTesting().empty());
}

TEST(SidebarPresenterTest, RefreshSnapshotOmitsInspectorDetailsForMultiSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  auto target = app.document().document().querySelector("#target");
  auto peer = app.document().document().querySelector("#peer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(peer.has_value());

  app.setSelection(std::vector<svg::SVGElement>{*target, *peer});

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  EXPECT_FALSE(presenter.inspectorHasSelectionForTesting());
  EXPECT_THAT(presenter.inspectorXmlAttributesForTesting(), IsEmpty());
  EXPECT_THAT(presenter.inspectorComputedStyleForTesting(), IsEmpty());
}

class SidebarPresenterImGuiTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    ctx_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.ConfigMacOSXBehaviors = false;
    io.Fonts->Build();
  }

  void TearDown() override {
    if (ctx_ != nullptr) {
      ImGui::DestroyContext(ctx_);
      ctx_ = nullptr;
    }
  }

  ImGuiContext* ctx_ = nullptr;

  static bool RenderInspectorFrame(SidebarPresenter& presenter, EditorApp* app,
                                   const char* windowName) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400, 300);
    io.AddMousePosEvent(-1.0f, -1.0f);
    io.AddMouseButtonEvent(0, false);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
    ImGui::Begin(windowName, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    const bool queuedMutation = presenter.renderInspector(app, ViewportState{});
    ImGui::End();
    ImGui::Render();
    return queuedMutation;
  }
};

TEST_F(SidebarPresenterImGuiTest, TreeViewAndInspectorRenderEmptySnapshotReadOnly) {
  SidebarPresenter presenter;
  TreeViewState treeState;
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);

  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_empty_snapshot_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  presenter.renderTreeView(nullptr, treeState);
  const bool queuedMutation = presenter.renderInspector(nullptr, ViewportState{});
  ImGui::End();
  ImGui::Render();

  const ImDrawData* drawData = ImGui::GetDrawData();
  EXPECT_FALSE(queuedMutation);
  EXPECT_FALSE(treeState.selectionChangedInTree);
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
}

TEST_F(SidebarPresenterImGuiTest, InspectorRendersMultiSelectionAndSingleSelectionSnapshots) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  const auto target = app.document().document().querySelector("#target");
  const auto peer = app.document().document().querySelector("#peer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(peer.has_value());

  SidebarPresenter presenter;
  app.setSelection(std::vector<svg::SVGElement>{*target, *peer});
  presenter.refreshSnapshot(app);

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);

  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_multi_selection_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  EXPECT_FALSE(presenter.renderInspector(&app, ViewportState{}));
  ImGui::End();
  ImGui::Render();
  const ImDrawData* multiDrawData = ImGui::GetDrawData();
  ASSERT_NE(multiDrawData, nullptr);
  EXPECT_GT(multiDrawData->TotalVtxCount, 0);

  app.setSelection(*target);
  presenter.refreshSnapshot(app);

  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_single_selection_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  EXPECT_FALSE(presenter.renderInspector(&app, ViewportState{}));
  ImGui::End();
  ImGui::Render();
  const ImDrawData* singleDrawData = ImGui::GetDrawData();
  ASSERT_NE(singleDrawData, nullptr);
  EXPECT_GT(singleDrawData->TotalVtxCount, 0);
}

TEST_F(SidebarPresenterImGuiTest, TreeViewOpensAncestorsForPendingScrollTarget) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));
  app.setCleanSourceText(kInspectorSvg);

  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  TreeViewState treeState;
  treeState.scrollTarget = *target;
  treeState.pendingScroll = true;

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_tree_scroll_target_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  presenter.renderTreeView(nullptr, treeState);
  ImGui::End();
  ImGui::Render();

  EXPECT_TRUE(treeState.pendingScroll);
  EXPECT_FALSE(treeState.selectionChangedInTree);
  const ImDrawData* drawData = ImGui::GetDrawData();
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
}

TEST_F(SidebarPresenterImGuiTest, InspectorRendersSingleSelectionWithoutElementDetails) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(R"(<svg xmlns="http://www.w3.org/2000/svg">
    <defs/>
  </svg>)"));

  const auto defs = app.document().document().querySelector("defs");
  ASSERT_TRUE(defs.has_value());
  app.setSelection(*defs);

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  ASSERT_TRUE(presenter.inspectorHasSelectionForTesting());
  EXPECT_EQ(presenter.inspectorTitleForTesting(), "Selected: <defs>");
  EXPECT_TRUE(presenter.inspectorXmlAttributesForTesting().empty());

  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, "##sidebar_defs_inspector_test"));
  const ImDrawData* drawData = ImGui::GetDrawData();
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
}

TEST_F(SidebarPresenterImGuiTest, InspectorRendersLiveNoSelectionState) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kInspectorSvg));

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);
  ASSERT_FALSE(presenter.inspectorHasSelectionForTesting());

  EXPECT_FALSE(RenderInspectorFrame(presenter, &app, "##sidebar_no_selection_live_test"));
  const ImDrawData* drawData = ImGui::GetDrawData();
  ASSERT_NE(drawData, nullptr);
  EXPECT_GT(drawData->TotalVtxCount, 0);
}

TEST_F(SidebarPresenterImGuiTest, PathOperationButtonsRenderSvgBitmapIcons) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(R"(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="a" x="0" y="0" width="20" height="20"/>
    <rect id="b" x="10" y="10" width="20" height="20"/>
  </svg>)"));

  const std::optional<svg::SVGElement> a = app.document().document().querySelector("#a");
  const std::optional<svg::SVGElement> b = app.document().document().querySelector("#b");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  app.setSelection(std::vector<svg::SVGElement>{*a, *b});

  SidebarPresenter presenter;
  presenter.refreshSnapshot(app);

  constexpr ImTextureID kIconTexture = static_cast<ImTextureID>(0x9876);
  int providerCalls = 0;
  int nonEmptyBitmaps = 0;
  int retinaBitmaps = 0;
  const SidebarPresenter::IconTextureProvider iconTextureProvider =
      [&](std::uint64_t, const svg::RendererBitmap& bitmap) {
        ++providerCalls;
        if (!bitmap.empty() && bitmap.dimensions.x > 0 && bitmap.dimensions.y > 0) {
          ++nonEmptyBitmaps;
        }
        if (!bitmap.empty() && bitmap.dimensions.x >= 36 && bitmap.dimensions.y >= 36) {
          ++retinaBitmaps;
        }
        return SidebarPresenter::IconTexture{
            .texture = kIconTexture,
            .uvBottomRight = Vector2d(1.0, 1.0),
        };
      };

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(400, 300);
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);
  ImGui::NewFrame();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(360, 280), ImGuiCond_Always);
  ImGui::Begin("##sidebar_path_icons_test", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
  presenter.renderInspector(&app, ViewportState{}, iconTextureProvider);
  const ImDrawList* drawList = ImGui::GetWindowDrawList();
  int imageQuads = 0;
  for (int cmdIndex = 0; cmdIndex < drawList->CmdBuffer.Size; ++cmdIndex) {
    const ImDrawCmd& cmd = drawList->CmdBuffer[cmdIndex];
    if (cmd.GetTexID() == kIconTexture) {
      imageQuads += static_cast<int>(cmd.ElemCount / 6u);
    }
  }
  ImGui::End();
  ImGui::Render();

  EXPECT_EQ(providerCalls, 4)
      << "the path operation UI should request Union, Intersect, Subtract Front, and Exclude "
         "icon textures";
  EXPECT_EQ(nonEmptyBitmaps, providerCalls)
      << "path operation buttons must receive Donner-rendered Bootstrap SVG bitmaps";
  EXPECT_EQ(retinaBitmaps, providerCalls)
      << "path operation icons are drawn at 18 logical px and must be rasterized at 2x or "
         "higher before ImGui scales them";
  EXPECT_EQ(imageQuads, 4) << "path operation buttons should render as image quads";
}

}  // namespace
}  // namespace donner::editor
