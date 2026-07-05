#include "donner/editor/TextInspectorPanel.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

#include "donner/editor/EditorApp.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/DocumentState.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTextDoc =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <text id="t" x="10" y="20" font-family="serif" font-size="16">Hello</text>
       </svg>)";

constexpr std::string_view kTextDocWithOnlyStrokeWidth =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
         <text id="t" stroke-width="3">Plain</text>
       </svg>)";

class TextInspectorPanelTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    imguiContext_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800, 600);
    io.ConfigMacOSXBehaviors = false;
    io.Fonts->Build();
  }

  void TearDown() override {
    if (imguiContext_ != nullptr) {
      ImGui::DestroyContext(imguiContext_);
      imguiContext_ = nullptr;
    }
  }

  ImGuiContext* imguiContext_ = nullptr;

  static bool RenderPanelFrame(TextInspectorPanel& panel, EditorApp& app, double nowSeconds,
                               const char* windowName) {
    ImGui::NewFrame();
    ImGui::Begin(windowName, nullptr, ImGuiWindowFlags_NoSavedSettings);
    const bool queuedMutation = panel.render(&app, nowSeconds);
    ImGui::End();
    ImGui::Render();
    return queuedMutation;
  }
};

// Regression for a QA-found SIGABRT: selecting a <text> and opening the Text
// inspector aborted because `syncBuffersFromSelection` read `textContent()` /
// attributes (raw ECS access) without a scoped read access, while the live
// editor keeps the document in ThreadingMode::ConcurrentDom. The panel must
// guard those reads. At the parent commit this test aborts inside
// `SVGTextContentElement::textContent()`; with the fix it renders cleanly.
TEST_F(TextInspectorPanelTest, RendersSelectedTextUnderConcurrentDomWithoutCrashing) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kTextDoc)));

  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  // Mirror the live editor: any UI-thread ECS read must hold an access guard
  // once the document is in ConcurrentDom.
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  TextInspectorPanel panel;

  ImGui::NewFrame();
  ImGui::Begin("##text_inspector_test", nullptr, ImGuiWindowFlags_NoSavedSettings);
  panel.render(&app, /*nowSeconds=*/0.0);
  ImGui::End();
  ImGui::Render();

  // Reaching here means the access-assert did not abort the process.
  EXPECT_EQ(app.selectedElements().size(), 1u);
}

TEST_F(TextInspectorPanelTest, RenderWithNoLiveAppIsNoOp) {
  TextInspectorPanel panel;

  ImGui::NewFrame();
  ImGui::Begin("##text_inspector_null_app", nullptr, ImGuiWindowFlags_NoSavedSettings);
  EXPECT_FALSE(panel.render(nullptr, /*nowSeconds=*/0.0));
  ImGui::End();
  ImGui::Render();
}

TEST_F(TextInspectorPanelTest, ClearingTrackedTextSelectionCommitsNoCleanMutation) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kTextDoc)));

  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  TextInspectorPanel panel;
  EXPECT_FALSE(RenderPanelFrame(panel, app, /*nowSeconds=*/0.0, "##text_inspector_track_text"));

  app.clearSelection();
  EXPECT_FALSE(RenderPanelFrame(panel, app, /*nowSeconds=*/1.0, "##text_inspector_clear_text"));
  EXPECT_TRUE(app.selectedElements().empty());
  EXPECT_FALSE(app.flushFrame()) << "clearing a clean tracked text edit must not queue a mutation";
}

TEST_F(TextInspectorPanelTest, SwitchingTrackedTextSelectionResyncsWithoutMutation) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="a" x="10" y="20">A</text>
           <text id="b" x="10" y="40" font-family="sans" font-size="18">B</text>
         </svg>)"));

  const std::optional<svg::SVGElement> first = app.document().document().querySelector("#a");
  const std::optional<svg::SVGElement> second = app.document().document().querySelector("#b");
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  TextInspectorPanel panel;
  app.setSelection(*first);
  EXPECT_FALSE(RenderPanelFrame(panel, app, /*nowSeconds=*/0.0, "##text_inspector_first_text"));

  app.setSelection(*second);
  EXPECT_FALSE(RenderPanelFrame(panel, app, /*nowSeconds=*/1.0, "##text_inspector_second_text"));
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), *second);
  EXPECT_FALSE(app.flushFrame()) << "switching clean text selections must not queue a mutation";
}

TEST_F(TextInspectorPanelTest, SwitchingTrackedTextToNonTextSelectionClearsTracking) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t" x="10" y="20">Hello</text>
           <rect id="r" x="0" y="0" width="10" height="10"/>
         </svg>)"));

  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  const std::optional<svg::SVGElement> rect = app.document().document().querySelector("#r");
  ASSERT_TRUE(text.has_value());
  ASSERT_TRUE(rect.has_value());

  TextInspectorPanel panel;
  app.setSelection(*text);
  EXPECT_FALSE(RenderPanelFrame(panel, app, /*nowSeconds=*/0.0, "##text_inspector_text_selection"));

  app.setSelection(*rect);
  EXPECT_FALSE(RenderPanelFrame(panel, app, /*nowSeconds=*/1.0, "##text_inspector_rect_selection"));
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front(), *rect);
  EXPECT_FALSE(app.flushFrame())
      << "switching from text to a non-text selection should only hide UI";
}

TEST_F(TextInspectorPanelTest, RendersSelectedTextWhenOptionalStyleAttributesAreMissing) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(std::string(kTextDocWithOnlyStrokeWidth)));

  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  TextInspectorPanel panel;

  ImGui::NewFrame();
  ImGui::Begin("##text_inspector_missing_attrs_test", nullptr, ImGuiWindowFlags_NoSavedSettings);
  EXPECT_FALSE(panel.render(&app, /*nowSeconds=*/0.0));
  ImGui::End();
  ImGui::Render();

  EXPECT_EQ(app.selectedElements().size(), 1u);
}

}  // namespace
}  // namespace donner::editor
