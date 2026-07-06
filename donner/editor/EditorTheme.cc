#include "donner/editor/EditorTheme.h"

namespace donner::editor {

namespace {

/// One accent's three tints (design doc 0054, deepest to lightest).
struct AccentTints {
  ImU32 active;
  ImU32 defaultTint;
  ImU32 hover;
  css::RGBA defaultRgba;
};

AccentTints TintsFor(Accent accent) {
  switch (accent) {
    case Accent::AzimuthBlue:  // A: #3B79E0 / #4C8DF6 / #6BA0F8
      return {IM_COL32(0x3B, 0x79, 0xE0, 0xFF), IM_COL32(0x4C, 0x8D, 0xF6, 0xFF),
              IM_COL32(0x6B, 0xA0, 0xF8, 0xFF), css::RGBA(0x4C, 0x8D, 0xF6, 0xFF)};
    case Accent::SignalTeal:  // B: #249E8C / #31C6B3 / #54D9C8
      return {IM_COL32(0x24, 0x9E, 0x8C, 0xFF), IM_COL32(0x31, 0xC6, 0xB3, 0xFF),
              IM_COL32(0x54, 0xD9, 0xC8, 0xFF), css::RGBA(0x31, 0xC6, 0xB3, 0xFF)};
    case Accent::UltraViolet:  // C: #7B5CE6 / #9A7CF7 / #B29AF9
      return {IM_COL32(0x7B, 0x5C, 0xE6, 0xFF), IM_COL32(0x9A, 0x7C, 0xF7, 0xFF),
              IM_COL32(0xB2, 0x9A, 0xF9, 0xFF), css::RGBA(0x9A, 0x7C, 0xF7, 0xFF)};
    case Accent::MacBlue:
      // D: #086ACC / #0A84FF / #47A3FF (QA-F8, 2026-07-06). #0A84FF is Apple's
      // macOS system blue, specified exactly by QA-F8; the active/hover tints
      // are not itemized by QA-F8, so they are derived the same way the other
      // three variants derive theirs (active = 20% darker, hover = 25% toward
      // white; B's own channel is already saturated at 0xFF so hover lightens
      // via desaturation instead of a further blue increase).
      return {IM_COL32(0x08, 0x6A, 0xCC, 0xFF), IM_COL32(0x0A, 0x84, 0xFF, 0xFF),
              IM_COL32(0x47, 0xA3, 0xFF, 0xFF), css::RGBA(0x0A, 0x84, 0xFF, 0xFF)};
  }
  // Unreachable for valid enum values; fall back to the shipped default.
  return {IM_COL32(0x08, 0x6A, 0xCC, 0xFF), IM_COL32(0x0A, 0x84, 0xFF, 0xFF),
          IM_COL32(0x47, 0xA3, 0xFF, 0xFF), css::RGBA(0x0A, 0x84, 0xFF, 0xFF)};
}

}  // namespace

EditorTheme EditorTheme::Dark(Accent accent) {
  EditorTheme t{};

  // Surfaces (QA-F8 macOS-dark-mode-like ramp, 2026-07-06). base/raised/
  // overlay/hover are the exact QA-F8 hexes (matching Apple's dark-mode
  // systemGray6/5/4/3); canvas and active are not itemized by QA-F8 and
  // continue the same ramp one step darker / one step lighter (systemGray2).
  t.surfaceCanvas = IM_COL32(0x10, 0x10, 0x12, 0xFF);
  t.surfaceSunken = IM_COL32(0x16, 0x16, 0x18, 0xFF);
  t.surfaceBase = IM_COL32(0x1C, 0x1C, 0x1E, 0xFF);
  t.surfaceRaised = IM_COL32(0x2C, 0x2C, 0x2E, 0xFF);
  t.surfaceOverlay = IM_COL32(0x3A, 0x3A, 0x3C, 0xFF);
  t.surfaceHover = IM_COL32(0x48, 0x48, 0x4A, 0xFF);
  t.surfaceActive = IM_COL32(0x63, 0x63, 0x66, 0xFF);

  // Borders. Not itemized by QA-F8; mapped onto the same ramp as the
  // surfaces they divide (raised/overlay) rather than left at the old hexes.
  t.borderSubtle = IM_COL32(0x2C, 0x2C, 0x2E, 0xFF);
  t.borderStrong = IM_COL32(0x3A, 0x3A, 0x3C, 0xFF);

  // Text (QA-F8 hexes; muted/disabled alpha-blend over surfaceBase rather
  // than being pre-flattened, matching Apple's secondary/tertiaryLabelColor).
  t.textPrimary = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
  t.textMuted = IM_COL32(0xEB, 0xEB, 0xF5, 0x99);     // 0x99/0xFF = 60%.
  t.textDisabled = IM_COL32(0xEB, 0xEB, 0xF5, 0x4D);  // 0x4D/0xFF = 30%.

  // Accent (chosen variant) + dark ink. Ink is not itemized by QA-F8 beyond
  // "dark ink on fills"; it matches surfaceCanvas, as before.
  const AccentTints tints = TintsFor(accent);
  t.accentActive = tints.active;
  t.accentDefault = tints.defaultTint;
  t.accentHover = tints.hover;
  t.accentInk = IM_COL32(0x10, 0x10, 0x12, 0xFF);
  t.accentColor = tints.defaultRgba;

  // Selection is derived from the accent; QA-F8 keeps stroke + 22% fill.
  t.selectionStroke = tints.defaultTint;
  t.selectionFillAlpha = 0.22f;

  // Semantic states (QA-F8 hexes).
  t.warning = IM_COL32(0xFF, 0xD6, 0x0A, 0xFF);
  t.destructive = IM_COL32(0xFF, 0x45, 0x3A, 0xFF);
  t.success = IM_COL32(0x32, 0xD7, 0x4B, 0xFF);

  // Metrics keep their in-struct defaults (4 px grid, radii, control sizes).
  return t;
}

css::RGBA EditorTheme::selectionRgba(uint8_t alpha) const {
  return css::RGBA(accentColor.r, accentColor.g, accentColor.b, alpha);
}

void EditorTheme::applyToImGuiStyle(ImGuiStyle& style) const {
  const auto set = [&](ImGuiCol slot, ImU32 token) {
    style.Colors[slot] = ImGui::ColorConvertU32ToFloat4(token);
  };

  // Surfaces / containers.
  set(ImGuiCol_WindowBg, surfaceBase);
  set(ImGuiCol_ChildBg, surfaceBase);
  set(ImGuiCol_PopupBg, surfaceOverlay);
  set(ImGuiCol_MenuBarBg, surfaceRaised);
  set(ImGuiCol_TitleBg, surfaceRaised);
  set(ImGuiCol_TitleBgActive, surfaceRaised);
  set(ImGuiCol_TitleBgCollapsed, surfaceRaised);

  // Frames (fields, combos, checkboxes).
  set(ImGuiCol_FrameBg, surfaceRaised);
  set(ImGuiCol_FrameBgHovered, surfaceHover);
  set(ImGuiCol_FrameBgActive, surfaceActive);

  // Buttons.
  set(ImGuiCol_Button, surfaceRaised);
  set(ImGuiCol_ButtonHovered, surfaceHover);
  set(ImGuiCol_ButtonActive, surfaceActive);

  // Headers (tree rows, selectables, collapsing headers).
  set(ImGuiCol_Header, surfaceActive);
  set(ImGuiCol_HeaderHovered, surfaceHover);
  set(ImGuiCol_HeaderActive, surfaceActive);

  // Borders / separators.
  set(ImGuiCol_Border, borderSubtle);
  set(ImGuiCol_Separator, borderSubtle);
  set(ImGuiCol_SeparatorHovered, borderStrong);
  set(ImGuiCol_SeparatorActive, accentDefault);

  // Text.
  set(ImGuiCol_Text, textPrimary);
  set(ImGuiCol_TextDisabled, textDisabled);

  // Accent-driven affordances.
  set(ImGuiCol_CheckMark, accentDefault);
  set(ImGuiCol_SliderGrab, accentDefault);
  set(ImGuiCol_SliderGrabActive, accentHover);
  set(ImGuiCol_NavHighlight, accentDefault);

  // Scrollbars.
  set(ImGuiCol_ScrollbarBg, surfaceSunken);
  set(ImGuiCol_ScrollbarGrab, surfaceHover);
  set(ImGuiCol_ScrollbarGrabHovered, surfaceActive);
  set(ImGuiCol_ScrollbarGrabActive, accentDefault);

  // Rounding (QA-F8 nudged radiusContainer 6 -> 8 for a rounder macOS-panel
  // feel; radiusControl is unchanged).
  style.FrameRounding = radiusControl;
  style.GrabRounding = radiusControl;
  style.WindowRounding = radiusContainer;
  style.PopupRounding = radiusContainer;
  style.ChildRounding = radiusContainer;

  // Padding / spacing (4 px grid).
  style.FramePadding = ImVec2(space2, space1);
  style.ItemSpacing = ImVec2(space2, 6.0f);
  style.WindowPadding = ImVec2(space2, space2);
  style.IndentSpacing = space4;
  style.ScrollbarSize = scrollbarSize;

  SetActive(*this);
}

const EditorTheme& EditorTheme::Active() {
  static EditorTheme active = EditorTheme::Dark(Accent::MacBlue);
  return active;
}

void EditorTheme::SetActive(const EditorTheme& theme) {
  // Active() owns the single mutable instance; re-fetch and overwrite it.
  const_cast<EditorTheme&>(Active()) = theme;
}

ImU32 WithAlpha(ImU32 color, uint8_t alpha) {
  return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

}  // namespace donner::editor
