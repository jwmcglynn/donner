#include "donner/editor/TextFormatBarPresenter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/Box.h"
#include "donner/base/FormatNumber.h"
#include "donner/base/Length.h"
#include "donner/base/RcString.h"
#include "donner/base/RelativeLengthMetrics.h"
#include "donner/base/SmallVector.h"
#include "donner/base/StringUtils.h"
#include "donner/editor/AttachedNumericStepper.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/SVGElement.h"
#include "donner/svg/core/FontStyle.h"
#include "donner/svg/core/TextDecoration.h"
#include "donner/svg/properties/PropertyRegistry.h"

namespace donner::editor {

namespace internal {

/// CSS name-start code points, including non-ASCII UTF-8 bytes.
bool IsFamilyIdentifierStart(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

/// Code points allowed after the first character of an unescaped CSS identifier.
bool IsFamilyIdentifierContinuation(unsigned char c) {
  return IsFamilyIdentifierStart(c) || (c >= '0' && c <= '9') || c == '-';
}

/// CSS-wide and reserved keywords cannot be unquoted family words.
bool IsReservedFamilyWord(std::string_view word) {
  for (const std::string_view reserved :
       {"inherit", "initial", "unset", "revert", "revert-layer", "default"}) {
    if (StringUtils::EqualsLowercase(word, reserved)) {
      return true;
    }
  }
  return false;
}

/// A leading hyphen requires another hyphen or a name-start code point.
bool HasFamilyIdentifierStart(std::string_view word) {
  if (word.empty()) {
    return false;
  }
  if (word.front() == '-') {
    return word.size() > 1 && (word[1] == '-' || IsFamilyIdentifierStart(word[1]));
  }
  return IsFamilyIdentifierStart(word.front());
}

/// Whether a decoded family word can round-trip as an unescaped CSS identifier.
bool IsEditableFamilyIdentifier(std::string_view word) {
  if (!HasFamilyIdentifierStart(word) || IsReservedFamilyWord(word)) {
    return false;
  }
  for (const unsigned char c : word) {
    if (!IsFamilyIdentifierContinuation(c)) {
      return false;
    }
  }
  return true;
}

/// Serialize a family as a CSS string, escaping quotes, backslashes and control bytes.
std::string QuotedFontFamily(std::string_view family) {
  std::string result = "\"";
  constexpr std::string_view kHex = "0123456789abcdef";
  for (const unsigned char c : family) {
    if (c < 0x20 || c == 0x7f) {
      result += '\\';
      result += kHex[c >> 4];
      result += kHex[c & 0xf];
      result += ' ';
    } else {
      if (c == '"' || c == '\\') {
        result += '\\';
      }
      result += static_cast<char>(c);
    }
  }
  result += '"';
  return result;
}

/// Preserve simple family display names and quote names that require CSS string syntax.
std::string EditableFontFamily(std::string_view family) {
  std::size_t offset = 0;
  do {
    const std::size_t space = family.find(' ', offset);
    const std::size_t end = space == std::string_view::npos ? family.size() : space;
    if (!IsEditableFamilyIdentifier(family.substr(offset, end - offset))) {
      return QuotedFontFamily(family);
    }
    if (space == std::string_view::npos) {
      break;
    }
    offset = space + 1;
  } while (offset <= family.size());
  return std::string(family);
}

/// Copies `value` into a fixed-size, null-terminated buffer.
template <std::size_t N>
void AssignBuffer(std::array<char, N>& buffer, std::string_view value) {
  const std::size_t count = std::min(value.size(), N - 1u);
  std::memcpy(buffer.data(), value.data(), count);
  buffer[count] = '\0';
}

/// Case-insensitive substring match, for the family-picker search filter. An
/// empty needle matches everything.
bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                        [&](char a, char b) { return lower(a) == lower(b); });
  return it != haystack.end();
}

/// Draws the loaded preview or its pending/failure label inside the current font row.
void DrawFontFamilyPreview(const FormatBarFontFamily& family, const ImVec2& rowMin,
                           const ImVec2& rowMax, bool rowVisible, FormatBarActions& actions) {
  if (family.preview.available()) {
    const float renderedWidth = std::min(family.preview.width, rowMax.x - rowMin.x);
    const float renderedHeight = std::min(family.preview.height, rowMax.y - rowMin.y);
    const ImVec2 previewMin(rowMin.x, rowMin.y + (rowMax.y - rowMin.y - renderedHeight) * 0.5f);
    const ImVec2 previewMax(previewMin.x + renderedWidth, previewMin.y + renderedHeight);
    ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(family.preview.texture),
                                         previewMin, previewMax, ImVec2(0.0f, 0.0f),
                                         ImVec2(family.preview.uvMaxX, family.preview.uvMaxY),
                                         ImGui::GetColorU32(ImGuiCol_Text));
  } else {
    const std::string label =
        family.name + (family.previewFailed || family.availability == svg::FontAssetState::Failed
                           ? " (unavailable)"
                           : " (loading...)");
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(rowMin.x, rowMin.y + (rowMax.y - rowMin.y - ImGui::GetTextLineHeight()) * 0.5f),
        ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
    if (rowVisible && family.availability != svg::FontAssetState::Failed && !family.previewFailed) {
      actions.requestFontPreviews.push_back(family.name);
    }
  }
}

/// Shows status and explicit retry actions after drawing the font row.
void RenderFontFamilyFeedback(const FormatBarFontFamily& family, bool rowVisible,
                              FormatBarActions& actions) {
  if (rowVisible && ImGui::IsItemHovered() && !family.preview.available()) {
    ImGui::SetTooltip(family.previewFailed ? "Font preview could not be decoded."
                      : family.availability == svg::FontAssetState::Failed
                          ? "Font unavailable. Use Retry font to try again."
                          : "Loading font preview...");
  }
  if (family.availability == svg::FontAssetState::Failed) {
    ImGui::TextDisabled("Font unavailable");
    ImGui::SameLine();
    if (ImGui::SmallButton("Retry font")) {
      actions.retryFontFamilies.push_back(family.name);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Retries are limited to once every 30 seconds.");
    }
  }
}

/// Renders one selectable family and reports its visibility and font-loading actions.
bool RenderFontFamilyRow(const FormatBarFontFamily& family, bool selected,
                         FormatBarActions& actions) {
  ImGui::PushID(family.name.c_str());
  const float rowHeight = std::max(ImGui::GetTextLineHeight(), family.preview.height);
  const bool chosen = ImGui::Selectable("##font_family", selected, ImGuiSelectableFlags_None,
                                        ImVec2(0.0f, rowHeight));
  const ImVec2 rowMin = ImGui::GetItemRectMin();
  const ImVec2 rowMax = ImGui::GetItemRectMax();
  const bool rowVisible = ImGui::IsItemVisible();
  if (rowVisible) {
    actions.visibleFontFamilies.push_back(family.name);
  }
  DrawFontFamilyPreview(family, rowMin, rowMax, rowVisible, actions);
  RenderFontFamilyFeedback(family, rowVisible, actions);
  ImGui::PopID();
  return chosen;
}

}  // namespace internal

using internal::AssignBuffer;
using internal::ContainsCaseInsensitive;

bool FormatBarShouldShow(bool hasSingleTextSelection, bool textEditingActive) {
  return hasSingleTextSelection || textEditingActive;
}

void ReadTextFormatState(const svg::SVGElement& text, FormatBarState* state) {
  if (state == nullptr) {
    return;
  }
  // Hold write access while reading: the returned style lives in the document
  // registry, which a concurrent restyle may otherwise replace mid-read.
  text.withWriteAccess([&](svg::DocumentWriteAccess&, EntityHandle) {
    ReadTextFormatState(text.getComputedStyle(), state);
  });
}

void ReadTextFormatState(const svg::PropertyRegistry& style, FormatBarState* state) {
  if (state == nullptr) {
    return;
  }

  // `get()` returns the value by copy; keep it alive for the loop.
  const SmallVector<RcString, 1> families = style.fontFamily.get().value();
  state->fontFamily.clear();
  for (const RcString& family : families) {
    if (!state->fontFamily.empty()) {
      state->fontFamily += ", ";
    }
    state->fontFamily += internal::EditableFontFamily(std::string_view(family));
  }

  const double fontSize =
      style.fontSize.get().value().toPixels(Box2d(), FontMetrics(), Lengthd::Extent::Mixed);
  state->hasFontSize = std::isfinite(fontSize);
  state->fontSize = state->hasFontSize ? static_cast<float>(fontSize) : 0.0f;

  // 700 is the `bold` keyword; lighter weights do not light up the Bold toggle.
  state->bold = style.fontWeight.get().value() >= 700;
  state->italic = style.fontStyle.get().value() == svg::FontStyle::Italic;
  state->underline =
      svg::hasFlag(style.textDecoration.get().value(), svg::TextDecoration::Underline);
}

bool ApplyFormatBarActionsToSelection(const FormatBarActions& actions, const FormatBarState& state,
                                      bool routeTogglesToSelection, EditorApp& app) {
  bool queued = false;

  if (actions.setFontFamily) {
    queued = app.setAttributeOnSelection("font-family", actions.fontFamily) || queued;
  }
  if (actions.setFontSize) {
    queued = app.setAttributeOnSelection("font-size", donner::detail::FormatNumberForSVG(
                                                          static_cast<double>(actions.fontSize))) ||
             queued;
  }

  if (routeTogglesToSelection) {
    // Toggle to the opposite of the currently-displayed state. Toggle-off
    // writes the explicit reset value so this stays on the attribute-write path
    // (the editing-session path in TextTool removes the attribute instead).
    if (actions.toggleBold) {
      queued = app.setAttributeOnSelection("font-weight", state.bold ? "normal" : "bold") || queued;
    }
    if (actions.toggleItalic) {
      queued =
          app.setAttributeOnSelection("font-style", state.italic ? "normal" : "italic") || queued;
    }
    if (actions.toggleUnderline) {
      queued =
          app.setAttributeOnSelection("text-decoration", state.underline ? "none" : "underline") ||
          queued;
    }
  }

  return queued;
}

std::vector<FormatBarFontFamily> BuildFormatBarFamilies(
    const std::vector<svg::FontFamilyInfo>& catalogFamilies,
    const std::function<FormatBarFontPreview(const svg::FontFamilyInfo&)>& previewForFamily) {
  std::vector<FormatBarFontFamily> families;
  families.reserve(catalogFamilies.size());
  // Preserve the catalog's ordering (Bundled group first, then System, sorted
  // within each), so the picker shows the same grouping the header separators
  // key off of.
  for (const svg::FontFamilyInfo& info : catalogFamilies) {
    families.push_back(FormatBarFontFamily{
        .name = info.family,
        .preview = previewForFamily ? previewForFamily(info) : FormatBarFontPreview{},
        .source = info.source,
    });
  }
  return families;
}

namespace {

bool RenderFormatToggle(const char* label, bool active, ImFont* face, float width) {
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  if (face != nullptr) {
    ImGui::PushFont(face);
  }
  const bool clicked = ImGui::Button(label, ImVec2(width, 0.0f));
  if (face != nullptr) {
    ImGui::PopFont();
  }
  if (active) {
    ImGui::PopStyleColor();
  }
  return clicked;
}

}  // namespace

float TextFormatBarPresenter::BarHeight() {
  return ImGui::GetFrameHeight() + 16.0f;
}

FormatBarActions TextFormatBarPresenter::render(const FormatBarState& state, const ImVec2& topLeft,
                                                float width) {
  FormatBarActions actions;
  fontSizePresetRects_.fill(std::nullopt);
  if (!state.visible) {
    return actions;
  }

  // Re-seed the free-text family buffer whenever the underlying value changes,
  // so an external edit (selection change, another surface) is reflected while
  // an in-progress edit here is not clobbered mid-keystroke.
  if (!trackedFamily_ || lastSyncedFamily_ != state.fontFamily) {
    AssignBuffer(fontFamilyBuffer_, state.fontFamily);
    fontFamilySeedTruncated_ = state.fontFamily.size() >= fontFamilyBuffer_.size();
    lastSyncedFamily_ = state.fontFamily;
    trackedFamily_ = true;
  }

  const EditorTheme& theme = EditorTheme::Active();
  const ImVec2 size(width, BarHeight());
  const ImVec2 shadowOffset(0.0f, 3.0f);
  ImDrawList* drawList = ImGui::GetWindowDrawList();
  drawList->AddRectFilled(
      ImVec2(topLeft.x + shadowOffset.x, topLeft.y + shadowOffset.y),
      ImVec2(topLeft.x + size.x + shadowOffset.x, topLeft.y + size.y + shadowOffset.y),
      WithAlpha(theme.surfaceCanvas, 150), theme.radiusContainer);

  // Keep the controls in a child of the render pane. The old independent
  // NoBringToFrontOnFocus window could remain behind its parent after the user
  // selected text, leaving a visible bar whose controls could not receive
  // clicks. A child owns its screen rect above the pane's overlapping canvas
  // item without taking keyboard focus merely by appearing.
  const ImVec2 savedCursorPos = ImGui::GetCursorPos();
  ImGui::SetCursorScreenPos(topLeft);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme.radiusContainer);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(theme.surfaceOverlay));
  ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(theme.borderStrong));
  constexpr ImGuiChildFlags kChildFlags =
      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
  constexpr ImGuiWindowFlags kWindowFlags =
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNavFocus;
  if (ImGui::BeginChild("##text_format_bar", size, kChildFlags, kWindowFlags)) {
    // --- Font family: free-text input plus a searchable preview dropdown. ---
    renderFontFamilyInput(actions);
    renderFontFamilyMenu(state, actions);
    ImVec2 sizeFieldMin;
    ImVec2 sizeFieldMax;
    bool sizeFieldActivated = false;
    renderFontSizeInput(state, actions, sizeFieldMin, sizeFieldMax, sizeFieldActivated);

    // --- Bold / Italic / Underline toggles. ---
    const float toggleWidth = ImGui::GetFrameHeight();
    ImGui::SameLine();
    if (RenderFormatToggle("B##format_bar_bold", state.bold, state.boldToggleFont, toggleWidth)) {
      actions.toggleBold = true;
    }
    ImGui::SameLine();
    if (RenderFormatToggle("I##format_bar_italic", state.italic, nullptr, toggleWidth)) {
      actions.toggleItalic = true;
    }
    ImGui::SameLine();
    if (RenderFormatToggle("U##format_bar_underline", state.underline, nullptr, toggleWidth)) {
      actions.toggleUnderline = true;
    }

    renderFontSizePresetPopup(state, sizeFieldMin, sizeFieldMax, sizeFieldActivated, &actions);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor(2);
  ImGui::PopStyleVar(3);
  ImGui::SetCursorPos(savedCursorPos);

  return actions;
}

void TextFormatBarPresenter::renderFontFamilyInput(FormatBarActions& actions) {
  ImGui::SetNextItemWidth(180.0f);
  ImGui::InputTextWithHint("##format_bar_font_family", "Font family", fontFamilyBuffer_.data(),
                           fontFamilyBuffer_.size(),
                           fontFamilySeedTruncated_ ? ImGuiInputTextFlags_ReadOnly : 0);
  if (fontFamilySeedTruncated_ && ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "This font list is too long for inline editing. Choose a font or edit it in source.");
  }
  if (!fontFamilySeedTruncated_ && ImGui::IsItemDeactivatedAfterEdit()) {
    actions.setFontFamily = true;
    actions.fontFamily = std::string(fontFamilyBuffer_.data());
    lastSyncedFamily_ = actions.fontFamily;
  }
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight());
}

void TextFormatBarPresenter::renderFontFamilyMenu(const FormatBarState& state,
                                                  FormatBarActions& actions) {
  if (ImGui::BeginCombo("##format_bar_font_family_menu", "", ImGuiComboFlags_NoPreview)) {
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##format_bar_font_search", "Search fonts", familySearchBuffer_.data(),
                             familySearchBuffer_.size());
    const std::string_view filter(familySearchBuffer_.data());
    std::optional<svg::FontSource> shownGroup;
    for (const FormatBarFontFamily& family : state.families) {
      if (!ContainsCaseInsensitive(family.name, filter)) {
        continue;
      }
      // Header at each Bundled/System boundary. Because `state.families` is
      // grouped (Bundled then System), the source changes at most once among
      // the filtered rows, so this prints one header per non-empty group.
      if (!shownGroup.has_value() || *shownGroup != family.source) {
        ImGui::TextDisabled(family.source == svg::FontSource::Bundled ? "Bundled" : "System");
        shownGroup = family.source;
      }
      const std::string familyCss = internal::EditableFontFamily(family.name);
      const bool chosen =
          internal::RenderFontFamilyRow(family, familyCss == state.fontFamily, actions);
      if (chosen) {
        actions.setFontFamily = true;
        actions.fontFamily = familyCss;
        AssignBuffer(fontFamilyBuffer_, familyCss);
        fontFamilySeedTruncated_ = familyCss.size() >= fontFamilyBuffer_.size();
        lastSyncedFamily_ = familyCss;
      }
    }
    ImGui::EndCombo();
  }
}

void TextFormatBarPresenter::renderFontSizeInput(const FormatBarState& state,
                                                 FormatBarActions& actions, ImVec2& sizeFieldMin,
                                                 ImVec2& sizeFieldMax, bool& sizeFieldActivated) {
  const EditorTheme& theme = EditorTheme::Active();
  ImGui::SameLine();
  if (!sizeControlActive_) {
    sizeEditValue_ = state.hasFontSize ? state.fontSize : 0.0f;
  }
  ImGui::SetNextItemWidth(128.0f);
  ImGui::InputFloat("##format_bar_font_size", &sizeEditValue_, 0.0f, 0.0f, "%.0f",
                    ImGuiInputTextFlags_AutoSelectAll);
  sizeControlActive_ = ImGui::IsItemActive();
  sizeFieldActivated = ImGui::IsItemActivated();
  const bool sizeEditCommitted = ImGui::IsItemDeactivatedAfterEdit();
  sizeFieldMin = ImGui::GetItemRectMin();
  sizeFieldMax = ImGui::GetItemRectMax();
  const AttachedNumericStepperResult sizeSteps =
      RenderAttachedNumericStepper("format_bar_font_size_steps", sizeFieldMin, sizeFieldMax, theme);
  if (sizeEditCommitted) {
    sizeEditValue_ =
        std::isfinite(sizeEditValue_) ? std::clamp(sizeEditValue_, 1.0f, 512.0f) : 16.0f;
    actions.setFontSize = true;
    actions.fontSize = sizeEditValue_;
  }
  if (sizeSteps.increment || sizeSteps.decrement) {
    const float currentSize =
        std::isfinite(sizeEditValue_) && sizeEditValue_ >= 1.0f ? sizeEditValue_ : 16.0f;
    sizeEditValue_ = std::clamp(currentSize + (sizeSteps.increment ? 1.0f : -1.0f), 1.0f, 512.0f);
    actions.setFontSize = true;
    actions.fontSize = sizeEditValue_;
  }
}

void TextFormatBarPresenter::renderFontSizePresetPopup(const FormatBarState& state,
                                                       const ImVec2& fieldMin,
                                                       const ImVec2& fieldMax, bool fieldActivated,
                                                       FormatBarActions* actions) {
  if (!BeginHybridNumericPresetPopup("##format_bar_font_size_presets", fieldMin, fieldMax,
                                     fieldActivated, 320.0f)) {
    return;
  }
  const EditorTheme& theme = EditorTheme::Active();
  for (std::size_t index = 0; index < kFormatBarFontSizePresets.size(); ++index) {
    ImGui::PushID(static_cast<int>(index));
    const int preset = kFormatBarFontSizePresets[index];
    const float previewSize = static_cast<float>(preset);
    const float rowHeight = std::max(30.0f, previewSize + 10.0f);
    const bool selected = state.hasFontSize && static_cast<int>(state.fontSize + 0.5f) == preset;
    const bool chosen = ImGui::Selectable("##size_preset", selected, 0, ImVec2(0.0f, rowHeight));
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    fontSizePresetRects_[index] = Box2d(Vector2d(rowMin.x, rowMin.y), Vector2d(rowMax.x, rowMax.y));
    ImDrawList* previewDraw = ImGui::GetWindowDrawList();
    previewDraw->PushClipRect(ImVec2(rowMin.x + 4.0f, rowMin.y), ImVec2(rowMin.x + 78.0f, rowMax.y),
                              true);
    previewDraw->AddText(ImGui::GetFont(), previewSize, ImVec2(rowMin.x + 8.0f, rowMin.y + 4.0f),
                         theme.textPrimary, "A");
    previewDraw->PopClipRect();
    char label[8];
    std::snprintf(label, sizeof(label), "%d", preset);
    previewDraw->AddText(
        ImVec2(rowMin.x + 88.0f, rowMin.y + (rowHeight - ImGui::GetFontSize()) * 0.5f),
        selected ? theme.accentDefault : theme.textPrimary, label);
    if (chosen) {
      actions->setFontSize = true;
      actions->fontSize = previewSize;
      sizeEditValue_ = previewSize;
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopID();
  }
  ImGui::EndPopup();
}

}  // namespace donner::editor
