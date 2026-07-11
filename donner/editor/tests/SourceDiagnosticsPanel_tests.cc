#include "donner/editor/SourceDiagnosticsPanel.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {
namespace {

TEST(SourceDiagnosticsPanel, CountsSeverityAndFindsStableId) {
  const std::array diagnostics = {
      SourceDiagnostic{.id = 11, .severity = DiagnosticSeverity::Warning, .message = "warning"},
      SourceDiagnostic{.id = 12, .severity = DiagnosticSeverity::Error, .message = "error"},
      SourceDiagnostic{.id = 13, .severity = DiagnosticSeverity::Error, .message = "error"},
  };

  EXPECT_EQ(CountSourceDiagnostics(diagnostics), (SourceDiagnosticCounts{2, 1}));
  ASSERT_NE(FindSourceDiagnostic(diagnostics, 12), nullptr);
  EXPECT_EQ(FindSourceDiagnostic(diagnostics, 12)->severity, DiagnosticSeverity::Error);
  EXPECT_EQ(FindSourceDiagnostic(diagnostics, 99), nullptr);
}

// ---------------------------------------------------------------------------
// Rendered panel behavior (headless ImGui)
// ---------------------------------------------------------------------------

class SourceDiagnosticsPanelImGuiTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400.0f, 300.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigMacOSXBehaviors = false;
    io.Fonts->Build();
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    context_ = nullptr;
  }

  /// Render one panel frame in a fixed 400x300 host window with the given
  /// mouse state, returning the panel's per-frame action.
  SourceDiagnosticsPanelAction Frame(std::span<const SourceDiagnostic> diagnostics,
                                     std::optional<std::uint64_t> sourceHoveredId, float height,
                                     const ImVec2& mouse = ImVec2(-100.0f, -100.0f),
                                     bool mouseDown = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400.0f, 300.0f);
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, mouseDown);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f), ImGuiCond_Always);
    ImGui::Begin("##diagnostics_host", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    const SourceDiagnosticsPanelAction action = panel_.render(diagnostics, sourceHoveredId, height);
    ImGui::End();
    ImGui::Render();
    const ImDrawData* drawData = ImGui::GetDrawData();
    lastVertexCount_ = drawData != nullptr ? drawData->TotalVtxCount : 0;
    return action;
  }

  /// Screen-space center of a diagnostics row. The host window sits at (0, 0)
  /// with the default (8, 8) window padding; the panel draws a 34px header
  /// (at the default font scale) above 30px rows.
  static ImVec2 RowCenter(std::size_t rowIndex) {
    constexpr float kPadding = 8.0f;
    constexpr float kHeaderHeight = 34.0f;
    constexpr float kRowHeight = 30.0f;
    return ImVec2(190.0f,
                  kPadding + kHeaderHeight + kRowHeight * (static_cast<float>(rowIndex) + 0.5f));
  }

  SourceDiagnosticsPanel panel_;
  int lastVertexCount_ = 0;

private:
  ImGuiContext* context_ = nullptr;
};

TEST_F(SourceDiagnosticsPanelImGuiTest, EmptyDiagnosticsOrZeroHeightRenderNothing) {
  const std::array diagnostics = {
      SourceDiagnostic{.id = 1, .severity = DiagnosticSeverity::Error, .message = "boom"},
  };

  SourceDiagnosticsPanelAction action = Frame({}, std::nullopt, 120.0f);
  EXPECT_FALSE(action.hoveredId.has_value());
  EXPECT_FALSE(action.activatedId.has_value());
  const int emptyVertexCount = lastVertexCount_;

  action = Frame(diagnostics, std::nullopt, 0.0f);
  EXPECT_FALSE(action.hoveredId.has_value());
  EXPECT_FALSE(action.activatedId.has_value());
  EXPECT_EQ(lastVertexCount_, emptyVertexCount);

  // A populated panel with a positive height draws header and row content.
  Frame(diagnostics, std::nullopt, 120.0f);
  EXPECT_GT(lastVertexCount_, emptyVertexCount);
}

TEST_F(SourceDiagnosticsPanelImGuiTest, HoverReportsRowAndClickActivatesIt) {
  const std::array diagnostics = {
      SourceDiagnostic{
          .id = 21, .severity = DiagnosticSeverity::Error, .line = 3, .column = 4,
          .message = "unexpected token"},
      SourceDiagnostic{
          .id = 22, .severity = DiagnosticSeverity::Warning, .line = 9, .column = 0,
          .message = "unused attribute"},
  };
  constexpr float kHeight = 160.0f;

  // Warm-up frame: hover resolves against the previous frame's window layout.
  Frame(diagnostics, std::nullopt, kHeight, RowCenter(0));

  SourceDiagnosticsPanelAction action = Frame(diagnostics, std::nullopt, kHeight, RowCenter(0));
  EXPECT_EQ(action.hoveredId, std::optional<std::uint64_t>(21));
  EXPECT_FALSE(action.activatedId.has_value());

  // Pressing the left button on the hovered row activates it.
  action = Frame(diagnostics, std::nullopt, kHeight, RowCenter(0), /*mouseDown=*/true);
  EXPECT_EQ(action.activatedId, std::optional<std::uint64_t>(21));
  Frame(diagnostics, std::nullopt, kHeight, RowCenter(0));  // Release.

  // The second row reports its own id on hover.
  Frame(diagnostics, std::nullopt, kHeight, RowCenter(1));
  action = Frame(diagnostics, std::nullopt, kHeight, RowCenter(1));
  EXPECT_EQ(action.hoveredId, std::optional<std::uint64_t>(22));
  EXPECT_FALSE(action.activatedId.has_value());

  // Off-panel mouse reports nothing.
  Frame(diagnostics, std::nullopt, kHeight);
  action = Frame(diagnostics, std::nullopt, kHeight);
  EXPECT_FALSE(action.hoveredId.has_value());
  EXPECT_FALSE(action.activatedId.has_value());
}

TEST_F(SourceDiagnosticsPanelImGuiTest, SourceHoverHighlightsRowWithoutEmittingActions) {
  const std::array diagnostics = {
      SourceDiagnostic{.id = 31, .severity = DiagnosticSeverity::Error, .message = "first"},
      SourceDiagnostic{.id = 32, .severity = DiagnosticSeverity::Error, .message = "second"},
      SourceDiagnostic{.id = 33, .severity = DiagnosticSeverity::Warning, .message = "third"},
      SourceDiagnostic{.id = 34, .severity = DiagnosticSeverity::Warning, .message = "fourth"},
  };
  constexpr float kHeight = 200.0f;

  // Exercise the clamped font-scale path alongside the highlight comparison.
  ImGui::GetIO().FontGlobalScale = 0.5f;

  Frame(diagnostics, std::nullopt, kHeight);
  SourceDiagnosticsPanelAction action = Frame(diagnostics, std::nullopt, kHeight);
  EXPECT_FALSE(action.hoveredId.has_value());
  const int plainVertexCount = lastVertexCount_;

  // A source-side hover id draws the selection rect for that row (extra
  // vertices) but must not report hover or activation back to the caller.
  action = Frame(diagnostics, std::optional<std::uint64_t>(32), kHeight);
  EXPECT_FALSE(action.hoveredId.has_value());
  EXPECT_FALSE(action.activatedId.has_value());
  EXPECT_GT(lastVertexCount_, plainVertexCount);
}

}  // namespace
}  // namespace donner::editor
