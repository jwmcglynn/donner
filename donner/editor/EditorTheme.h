#pragma once
/// @file
/// Donner editor design language (design doc 0054): a single source of truth
/// for the editor chrome's palette, spacing grid, rounding, and control
/// metrics. Replaces the scattered color literals and ad-hoc `PushStyleVar`
/// values that previously lived per-widget across the editor tree.
///
/// The tokens are ImGui-packed `ImU32` (built with `IM_COL32`) so both the
/// central `ImGuiStyle` and the raw `ImDrawList` draws (overlay chrome,
/// toolbar selection, chips) read the same values. Metrics are logical px;
/// callers multiply by `displayScale` exactly as they did before.

#include "donner/css/Color.h"
#include "donner/editor/ImGuiIncludes.h"

namespace donner::editor {

/// Accent hue variants from design doc 0054. QA-F8 (2026-07-06, operator round
/// 2) swapped the shipped default from SignalTeal to a new macOS-system-blue
/// variant, MacBlue, without disturbing the others: SignalTeal and
/// UltraViolet stay selectable, and AzimuthBlue is retained so the token
/// table and its tests cover every accent and a future re-tint is a one-line
/// change.
enum class Accent {
  AzimuthBlue,  ///< Variant A: #4C8DF6, classic / calm.
  SignalTeal,   ///< Variant B: #31C6B3, highest contrast (prior default).
  UltraViolet,  ///< Variant C: #9A7CF7, warmer / creative.
  MacBlue,      ///< Variant D: #0A84FF, macOS system blue (QA-F8 shipped default).
};

/// Number of `Accent` values, for token-completeness tests and iteration.
inline constexpr int kAccentCount = 4;

/// Named design tokens for the editor chrome (design doc 0054, Dark Slate;
/// surface/text/semantic hexes revised to the macOS-dark-mode-like palette
/// by QA-F8, 2026-07-06).
struct EditorTheme {
  // --- Surfaces: dark slate ramp, deepest to lightest ---
  // QA-F8 aligned base/raised/overlay/hover to Apple's dark-mode systemGray6
  // through systemGray3; canvas and active continue that ramp one step
  // darker / one step lighter (systemGray2) since QA-F8 did not specify them.
  ImU32 surfaceCanvas;   ///< #101012 artboard letterbox / deepest backdrop.
  ImU32 surfaceSunken;   ///< #161618 scroll troughs, wells, inset regions.
  ImU32 surfaceBase;     ///< #1C1C1E panels, sidebar, primary window bg (systemGray6).
  ImU32 surfaceRaised;   ///< #2C2C2E toolbar, menu bar, titlebars, field idle (systemGray5).
  ImU32 surfaceOverlay;  ///< #3A3A3C popovers, dropdowns, tooltips, chips (systemGray4).
  ImU32 surfaceHover;    ///< #48484A row / button hover (systemGray3).
  ImU32 surfaceActive;   ///< #636366 pressed / selected row background (systemGray2).

  // --- Borders ---
  // Not itemized by QA-F8; mapped onto the same systemGray ramp as the
  // surfaces they divide (raised/overlay) rather than left at the old hexes.
  ImU32 borderSubtle;  ///< #2C2C2E hairline dividers, panel edges (systemGray5).
  ImU32 borderStrong;  ///< #3A3A3C field outlines, focused container (systemGray4).

  // --- Text (on surfaceBase) ---
  ImU32 textPrimary;   ///< #FFFFFF body text, values (Apple labelColor dark).
  ImU32 textMuted;     ///< #EBEBF5 at 60% alpha secondary labels, meta (Apple secondaryLabelColor dark).
  ImU32 textDisabled;  ///< #EBEBF5 at 30% alpha disabled controls (Apple tertiaryLabelColor dark; intentionally sub-AA).

  // --- Accent: the chosen variant, three tints + dark ink ---
  ImU32 accentActive;   ///< Darker, pressed.
  ImU32 accentDefault;  ///< Base accent (foreground icons, focus ring, links).
  ImU32 accentHover;    ///< Lighter, hover.
  ImU32 accentInk;      ///< Dark ink for labels ON accent fills; matches surfaceCanvas.

  // --- Selection: derived from the accent ---
  ImU32 selectionStroke;     ///< = accentDefault, opaque; marquee / handle stroke.
  float selectionFillAlpha;  ///< 0.22 fill alpha (accent-at-22%, not solid fill).

  // --- Semantic states ---
  ImU32 warning;      ///< #FFD60A amber: promote-refused, over-budget frames.
  ImU32 destructive;  ///< #FF453A red: delete, errors, stalls.
  ImU32 success;      ///< #32D74B green: committed / in-budget.

  // --- Spacing grid (logical px, multiples of 4) ---
  float space1{4.0f};
  float space2{8.0f};
  float space3{12.0f};
  float space4{16.0f};
  float space6{24.0f};
  float space8{32.0f};

  // --- Rounding (logical px) ---
  float radiusControl{4.0f};    ///< Buttons, fields, toggles, swatches, chips.
  float radiusContainer{8.0f};  ///< Panels, cards, popovers, tooltips (QA-F8: nudged
                                 ///< up from 6 for a rounder macOS-panel feel).

  // --- Control metrics (logical px) ---
  float toolButtonSize{30.0f};
  float treeRowHeight{24.0f};
  float scrollbarSize{12.0f};

  // --- The accent as a Donner css::RGBA, for the overlay's css color seam ---
  /// The overlay/selection chrome draws through Donner's own `css::Color`
  /// path, not ImGui packing, so it reads the accent from here rather than
  /// unpacking `selectionStroke`.
  css::RGBA accentColor;

  /// Build the dark theme for the given accent (design doc 0054 palette,
  /// macOS-dark-mode-like revision per QA-F8). Defaults to the shipped
  /// MacBlue (variant D).
  static EditorTheme Dark(Accent accent = Accent::MacBlue);

  /// The accent tinted to `alpha` (0-255) as a `css::RGBA`, for overlay
  /// stroke/fill draws that use Donner's css color type.
  css::RGBA selectionRgba(uint8_t alpha = 0xFF) const;

  /// Map these tokens onto an `ImGuiStyle` (colors + rounding / padding /
  /// spacing vars) and publish this theme as the process-wide active theme.
  /// Replaces `ImGui::StyleColorsDark()` at the editor's init site.
  void applyToImGuiStyle(ImGuiStyle& style) const;

  /// The process-wide active theme, published by `applyToImGuiStyle` so raw
  /// `ImDrawList` widgets read the same tokens without threading the struct
  /// through every call site. Defaults to `Dark(MacBlue)` before any apply.
  static const EditorTheme& Active();

  /// Publish `theme` as the active theme (called by `applyToImGuiStyle`).
  static void SetActive(const EditorTheme& theme);
};

/// Returns `color` with its alpha channel replaced by `alpha` (ImGui packing).
/// Lets raw `ImDrawList` chrome reuse an opaque theme token at reduced opacity
/// (e.g. translucent scrollbar rails and info chips).
ImU32 WithAlpha(ImU32 color, uint8_t alpha);

}  // namespace donner::editor
