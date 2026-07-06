#include "donner/editor/TextFormatBarPresenter.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "donner/editor/EditorApp.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::editor {
namespace {

/// Attribute value of the lone `<text id="t">` after a flush, or nullopt.
std::optional<std::string> TextAttribute(EditorApp& app, std::string_view name) {
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  if (!text.has_value()) {
    return std::nullopt;
  }
  const std::optional<RcString> value = text->getAttribute(name);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::string(std::string_view(*value));
}

// ---------------------------------------------------------------------------
// Visibility rule
// ---------------------------------------------------------------------------

TEST(TextFormatBarPresenterActionsTest, VisibleWhileTextSelectedOrEditing) {
  EXPECT_FALSE(FormatBarShouldShow(/*hasSingleTextSelection=*/false, /*textEditingActive=*/false));
  EXPECT_TRUE(FormatBarShouldShow(/*hasSingleTextSelection=*/true, /*textEditingActive=*/false));
  EXPECT_TRUE(FormatBarShouldShow(/*hasSingleTextSelection=*/false, /*textEditingActive=*/true));
  EXPECT_TRUE(FormatBarShouldShow(/*hasSingleTextSelection=*/true, /*textEditingActive=*/true));
}

// ---------------------------------------------------------------------------
// State reading
// ---------------------------------------------------------------------------

TEST(TextFormatBarPresenterActionsTest, ReadsFamilySizeAndStyleFlags) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t" font-family="Fira Code" font-size="24" font-weight="bold"
                 font-style="italic" text-decoration="underline">Hi</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());

  FormatBarState state;
  ReadTextFormatState(*text, &state);

  EXPECT_EQ(state.fontFamily, "Fira Code");
  EXPECT_TRUE(state.hasFontSize);
  EXPECT_FLOAT_EQ(state.fontSize, 24.0f);
  EXPECT_TRUE(state.bold);
  EXPECT_TRUE(state.italic);
  EXPECT_TRUE(state.underline);
}

TEST(TextFormatBarPresenterActionsTest, ReadsDefaultsWhenStyleAttributesAbsent) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t">Plain</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());

  FormatBarState state;
  ReadTextFormatState(*text, &state);

  EXPECT_TRUE(state.fontFamily.empty());
  EXPECT_FALSE(state.hasFontSize);
  EXPECT_FALSE(state.bold);
  EXPECT_FALSE(state.italic);
  EXPECT_FALSE(state.underline);
}

TEST(TextFormatBarPresenterActionsTest, ReadTextFormatStateIgnoresNullState) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t" font-size="16">Hi</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  // Must not dereference a null out-parameter.
  ReadTextFormatState(*text, nullptr);
}

// ---------------------------------------------------------------------------
// Command routing: bar actions reach the document
// ---------------------------------------------------------------------------

TEST(TextFormatBarPresenterActionsTest, ToggleBoldWritesFontWeightBoldToDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t">Hi</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  FormatBarState state;  // Currently not bold.
  FormatBarActions actions;
  actions.toggleBold = true;

  EXPECT_TRUE(ApplyFormatBarActionsToSelection(actions, state,
                                               /*routeTogglesToSelection=*/true, app));
  EXPECT_TRUE(app.flushFrame());
  EXPECT_EQ(TextAttribute(app, "font-weight"), "bold");
}

TEST(TextFormatBarPresenterActionsTest, ToggleBoldOffWritesFontWeightNormal) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t" font-weight="bold">Hi</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  FormatBarState state;
  state.bold = true;  // Currently bold, so the toggle turns it off.
  FormatBarActions actions;
  actions.toggleBold = true;

  EXPECT_TRUE(ApplyFormatBarActionsToSelection(actions, state,
                                               /*routeTogglesToSelection=*/true, app));
  EXPECT_TRUE(app.flushFrame());
  EXPECT_EQ(TextAttribute(app, "font-weight"), "normal");
}

TEST(TextFormatBarPresenterActionsTest, ItalicAndUnderlineTogglesWriteToDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t">Hi</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  FormatBarState state;
  FormatBarActions actions;
  actions.toggleItalic = true;
  actions.toggleUnderline = true;

  EXPECT_TRUE(ApplyFormatBarActionsToSelection(actions, state,
                                               /*routeTogglesToSelection=*/true, app));
  EXPECT_TRUE(app.flushFrame());
  EXPECT_EQ(TextAttribute(app, "font-style"), "italic");
  EXPECT_EQ(TextAttribute(app, "text-decoration"), "underline");
}

TEST(TextFormatBarPresenterActionsTest, FontFamilyAndSizeWritesReachDocument) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t" font-family="serif" font-size="12">Hi</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  FormatBarState state;
  FormatBarActions actions;
  actions.setFontFamily = true;
  actions.fontFamily = "Roboto";
  actions.setFontSize = true;
  actions.fontSize = 18.0f;

  EXPECT_TRUE(ApplyFormatBarActionsToSelection(actions, state,
                                               /*routeTogglesToSelection=*/false, app));
  EXPECT_TRUE(app.flushFrame());
  EXPECT_EQ(TextAttribute(app, "font-family"), "Roboto");
  EXPECT_EQ(TextAttribute(app, "font-size"), "18");
}

TEST(TextFormatBarPresenterActionsTest, TogglesSkippedWhenRoutedToEditingSession) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t">Hi</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  FormatBarState state;
  FormatBarActions actions;
  actions.toggleBold = true;

  // With routeTogglesToSelection=false the shell owns B/I/U via TextTool, so the
  // attribute-write path must not queue a font-weight write.
  EXPECT_FALSE(ApplyFormatBarActionsToSelection(actions, state,
                                                /*routeTogglesToSelection=*/false, app));
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(TextAttribute(app, "font-weight"), std::nullopt);
}

TEST(TextFormatBarPresenterActionsTest, NoActionsQueueNoMutation) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t">Hi</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);

  EXPECT_FALSE(ApplyFormatBarActionsToSelection(FormatBarActions{}, FormatBarState{},
                                                /*routeTogglesToSelection=*/true, app));
  EXPECT_FALSE(app.flushFrame());
}

// ---------------------------------------------------------------------------
// Family list construction (W3 catalog wiring)
// ---------------------------------------------------------------------------

TEST(TextFormatBarPresenterActionsTest, BuildFormatBarFamiliesGroupsEmbeddedThenSystem) {
  // Distinct sentinel pointers stand in for loaded ImGui faces; the builder only
  // stores them, never dereferences them.
  ImFont* const robotoFace = reinterpret_cast<ImFont*>(0x10);
  ImFont* const codeFace = reinterpret_cast<ImFont*>(0x20);

  // Mirror FontCatalog::families(): the Embedded group first, then System, each
  // already sorted within its group.
  const std::vector<svg::FontFamilyInfo> catalog = {
      {"Fira Code", svg::FontSource::Embedded, svg::FontCategory::Monospace},
      {"Roboto", svg::FontSource::Embedded, svg::FontCategory::SansSerif},
      {"Helvetica", svg::FontSource::System, svg::FontCategory::SansSerif},
  };

  const std::vector<FormatBarFontFamily> built =
      BuildFormatBarFamilies(catalog, [&](const svg::FontFamilyInfo& info) -> ImFont* {
        if (info.family == "Roboto") {
          return robotoFace;
        }
        if (info.family == "Fira Code") {
          return codeFace;
        }
        return nullptr;
      });

  ASSERT_EQ(built.size(), 3u);
  // Grouping/order is preserved so the picker's Embedded/System headers land on
  // the right rows.
  EXPECT_EQ(built[0].name, "Fira Code");
  EXPECT_EQ(built[0].source, svg::FontSource::Embedded);
  EXPECT_EQ(built[0].previewFont, codeFace);
  EXPECT_EQ(built[1].name, "Roboto");
  EXPECT_EQ(built[1].source, svg::FontSource::Embedded);
  EXPECT_EQ(built[1].previewFont, robotoFace);
  EXPECT_EQ(built[2].name, "Helvetica");
  EXPECT_EQ(built[2].source, svg::FontSource::System);
  // A System family with no loaded face previews in the default UI font (null).
  EXPECT_EQ(built[2].previewFont, nullptr);
}

TEST(TextFormatBarPresenterActionsTest, BuildFormatBarFamiliesToleratesNullPreviewResolver) {
  const std::vector<svg::FontFamilyInfo> catalog = {
      {"Roboto", svg::FontSource::Embedded, svg::FontCategory::SansSerif},
  };
  const std::vector<FormatBarFontFamily> built = BuildFormatBarFamilies(catalog, {});
  ASSERT_EQ(built.size(), 1u);
  EXPECT_EQ(built[0].name, "Roboto");
  EXPECT_EQ(built[0].previewFont, nullptr);
}

// ---------------------------------------------------------------------------
// Render seam (visibility gating through the imgui surface)
// ---------------------------------------------------------------------------

class TextFormatBarPresenterTest : public ::testing::Test {
protected:
  void SetUp() override {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->Build();
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    context_ = nullptr;
  }

  static FormatBarActions RenderFrame(TextFormatBarPresenter& bar, const FormatBarState& state) {
    ImGui::NewFrame();
    const FormatBarActions actions = bar.render(state, /*originY=*/20.0f, /*width=*/1280.0f);
    ImGui::Render();
    return actions;
  }

private:
  ImGuiContext* context_ = nullptr;
};

TEST_F(TextFormatBarPresenterTest, HiddenStateRendersNothingAndReturnsNoActions) {
  TextFormatBarPresenter bar;
  FormatBarState state;  // visible == false.

  const FormatBarActions actions = RenderFrame(bar, state);

  EXPECT_FALSE(actions.setFontFamily);
  EXPECT_FALSE(actions.setFontSize);
  EXPECT_FALSE(actions.toggleBold);
  EXPECT_FALSE(actions.toggleItalic);
  EXPECT_FALSE(actions.toggleUnderline);
}

TEST_F(TextFormatBarPresenterTest, VisibleBarRendersControlsWithoutImplicitActions) {
  TextFormatBarPresenter bar;
  ImFont* face = ImGui::GetIO().Fonts->Fonts[0];

  FormatBarState state;
  state.visible = true;
  state.fontFamily = "Roboto";
  state.fontSize = 16.0f;
  state.hasFontSize = true;
  state.bold = true;
  state.boldToggleFont = face;
  state.families = {FormatBarFontFamily{"Roboto", face}, FormatBarFontFamily{"serif", nullptr}};

  // Two frames: the first seeds internal buffers, the second exercises the
  // steady state. Neither involves user input, so no actions should fire.
  EXPECT_FALSE(RenderFrame(bar, state).toggleBold);
  const FormatBarActions actions = RenderFrame(bar, state);
  EXPECT_FALSE(actions.setFontFamily);
  EXPECT_FALSE(actions.setFontSize);
  EXPECT_FALSE(actions.toggleBold);
  EXPECT_FALSE(actions.toggleItalic);
  EXPECT_FALSE(actions.toggleUnderline);
}

}  // namespace
}  // namespace donner::editor
