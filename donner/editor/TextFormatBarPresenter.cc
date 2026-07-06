#include "donner/editor/TextFormatBarPresenter.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "donner/base/FormatNumber.h"
#include "donner/base/RcString.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/EditorTheme.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/SVGElement.h"

namespace donner::editor {

namespace internal {

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

/// Parse a leading number from an attribute value (e.g. "16", "16px"). Returns
/// nullopt when no number is present.
std::optional<double> ParseLeadingNumber(std::string_view value) {
  const auto result = ::donner::parser::NumberParser::Parse(value);
  if (result.hasError()) {
    return std::nullopt;
  }
  return result.result().number;
}

}  // namespace internal

using internal::AssignBuffer;
using internal::ContainsCaseInsensitive;
using internal::ParseLeadingNumber;

bool FormatBarShouldShow(bool hasSingleTextSelection, bool textEditingActive) {
  return hasSingleTextSelection || textEditingActive;
}

void ReadTextFormatState(const svg::SVGElement& text, FormatBarState* state) {
  if (state == nullptr) {
    return;
  }
  // §concurrent-dom: reading attributes (raw ECS access) from the UI thread
  // needs a scoped read access while the live editor keeps the document in
  // ThreadingMode::ConcurrentDom, mirroring TextInspectorPanel.
  text.withReadAccess([&](svg::DocumentReadAccess&, EntityHandle) {
    if (auto family = text.getAttribute("font-family")) {
      state->fontFamily = std::string(std::string_view(*family));
    } else {
      state->fontFamily.clear();
    }

    state->hasFontSize = false;
    state->fontSize = 0.0f;
    if (auto size = text.getAttribute("font-size")) {
      if (auto parsed = ParseLeadingNumber(std::string_view(*size))) {
        state->fontSize = static_cast<float>(*parsed);
        state->hasFontSize = true;
      }
    }

    const auto weight = text.getAttribute("font-weight");
    state->bold = weight.has_value() && std::string_view(*weight) == "bold";

    const auto style = text.getAttribute("font-style");
    state->italic = style.has_value() && std::string_view(*style) == "italic";

    const auto decoration = text.getAttribute("text-decoration");
    state->underline = decoration.has_value() && std::string_view(*decoration) == "underline";
  });
}

bool ApplyFormatBarActionsToSelection(const FormatBarActions& actions, const FormatBarState& state,
                                      bool routeTogglesToSelection, EditorApp& app) {
  bool queued = false;

  if (actions.setFontFamily) {
    queued = app.setAttributeOnSelection("font-family", actions.fontFamily) || queued;
  }
  if (actions.setFontSize) {
    queued = app.setAttributeOnSelection(
                 "font-size", donner::detail::FormatNumberForSVG(
                                  static_cast<double>(actions.fontSize))) ||
             queued;
  }

  if (routeTogglesToSelection) {
    // Toggle to the opposite of the currently-displayed state. Toggle-off
    // writes the explicit reset value so this stays on the attribute-write path
    // (the editing-session path in TextTool removes the attribute instead).
    if (actions.toggleBold) {
      queued =
          app.setAttributeOnSelection("font-weight", state.bold ? "normal" : "bold") || queued;
    }
    if (actions.toggleItalic) {
      queued =
          app.setAttributeOnSelection("font-style", state.italic ? "normal" : "italic") || queued;
    }
    if (actions.toggleUnderline) {
      queued = app.setAttributeOnSelection("text-decoration",
                                           state.underline ? "none" : "underline") ||
               queued;
    }
  }

  return queued;
}

std::vector<FormatBarFontFamily> BuildFormatBarFamilies(
    const std::vector<svg::FontFamilyInfo>& catalogFamilies,
    const std::function<ImFont*(const svg::FontFamilyInfo&)>& previewForFamily) {
  std::vector<FormatBarFontFamily> families;
  families.reserve(catalogFamilies.size());
  // Preserve the catalog's ordering (Embedded group first, then System, sorted
  // within each), so the picker shows the same grouping the header separators
  // key off of.
  for (const svg::FontFamilyInfo& info : catalogFamilies) {
    families.push_back(FormatBarFontFamily{
        .name = info.family,
        .previewFont = previewForFamily ? previewForFamily(info) : nullptr,
        .source = info.source,
    });
  }
  return families;
}

FormatBarActions TextFormatBarPresenter::render(const FormatBarState& state,
                                                const FormatBarPlacement& placement) {
  FormatBarActions actions;
  if (!state.visible) {
    return actions;
  }

  // Re-seed the free-text family buffer whenever the underlying value changes,
  // so an external edit (selection change, another surface) is reflected while
  // an in-progress edit here is not clobbered mid-keystroke.
  if (!trackedFamily_ || lastSyncedFamily_ != state.fontFamily) {
    AssignBuffer(fontFamilyBuffer_, state.fontFamily);
    lastSyncedFamily_ = state.fontFamily;
    trackedFamily_ = true;
  }

  // Anchor the palette just below the tool palette, then clamp so an auto-sized
  // bar wider/taller than the toolbar never spills outside the canvas pane. The
  // clamp uses last frame's measured size; the content is stable, so this
  // converges in one frame and reads as instant.
  constexpr float kEdgeMargin = 4.0f;
  float posX = placement.anchorX;
  float posY = placement.anchorY;
  if (lastBarWidth_ > 0.0f) {
    posX = std::min(posX, placement.clampMaxX - lastBarWidth_ - kEdgeMargin);
  }
  if (lastBarHeight_ > 0.0f) {
    posY = std::min(posY, placement.clampMaxY - lastBarHeight_ - kEdgeMargin);
  }
  posX = std::max(posX, placement.clampMinX + kEdgeMargin);
  posY = std::max(posY, placement.clampMinY + kEdgeMargin);

  ImGui::SetNextWindowPos(ImVec2(posX, posY));
  constexpr ImGuiWindowFlags kBarFlags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_AlwaysAutoResize;

  // Floating-palette chrome from the editor theme: rounded container on the
  // raised surface, so the bar reads as a palette over the canvas rather than a
  // full-width strip.
  const EditorTheme& theme = EditorTheme::Active();
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme.radiusContainer);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.surfaceRaised);
  const bool open = ImGui::Begin("##text_format_bar", nullptr, kBarFlags);
  if (open) {
    // --- Font family: free-text input plus a searchable preview dropdown. ---
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##format_bar_font_family", "Font family", fontFamilyBuffer_.data(),
                             fontFamilyBuffer_.size());
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      actions.setFontFamily = true;
      actions.fontFamily = std::string(fontFamilyBuffer_.data());
      lastSyncedFamily_ = actions.fontFamily;
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (ImGui::BeginCombo("##format_bar_font_family_menu", "", ImGuiComboFlags_NoPreview)) {
      ImGui::SetNextItemWidth(200.0f);
      ImGui::InputTextWithHint("##format_bar_font_search", "Search fonts",
                               familySearchBuffer_.data(), familySearchBuffer_.size());
      const std::string_view filter(familySearchBuffer_.data());
      std::optional<svg::FontSource> shownGroup;
      for (const FormatBarFontFamily& family : state.families) {
        if (!ContainsCaseInsensitive(family.name, filter)) {
          continue;
        }
        // Header at each Embedded/System boundary. Because `state.families` is
        // grouped (Embedded then System), the source changes at most once among
        // the filtered rows, so this prints one header per non-empty group.
        if (!shownGroup.has_value() || *shownGroup != family.source) {
          ImGui::TextDisabled(family.source == svg::FontSource::Embedded ? "Embedded" : "System");
          shownGroup = family.source;
        }
        const bool selected = family.name == state.fontFamily;
        // Preview each family in its own face where the editor has one loaded.
        if (family.previewFont != nullptr) {
          ImGui::PushFont(family.previewFont);
        }
        const bool chosen = ImGui::Selectable(family.name.c_str(), selected);
        if (family.previewFont != nullptr) {
          ImGui::PopFont();
        }
        if (chosen) {
          actions.setFontFamily = true;
          actions.fontFamily = family.name;
          AssignBuffer(fontFamilyBuffer_, family.name);
          lastSyncedFamily_ = family.name;
        }
      }
      ImGui::EndCombo();
    }

    // --- Font size: drag box plus a preset dropdown. ---
    ImGui::SameLine();
    if (!sizeControlActive_) {
      sizeEditValue_ = state.hasFontSize ? state.fontSize : 0.0f;
    }
    ImGui::SetNextItemWidth(64.0f);
    ImGui::DragFloat("##format_bar_font_size", &sizeEditValue_, 0.5f, 1.0f, 512.0f, "%.0f");
    sizeControlActive_ = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      actions.setFontSize = true;
      actions.fontSize = sizeEditValue_;
    }
    ImGui::SameLine(0.0f, 0.0f);
    if (ImGui::BeginCombo("##format_bar_font_size_menu", "", ImGuiComboFlags_NoPreview)) {
      for (const int preset : kFormatBarFontSizePresets) {
        const bool selected = state.hasFontSize &&
                              static_cast<int>(state.fontSize + 0.5f) == preset;
        char label[8];
        std::snprintf(label, sizeof(label), "%d", preset);
        if (ImGui::Selectable(label, selected)) {
          actions.setFontSize = true;
          actions.fontSize = static_cast<float>(preset);
          sizeEditValue_ = actions.fontSize;
        }
      }
      ImGui::EndCombo();
    }

    // --- Bold / Italic / Underline toggles. ---
    const float toggleWidth = ImGui::GetFrameHeight();
    const auto toggle = [&](const char* label, bool active, ImFont* face) -> bool {
      if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      if (face != nullptr) {
        ImGui::PushFont(face);
      }
      const bool clicked = ImGui::Button(label, ImVec2(toggleWidth, 0.0f));
      if (face != nullptr) {
        ImGui::PopFont();
      }
      if (active) {
        ImGui::PopStyleColor();
      }
      return clicked;
    };

    ImGui::SameLine();
    if (toggle("B##format_bar_bold", state.bold, state.boldToggleFont)) {
      actions.toggleBold = true;
    }
    ImGui::SameLine();
    if (toggle("I##format_bar_italic", state.italic, nullptr)) {
      actions.toggleItalic = true;
    }
    ImGui::SameLine();
    if (toggle("U##format_bar_underline", state.underline, nullptr)) {
      actions.toggleUnderline = true;
    }

    // Remember the auto-sized palette footprint so next frame can clamp it
    // within the canvas pane before committing its position.
    const ImVec2 barSize = ImGui::GetWindowSize();
    lastBarWidth_ = barSize.x;
    lastBarHeight_ = barSize.y;
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();

  return actions;
}

}  // namespace donner::editor
