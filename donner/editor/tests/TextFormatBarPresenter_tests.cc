#include "donner/editor/TextFormatBarPresenter.h"

#include <gtest/gtest.h>

#include <cstddef>
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

TEST(TextFormatBarPresenterActionsTest, ReadsUnparseableFontSizeAsAbsent) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t" font-size="large">Hi</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());

  FormatBarState state;
  ReadTextFormatState(*text, &state);

  // "large" has no leading number, so the size control shows no resolved size.
  EXPECT_FALSE(state.hasFontSize);
  EXPECT_FLOAT_EQ(state.fontSize, 0.0f);
}

TEST(TextFormatBarPresenterActionsTest, ReadsNonMatchingStyleValuesAsUnset) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t" font-weight="500" font-style="oblique"
                 text-decoration="line-through">Hi</text>
         </svg>)"));
  const std::optional<svg::SVGElement> text = app.document().document().querySelector("#t");
  ASSERT_TRUE(text.has_value());

  FormatBarState state;
  ReadTextFormatState(*text, &state);

  // Values other than bold/italic/underline do not light up the toggles.
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

TEST(TextFormatBarPresenterActionsTest, ActionsWithEmptySelectionQueueNothing) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
           <text id="t">Hi</text>
         </svg>)"));
  // No selection: every attribute write is refused, so nothing is queued even
  // with all actions raised.
  FormatBarActions actions;
  actions.setFontFamily = true;
  actions.fontFamily = "Roboto";
  actions.setFontSize = true;
  actions.fontSize = 18.0f;
  actions.toggleBold = true;
  actions.toggleItalic = true;
  actions.toggleUnderline = true;

  EXPECT_FALSE(ApplyFormatBarActionsToSelection(actions, FormatBarState{},
                                                /*routeTogglesToSelection=*/true, app));
  EXPECT_FALSE(app.flushFrame());
  EXPECT_EQ(TextAttribute(app, "font-family"), std::nullopt);
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
// Family list construction (catalog wiring)
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
    const FormatBarActions actions =
        bar.render(state, ImVec2(320.0f, 80.0f), TextFormatBarPresenter::PreferredWidth());
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

// ---------------------------------------------------------------------------
// Interactive control behavior (headless ImGui with simulated mouse/keyboard)
// ---------------------------------------------------------------------------

/// Accumulate every edge-triggered flag from @p frame into @p merged.
void MergeActions(FormatBarActions& merged, const FormatBarActions& frame) {
  if (frame.setFontFamily) {
    merged.setFontFamily = true;
    merged.fontFamily = frame.fontFamily;
  }
  if (frame.setFontSize) {
    merged.setFontSize = true;
    merged.fontSize = frame.fontSize;
  }
  merged.toggleBold = merged.toggleBold || frame.toggleBold;
  merged.toggleItalic = merged.toggleItalic || frame.toggleItalic;
  merged.toggleUnderline = merged.toggleUnderline || frame.toggleUnderline;
}

class TextFormatBarPresenterInputTest : public ::testing::Test {
protected:
  // The bar is placed at a fixed spot with its 8px window padding, so control
  // positions are derived from ImGui frame metrics captured during rendering.
  static constexpr float kBarX = 320.0f;
  static constexpr float kBarY = 80.0f;
  static constexpr float kPadding = 8.0f;
  static constexpr float kFamilyInputWidth = 180.0f;
  static constexpr float kSizeDragWidth = 64.0f;

  void SetUp() override {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigMacOSXBehaviors = false;
    io.Fonts->Build();
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    context_ = nullptr;
  }

  /// Fully-visible bar state with an Embedded and a System family, so the
  /// picker shows both group headers and a previewed face.
  FormatBarState MakeState() {
    FormatBarState state;
    state.visible = true;
    state.fontFamily = "Roboto";
    state.fontSize = 16.0f;
    state.hasFontSize = true;
    ImFont* face = ImGui::GetIO().Fonts->Fonts[0];
    state.boldToggleFont = face;
    // Two Embedded families followed by one System family: the picker prints
    // one header per group and no header between same-group rows.
    state.families = {
        FormatBarFontFamily{"Roboto", face, svg::FontSource::Embedded},
        FormatBarFontFamily{"Fira Code", face, svg::FontSource::Embedded},
        FormatBarFontFamily{"Zilla Slab", nullptr, svg::FontSource::System},
    };
    return state;
  }

  FormatBarActions Frame(const FormatBarState& state,
                         const ImVec2& mouse = ImVec2(-100.0f, -100.0f), bool mouseDown = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, mouseDown);
    ImGui::NewFrame();
    frameHeight_ = ImGui::GetFrameHeight();
    itemSpacingX_ = ImGui::GetStyle().ItemSpacing.x;
    const FormatBarActions actions =
        bar_.render(state, ImVec2(kBarX, kBarY), TextFormatBarPresenter::PreferredWidth());
    ImGui::Render();
    return actions;
  }

  /// Simulate a full left click (press + release) and merge both frames.
  FormatBarActions Click(const FormatBarState& state, const ImVec2& mouse) {
    FormatBarActions merged = Frame(state, mouse, /*mouseDown=*/true);
    MergeActions(merged, Frame(state, mouse, /*mouseDown=*/false));
    return merged;
  }

  // Horizontal layout of the single control row (left to right): family input,
  // family combo arrow, size drag, size combo arrow, B, I, U.
  float RowCenterY() const { return kBarY + kPadding + frameHeight_ * 0.5f; }
  float RowBottomY() const { return kBarY + kPadding + frameHeight_; }
  float FamilyInputLeft() const { return kBarX + kPadding; }
  float FamilyArrowLeft() const { return FamilyInputLeft() + kFamilyInputWidth; }
  float SizeDragLeft() const { return FamilyArrowLeft() + frameHeight_ + itemSpacingX_; }
  float SizeArrowLeft() const { return SizeDragLeft() + kSizeDragWidth; }
  float BoldLeft() const { return SizeArrowLeft() + frameHeight_ + itemSpacingX_; }
  float ItalicLeft() const { return BoldLeft() + frameHeight_ + itemSpacingX_; }
  float UnderlineLeft() const { return ItalicLeft() + frameHeight_ + itemSpacingX_; }

  ImVec2 ToggleCenter(float left) const {
    return ImVec2(left + frameHeight_ * 0.5f, RowCenterY());
  }

  /// Open the combo popup whose arrow-only control is centered at
  /// @p arrowCenter. New ImGui windows are hidden their first frame while
  /// their size is measured, so run one idle frame before interacting with
  /// the popup contents.
  void OpenComboPopup(const FormatBarState& state, const ImVec2& arrowCenter) {
    Click(state, arrowCenter);
    Frame(state);
  }

  /// Click down a combo popup (opened at @p popupLeft, below the control row)
  /// until a row emits an action, returning the merged actions of that click.
  FormatBarActions ClickFirstActionRowInPopup(const FormatBarState& state, float popupLeft,
                                              bool (*hasAction)(const FormatBarActions&)) {
    // 12px in from the popup's left edge lands inside its rows even for the
    // narrow single-digit size presets.
    for (float y = RowBottomY() + 4.0f; y <= RowBottomY() + 240.0f; y += 5.0f) {
      const FormatBarActions actions = Click(state, ImVec2(popupLeft + 12.0f, y));
      if (hasAction(actions)) {
        return actions;
      }
    }
    return FormatBarActions{};
  }

  TextFormatBarPresenter bar_;
  float frameHeight_ = 0.0f;
  float itemSpacingX_ = 8.0f;

private:
  ImGuiContext* context_ = nullptr;
};

TEST_F(TextFormatBarPresenterInputTest, ClickingToggleButtonsEmitsBoldItalicUnderline) {
  const FormatBarState state = [&] {
    FormatBarState s = MakeState();
    s.bold = true;  // Exercises the highlighted-toggle styling for "B".
    return s;
  }();

  Frame(state);  // Warm-up frame: establishes layout metrics and hover state.

  FormatBarActions actions = Click(state, ToggleCenter(BoldLeft()));
  EXPECT_TRUE(actions.toggleBold);
  EXPECT_FALSE(actions.toggleItalic);
  EXPECT_FALSE(actions.toggleUnderline);
  EXPECT_FALSE(actions.setFontFamily);
  EXPECT_FALSE(actions.setFontSize);

  actions = Click(state, ToggleCenter(ItalicLeft()));
  EXPECT_TRUE(actions.toggleItalic);
  EXPECT_FALSE(actions.toggleBold);

  actions = Click(state, ToggleCenter(UnderlineLeft()));
  EXPECT_TRUE(actions.toggleUnderline);
  EXPECT_FALSE(actions.toggleBold);
}

TEST_F(TextFormatBarPresenterInputTest, DraggingSizeControlCommitsNewFontSize) {
  const FormatBarState state = MakeState();  // fontSize == 16.

  Frame(state);
  const ImVec2 dragCenter(SizeDragLeft() + kSizeDragWidth * 0.5f, RowCenterY());

  // Press on the drag control and pull it 30px to the right before releasing;
  // the presenter commits the edited value on the release frame.
  Frame(state, dragCenter);
  FormatBarActions merged = Frame(state, dragCenter, /*mouseDown=*/true);
  for (float dx = 10.0f; dx <= 30.0f; dx += 10.0f) {
    MergeActions(merged, Frame(state, ImVec2(dragCenter.x + dx, dragCenter.y),
                               /*mouseDown=*/true));
  }
  MergeActions(merged, Frame(state, ImVec2(dragCenter.x + 30.0f, dragCenter.y),
                             /*mouseDown=*/false));

  EXPECT_TRUE(merged.setFontSize);
  EXPECT_GT(merged.fontSize, 16.0f);
  EXPECT_FALSE(merged.setFontFamily);
}

TEST_F(TextFormatBarPresenterInputTest, SizePresetMenuSelectsPreset) {
  FormatBarState state = MakeState();
  state.fontSize = 8.0f;  // Matches the first preset so its row shows selected.

  Frame(state);
  // Open the size preset combo (the arrow-only control right of the drag box).
  OpenComboPopup(state, ImVec2(SizeArrowLeft() + frameHeight_ * 0.5f, RowCenterY()));

  // One frame with no resolved size: the open popup renders every preset row
  // unselected without emitting actions.
  FormatBarState noSizeState = state;
  noSizeState.hasFontSize = false;
  EXPECT_FALSE(Frame(noSizeState).setFontSize);

  const FormatBarActions actions = ClickFirstActionRowInPopup(
      state, SizeArrowLeft(), [](const FormatBarActions& a) { return a.setFontSize; });

  ASSERT_TRUE(actions.setFontSize) << "No preset row found in the size popup";
  EXPECT_FLOAT_EQ(actions.fontSize, static_cast<float>(kFormatBarFontSizePresets.front()));
}

TEST_F(TextFormatBarPresenterInputTest, FamilyMenuListsGroupsAndSelectsFamily) {
  const FormatBarState state = MakeState();

  Frame(state);
  // Open the family picker dropdown (arrow right of the free-text box).
  OpenComboPopup(state, ImVec2(FamilyArrowLeft() + frameHeight_ * 0.5f, RowCenterY()));

  // Scan down the popup: the search box and the "Embedded" header emit no
  // actions; the first selectable row is the first configured family.
  const FormatBarActions actions = ClickFirstActionRowInPopup(
      state, FamilyArrowLeft(), [](const FormatBarActions& a) { return a.setFontFamily; });

  ASSERT_TRUE(actions.setFontFamily) << "No family row found in the picker popup";
  EXPECT_EQ(actions.fontFamily, "Roboto");
}

TEST_F(TextFormatBarPresenterInputTest, FamilySearchFiltersRowsCaseInsensitively) {
  const FormatBarState state = MakeState();

  Frame(state);
  OpenComboPopup(state, ImVec2(FamilyArrowLeft() + frameHeight_ * 0.5f, RowCenterY()));

  // Focus the search box at the top of the popup and type a lowercase filter
  // that only matches "Zilla Slab".
  const ImVec2 searchCenter(FamilyArrowLeft() + kPadding + 100.0f,
                            RowBottomY() + kPadding + frameHeight_ * 0.5f);
  Click(state, searchCenter);
  ImGuiIO& io = ImGui::GetIO();
  io.AddInputCharacter('z');
  io.AddInputCharacter('i');
  io.AddInputCharacter('l');
  Frame(state, searchCenter);

  // With the filter applied, the first (and only) selectable row is the
  // System-group family, proving the case-insensitive match dropped "Roboto".
  const FormatBarActions actions = ClickFirstActionRowInPopup(
      state, FamilyArrowLeft(), [](const FormatBarActions& a) { return a.setFontFamily; });

  ASSERT_TRUE(actions.setFontFamily) << "No family row matched the search filter";
  EXPECT_EQ(actions.fontFamily, "Zilla Slab");
}

TEST_F(TextFormatBarPresenterInputTest, FreeTextFamilyCommitEmitsSetFontFamily) {
  FormatBarState state = MakeState();
  state.fontFamily.clear();  // Start from an empty free-text buffer.

  Frame(state);
  // Activate the free-text box, type a family name, then click away to commit.
  Click(state, ImVec2(FamilyInputLeft() + kFamilyInputWidth * 0.5f, RowCenterY()));
  ImGuiIO& io = ImGui::GetIO();
  io.AddInputCharacter('F');
  io.AddInputCharacter('i');
  io.AddInputCharacter('r');
  io.AddInputCharacter('a');
  Frame(state, ImVec2(FamilyInputLeft() + kFamilyInputWidth * 0.5f, RowCenterY()));

  const FormatBarActions actions = Click(state, ImVec2(900.0f, 400.0f));
  EXPECT_TRUE(actions.setFontFamily);
  EXPECT_EQ(actions.fontFamily, "Fira");
}

TEST_F(TextFormatBarPresenterInputTest, ExternalFamilyChangeReseedsFreeTextBuffer) {
  FormatBarState state = MakeState();
  state.fontFamily = "Alpha";
  Frame(state);  // Seeds the buffer from "Alpha".

  state.fontFamily = "Beta";
  Frame(state);  // External change: the buffer re-seeds to "Beta".

  // Edit and commit: the committed value derives from "Beta", not "Alpha".
  Click(state, ImVec2(FamilyInputLeft() + kFamilyInputWidth * 0.5f, RowCenterY()));
  ImGui::GetIO().AddInputCharacter('X');
  Frame(state, ImVec2(FamilyInputLeft() + kFamilyInputWidth * 0.5f, RowCenterY()));

  const FormatBarActions actions = Click(state, ImVec2(900.0f, 400.0f));
  ASSERT_TRUE(actions.setFontFamily);
  std::string committed = actions.fontFamily;
  const std::size_t inserted = committed.find('X');
  ASSERT_NE(inserted, std::string::npos) << "Committed value: " << committed;
  committed.erase(inserted, 1);
  EXPECT_EQ(committed, "Beta");
}

}  // namespace
}  // namespace donner::editor
