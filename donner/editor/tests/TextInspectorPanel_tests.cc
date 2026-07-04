#include "donner/editor/TextInspectorPanel.h"

#include <gmock/gmock.h>
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
  EXPECT_THAT(app.selectedElements(), ::testing::ElementsAre(*text));
}

}  // namespace
}  // namespace donner::editor
