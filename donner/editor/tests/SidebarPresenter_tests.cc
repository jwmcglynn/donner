#include "donner/editor/SidebarPresenter.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/DocumentState.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

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
  ASSERT_EQ(xmlAttributes.size(), 6u);
  EXPECT_EQ(xmlAttributes[0].first, "x");
  EXPECT_EQ(xmlAttributes[0].second, "10");
  EXPECT_EQ(xmlAttributes[1].first, "y");
  EXPECT_EQ(xmlAttributes[1].second, "20");
  EXPECT_EQ(xmlAttributes[2].first, "width");
  EXPECT_EQ(xmlAttributes[2].second, "100");
  EXPECT_EQ(xmlAttributes[3].first, "height");
  EXPECT_EQ(xmlAttributes[3].second, "50");
  EXPECT_EQ(xmlAttributes[4].first, "fill");
  EXPECT_EQ(xmlAttributes[4].second, "red");
  EXPECT_EQ(xmlAttributes[5].first, "id");
  EXPECT_EQ(xmlAttributes[5].second, "target");

  const auto computedStyle = presenter.inspectorComputedStyleForTesting();
  ASSERT_EQ(computedStyle.size(), 9u);
  EXPECT_EQ(computedStyle[0].first, "display");
  EXPECT_EQ(computedStyle[1].first, "visibility");
  EXPECT_EQ(computedStyle[2].first, "opacity");
  EXPECT_EQ(computedStyle[3].first, "fill");
  EXPECT_EQ(computedStyle[4].first, "fill-opacity");
  EXPECT_EQ(computedStyle[5].first, "stroke");
  EXPECT_EQ(computedStyle[6].first, "stroke-width");
  EXPECT_EQ(computedStyle[7].first, "stroke-opacity");
  EXPECT_EQ(computedStyle[8].first, "color");

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
  EXPECT_TRUE(presenter.inspectorXmlAttributesForTesting().empty());
  EXPECT_TRUE(presenter.inspectorComputedStyleForTesting().empty());
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
};

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
