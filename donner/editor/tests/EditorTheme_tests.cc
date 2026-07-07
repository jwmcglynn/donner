#include "donner/editor/EditorTheme.h"

#include <gtest/gtest.h>

#include <array>

namespace donner::editor {

namespace {

constexpr std::array<Accent, kAccentCount> kAllAccents = {
    Accent::AzimuthBlue, Accent::SignalTeal, Accent::UltraViolet};

/// Every color token, so completeness can be asserted without listing each one
/// at every call site. None of the design-doc tokens is transparent-black
/// (IM_COL32 == 0), so "nonzero" is a sound "was populated" proxy.
std::array<ImU32, 20> AllColorTokens(const EditorTheme& t) {
  return {t.surfaceCanvas,   t.surfaceSunken, t.surfaceBase,   t.surfaceRaised,
          t.surfaceOverlay,  t.surfaceHover,  t.surfaceActive, t.borderSubtle,
          t.borderStrong,    t.textPrimary,   t.textMuted,     t.textDisabled,
          t.accentActive,    t.accentDefault, t.accentHover,   t.accentInk,
          t.selectionStroke, t.warning,       t.destructive,   t.success};
}

}  // namespace

// Every accent yields a fully-populated token struct.
TEST(EditorTheme, TokenCompletenessForEveryAccent) {
  for (const Accent accent : kAllAccents) {
    const EditorTheme theme = EditorTheme::Dark(accent);
    for (const ImU32 token : AllColorTokens(theme)) {
      EXPECT_NE(token, 0u) << "an accent " << static_cast<int>(accent)
                           << " color token was left transparent-black";
    }
    // Metrics are on the 4 px grid with the doc's concrete defaults.
    EXPECT_FLOAT_EQ(theme.space1, 4.0f);
    EXPECT_FLOAT_EQ(theme.space2, 8.0f);
    EXPECT_FLOAT_EQ(theme.space4, 16.0f);
    EXPECT_FLOAT_EQ(theme.radiusControl, 4.0f);
    EXPECT_FLOAT_EQ(theme.radiusContainer, 6.0f);
    EXPECT_FLOAT_EQ(theme.scrollbarSize, 12.0f);
    EXPECT_FLOAT_EQ(theme.selectionFillAlpha, 0.22f);
    // Ink on accent fills is the dark ink for every accent.
    EXPECT_EQ(theme.accentInk, IM_COL32(0x0E, 0x11, 0x16, 0xFF));
    // Selection derives from the accent.
    EXPECT_EQ(theme.selectionStroke, theme.accentDefault);
  }
}

// The shipped default is SignalTeal (variant B), and each accent is distinct.
TEST(EditorTheme, AccentEnumCoverage) {
  const EditorTheme defaulted = EditorTheme::Dark();
  const EditorTheme teal = EditorTheme::Dark(Accent::SignalTeal);
  EXPECT_EQ(defaulted.accentDefault, teal.accentDefault);
  EXPECT_EQ(teal.accentDefault, IM_COL32(0x31, 0xC6, 0xB3, 0xFF));

  const EditorTheme blue = EditorTheme::Dark(Accent::AzimuthBlue);
  const EditorTheme violet = EditorTheme::Dark(Accent::UltraViolet);
  EXPECT_EQ(blue.accentDefault, IM_COL32(0x4C, 0x8D, 0xF6, 0xFF));
  EXPECT_EQ(violet.accentDefault, IM_COL32(0x9A, 0x7C, 0xF7, 0xFF));

  // All three accents are distinct on their default tint.
  EXPECT_NE(blue.accentDefault, teal.accentDefault);
  EXPECT_NE(teal.accentDefault, violet.accentDefault);
  EXPECT_NE(blue.accentDefault, violet.accentDefault);

  // Surfaces / semantics are accent-independent.
  EXPECT_EQ(blue.surfaceBase, teal.surfaceBase);
  EXPECT_EQ(blue.destructive, violet.destructive);
}

// The named palette values match design doc 0054 exactly.
TEST(EditorTheme, PaletteMatchesDoc) {
  const EditorTheme t = EditorTheme::Dark(Accent::SignalTeal);
  EXPECT_EQ(t.surfaceCanvas, IM_COL32(0x0E, 0x11, 0x16, 0xFF));
  EXPECT_EQ(t.surfaceBase, IM_COL32(0x16, 0x1B, 0x22, 0xFF));
  EXPECT_EQ(t.surfaceRaised, IM_COL32(0x1E, 0x25, 0x2E, 0xFF));
  EXPECT_EQ(t.surfaceOverlay, IM_COL32(0x23, 0x2B, 0x36, 0xFF));
  EXPECT_EQ(t.textPrimary, IM_COL32(0xE6, 0xEA, 0xF0, 0xFF));
  EXPECT_EQ(t.textMuted, IM_COL32(0x9A, 0xA5, 0xB4, 0xFF));
  EXPECT_EQ(t.warning, IM_COL32(0xE3, 0xB3, 0x41, 0xFF));
  EXPECT_EQ(t.destructive, IM_COL32(0xF0, 0x61, 0x6A, 0xFF));
  EXPECT_EQ(t.success, IM_COL32(0x3F, 0xB9, 0x84, 0xFF));
}

// The css seam returns the accent tinted to the requested alpha.
TEST(EditorTheme, SelectionRgbaTintsAccent) {
  const EditorTheme t = EditorTheme::Dark(Accent::SignalTeal);
  const css::RGBA opaque = t.selectionRgba();
  EXPECT_EQ(opaque.r, 0x31);
  EXPECT_EQ(opaque.g, 0xC6);
  EXPECT_EQ(opaque.b, 0xB3);
  EXPECT_EQ(opaque.a, 0xFF);

  const css::RGBA faint = t.selectionRgba(0x33);
  EXPECT_EQ(faint.r, 0x31);
  EXPECT_EQ(faint.g, 0xC6);
  EXPECT_EQ(faint.b, 0xB3);
  EXPECT_EQ(faint.a, 0x33);
}

// applyToImGuiStyle sets every mapped ImGuiCol_ slot and style var (doc table).
TEST(EditorTheme, ApplyToImGuiStyleSetsMappedSlots) {
  const EditorTheme t = EditorTheme::Dark(Accent::SignalTeal);
  ImGuiStyle style;  // Default-constructed; no live ImGui context required.
  t.applyToImGuiStyle(style);

  const auto slotU32 = [&](ImGuiCol slot) {
    return ImGui::ColorConvertFloat4ToU32(style.Colors[slot]);
  };

  EXPECT_EQ(slotU32(ImGuiCol_WindowBg), t.surfaceBase);
  EXPECT_EQ(slotU32(ImGuiCol_ChildBg), t.surfaceBase);
  EXPECT_EQ(slotU32(ImGuiCol_PopupBg), t.surfaceOverlay);
  EXPECT_EQ(slotU32(ImGuiCol_MenuBarBg), t.surfaceRaised);
  EXPECT_EQ(slotU32(ImGuiCol_TitleBgActive), t.surfaceRaised);
  EXPECT_EQ(slotU32(ImGuiCol_FrameBg), t.surfaceRaised);
  EXPECT_EQ(slotU32(ImGuiCol_FrameBgHovered), t.surfaceHover);
  EXPECT_EQ(slotU32(ImGuiCol_FrameBgActive), t.surfaceActive);
  EXPECT_EQ(slotU32(ImGuiCol_Button), t.surfaceRaised);
  EXPECT_EQ(slotU32(ImGuiCol_ButtonHovered), t.surfaceHover);
  EXPECT_EQ(slotU32(ImGuiCol_ButtonActive), t.surfaceActive);
  EXPECT_EQ(slotU32(ImGuiCol_Header), t.surfaceActive);
  EXPECT_EQ(slotU32(ImGuiCol_HeaderHovered), t.surfaceHover);
  EXPECT_EQ(slotU32(ImGuiCol_Border), t.borderSubtle);
  EXPECT_EQ(slotU32(ImGuiCol_Separator), t.borderSubtle);
  EXPECT_EQ(slotU32(ImGuiCol_Text), t.textPrimary);
  EXPECT_EQ(slotU32(ImGuiCol_TextDisabled), t.textDisabled);
  EXPECT_EQ(slotU32(ImGuiCol_CheckMark), t.accentDefault);
  EXPECT_EQ(slotU32(ImGuiCol_SliderGrab), t.accentDefault);
  EXPECT_EQ(slotU32(ImGuiCol_SliderGrabActive), t.accentHover);
  EXPECT_EQ(slotU32(ImGuiCol_NavHighlight), t.accentDefault);
  EXPECT_EQ(slotU32(ImGuiCol_ScrollbarBg), t.surfaceSunken);
  EXPECT_EQ(slotU32(ImGuiCol_ScrollbarGrab), t.surfaceHover);

  EXPECT_FLOAT_EQ(style.FrameRounding, 4.0f);
  EXPECT_FLOAT_EQ(style.GrabRounding, 4.0f);
  EXPECT_FLOAT_EQ(style.WindowRounding, 6.0f);
  EXPECT_FLOAT_EQ(style.PopupRounding, 6.0f);
  EXPECT_FLOAT_EQ(style.FramePadding.x, 8.0f);
  EXPECT_FLOAT_EQ(style.FramePadding.y, 4.0f);
  EXPECT_FLOAT_EQ(style.ItemSpacing.x, 8.0f);
  EXPECT_FLOAT_EQ(style.ItemSpacing.y, 6.0f);
  EXPECT_FLOAT_EQ(style.WindowPadding.x, 8.0f);
  EXPECT_FLOAT_EQ(style.WindowPadding.y, 8.0f);
  EXPECT_FLOAT_EQ(style.IndentSpacing, 16.0f);
  EXPECT_FLOAT_EQ(style.ScrollbarSize, 12.0f);
}

// applyToImGuiStyle publishes the active theme for raw-draw widgets.
TEST(EditorTheme, ApplyPublishesActiveTheme) {
  const EditorTheme teal = EditorTheme::Dark(Accent::SignalTeal);
  ImGuiStyle style;
  teal.applyToImGuiStyle(style);
  EXPECT_EQ(EditorTheme::Active().accentDefault, teal.accentDefault);

  const EditorTheme violet = EditorTheme::Dark(Accent::UltraViolet);
  EditorTheme::SetActive(violet);
  EXPECT_EQ(EditorTheme::Active().accentDefault, violet.accentDefault);

  // Restore the shipped default so test ordering cannot leak state.
  EditorTheme::SetActive(EditorTheme::Dark(Accent::SignalTeal));
}

}  // namespace donner::editor
