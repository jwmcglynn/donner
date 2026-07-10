#pragma once
/// @file
///
/// `TextFormatBarPresenter` renders a contextual floating text-formatting bar
/// below the canvas tool palette. The bar is shown only while text styling is in
/// context: when the canvas selection is a single `<text>` element or an
/// in-canvas text editing session is active. It offers a searchable font-family
/// picker (each known family previewed in its own face, with a free-text
/// fallback for families the editor lacks), a font-size combo with drag and
/// preset behavior, and Bold/Italic/Underline toggles.
///
/// Following the `MenuBarPresenter` pattern, the presenter is a thin, testable
/// surface: `render()` draws the imgui controls and returns edge-triggered
/// `FormatBarActions`, which the shell routes to the *existing* styling
/// commands. B/I/U during an editing session go through the `TextTool` style
/// toggles; family, size, and the selection-only B/I/U path go through the
/// `TextInspectorPanel` attribute-write seam (`setAttributeOnSelection`). This
/// bar is a second surface over those commands, not a new styling pipeline.

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "donner/svg/SVGElement.h"
#include "donner/svg/resources/FontCatalogTypes.h"

struct ImFont;
struct ImVec2;

namespace donner::editor {

class EditorApp;

/// One selectable font family in the format bar's family picker.
struct FormatBarFontFamily {
  /// CSS family name written to the document (e.g. "Roboto", "sans-serif").
  std::string name;
  /// Face used to preview this family's menu entry, or null to fall back to the
  /// default UI font. Lets each family render in its own face when the editor
  /// has it loaded (the embedded Roboto and Fira Code faces today).
  ImFont* previewFont = nullptr;
  /// Origin of this family in the catalog. The picker lists the Embedded group
  /// before the System group and shows a header at each group boundary.
  svg::FontSource source = svg::FontSource::Embedded;
};

/// Build the picker's family list from a catalog listing (see
/// `FontCatalog::families()`, which already orders Embedded before System and
/// sorts within each group). Each entry keeps its CSS family name and source;
/// `previewFont` is filled from @p previewForFamily for families the editor has
/// a loaded face for, and left null otherwise (the entry then renders in the
/// default UI font). Families the catalog lacks are still reachable through the
/// bar's free-text box, so this list is additive, not a whitelist.
[[nodiscard]] std::vector<FormatBarFontFamily> BuildFormatBarFamilies(
    const std::vector<svg::FontFamilyInfo>& catalogFamilies,
    const std::function<ImFont*(const svg::FontFamilyInfo&)>& previewForFamily);

/// Snapshot of the current text formatting context, driving the bar's controls.
///
/// The shell fills `visible` and `families`; the attribute-derived fields
/// (`fontFamily`, `fontSize`, `bold`, `italic`, `underline`) come from
/// \ref ReadTextFormatState against the single selected/edited `<text>`.
struct FormatBarState {
  /// Whether the bar should render at all: true when text is selected or a text
  /// editing session is active.
  bool visible = false;
  /// Current `font-family` value shown in the picker. May name a family the
  /// editor lacks; preserved verbatim as free text.
  std::string fontFamily;
  /// Current `font-size` value (document units) shown in the size combo.
  float fontSize = 0.0f;
  /// True when a numeric `font-size` was resolved for the current element.
  bool hasFontSize = false;
  /// Current `font-weight: bold` state.
  bool bold = false;
  /// Current `font-style: italic` state.
  bool italic = false;
  /// Current `text-decoration: underline` state.
  bool underline = false;
  /// Face used to render the Bold ("B") toggle glyph, or null for the default.
  ImFont* boldToggleFont = nullptr;
  /// Families offered in the picker dropdown, each with its preview face.
  std::vector<FormatBarFontFamily> families;
};

/// Edge-triggered requests emitted by the format bar in one frame. Each `set*`
/// / `toggle*` flag is true only on the frame the user committed the control.
struct FormatBarActions {
  /// The user chose a family from the dropdown or committed the free-text box.
  bool setFontFamily = false;
  std::string fontFamily;
  /// The user committed a new font size (drag release, typed entry, or preset).
  bool setFontSize = false;
  float fontSize = 0.0f;
  bool toggleBold = false;
  bool toggleItalic = false;
  bool toggleUnderline = false;
};

/// Common font-size presets offered by the size combo (document units).
inline constexpr std::array<int, 12> kFormatBarFontSizePresets = {8,  9,  10, 11, 12, 14,
                                                                  16, 18, 24, 36, 48, 72};

/// Whether the format bar should be shown, given the styling context.
///
/// The bar is contextual: it appears while the selection is a single `<text>`
/// element, or while an in-canvas text editing session is active (which also
/// makes that element the selection, but the session predicate is kept
/// explicit so the bar stays up through selection churn during editing).
///
/// @param hasSingleTextSelection True when exactly one `<text>` is selected.
/// @param textEditingActive True when an in-canvas text session owns the caret.
[[nodiscard]] bool FormatBarShouldShow(bool hasSingleTextSelection, bool textEditingActive);

/// Populate the attribute-derived fields (`fontFamily`, `fontSize`,
/// `hasFontSize`, `bold`, `italic`, `underline`) of @p state from a single
/// selected/edited `<text>` element. Reads under a scoped read access so it is
/// safe while the live editor keeps the document in ConcurrentDom. Leaves
/// `visible`, `families`, and `boldToggleFont` untouched (owned by the shell).
void ReadTextFormatState(const svg::SVGElement& text, FormatBarState* state);

/// Route the attribute-write half of the bar's actions onto the app's current
/// selection (the `TextInspectorPanel` seam): font family, font size, and -
/// when @p routeTogglesToSelection is true - the B/I/U toggles. During an
/// editing session the shell handles B/I/U through `TextTool` instead and
/// passes `routeTogglesToSelection=false`, so this only writes family/size.
///
/// Toggle-off writes the "normal"/"none" value (rather than removing the
/// attribute) to stay on the pure attribute-write path; the value that flips is
/// derived from @p state's current B/I/U flags.
///
/// @return true if any document mutation was queued.
bool ApplyFormatBarActionsToSelection(const FormatBarActions& actions, const FormatBarState& state,
                                      bool routeTogglesToSelection, EditorApp& app);

/// Contextual text-formatting bar shown as a floating canvas toolbar.
class TextFormatBarPresenter {
public:
  /// Preferred width of the compact single-row toolbar.
  [[nodiscard]] static constexpr float PreferredWidth() { return 460.0f; }
  /// Toolbar height for the current ImGui frame metrics.
  [[nodiscard]] static float BarHeight();

  /**
   * Render the bar and report the actions requested this frame.
   *
   * Draws nothing and returns an empty result when `state.visible` is false.
   * The bar is a floating rounded container at @p topLeft with a stable width.
   *
   * @param state Current formatting context (visibility, values, families).
   * @param topLeft Top-left edge in screen coordinates.
   * @param width Bar width in pixels, normally `PreferredWidth()`.
   * @return Edge-triggered actions to route to the styling commands.
   */
  [[nodiscard]] FormatBarActions render(const FormatBarState& state, const ImVec2& topLeft,
                                        float width);

private:
  /// Free-text font-family buffer, re-seeded when the underlying value changes.
  std::array<char, 256> fontFamilyBuffer_{};
  /// Family the buffer was last seeded from, to detect external changes.
  std::string lastSyncedFamily_;
  bool trackedFamily_ = false;
  /// Filter text for the searchable family dropdown.
  std::array<char, 64> familySearchBuffer_{};
  /// Working value for the size drag; only re-seeded from state while the drag
  /// control is idle, so an in-progress drag is not clobbered each frame.
  float sizeEditValue_ = 0.0f;
  bool sizeControlActive_ = false;
};

}  // namespace donner::editor
