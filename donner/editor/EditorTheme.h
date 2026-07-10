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

/// Accent hue variants from design doc 0054. The editor ships SignalTeal
/// (operator-approved variant B); the others are retained so the token table
/// and its tests cover every accent and a future re-tint is a one-line change.
enum class Accent {
  AzimuthBlue,  ///< Variant A: #4C8DF6, classic / calm.
  SignalTeal,   ///< Variant B: #31C6B3, highest contrast (shipped default).
  UltraViolet,  ///< Variant C: #9A7CF7, warmer / creative.
};

/// Number of `Accent` values, for token-completeness tests and iteration.
inline constexpr int kAccentCount = 3;

/// Named design tokens for the editor chrome (design doc 0054, Graphite).
struct EditorTheme {
  // --- Surfaces: neutral graphite ramp, deepest to lightest ---
  ImU32 surfaceCanvas;   ///< #111215 artboard letterbox / deepest backdrop.
  ImU32 surfaceSunken;   ///< #151619 scroll troughs, wells, inset regions.
  ImU32 surfaceBase;     ///< #1B1D20 panels, sidebar, primary window bg.
  ImU32 surfaceRaised;   ///< #24272B toolbar, menu bar, titlebars, field idle.
  ImU32 surfaceOverlay;  ///< #2B2F34 popovers, dropdowns, tooltips, chips.
  ImU32 surfaceHover;    ///< #343940 row / button hover.
  ImU32 surfaceActive;   ///< #3C424A pressed / selected row background.

  // --- Borders ---
  ImU32 borderSubtle;  ///< #30343A hairline dividers, panel edges.
  ImU32 borderStrong;  ///< #464C55 field outlines, focused container.

  // --- Text (on surfaceBase) ---
  ImU32 textPrimary;   ///< #F1F2F4 body text, values (AAA).
  ImU32 textMuted;     ///< #A8ADB5 secondary labels, meta (AA).
  ImU32 textDisabled;  ///< #656B74 disabled controls only (intentionally sub-AA).

  // --- Accent: the chosen variant, three tints + dark ink ---
  ImU32 accentActive;   ///< Darker, pressed.
  ImU32 accentDefault;  ///< Base accent (foreground icons, focus ring, links).
  ImU32 accentHover;    ///< Lighter, hover.
  ImU32 accentInk;      ///< #111215 dark ink for labels ON accent fills.

  // --- Selection: derived from the accent ---
  ImU32 selectionStroke;     ///< = accentDefault, opaque; marquee / handle stroke.
  float selectionFillAlpha;  ///< 0.22 fill alpha (accent-at-22%, not solid fill).

  // --- Semantic states ---
  ImU32 warning;      ///< #E3B341 amber: promote-refused, over-budget frames.
  ImU32 destructive;  ///< #F0616A red: delete, errors, stalls.
  ImU32 success;      ///< #3FB984 green: committed / in-budget.

  // --- Spacing grid (logical px, multiples of 4) ---
  float space1{4.0f};
  float space2{8.0f};
  float space3{12.0f};
  float space4{16.0f};
  float space6{24.0f};
  float space8{32.0f};

  // --- Rounding (logical px) ---
  float radiusControl{4.0f};    ///< Buttons, fields, toggles, swatches, chips.
  float radiusContainer{6.0f};  ///< Panels, cards, popovers, tooltips.

  // --- Control metrics (logical px) ---
  float toolButtonSize{32.0f};
  float treeRowHeight{24.0f};
  float scrollbarSize{12.0f};

  // --- The accent as a Donner css::RGBA, for the overlay's css color seam ---
  /// The overlay/selection chrome draws through Donner's own `css::Color`
  /// path, not ImGui packing, so it reads the accent from here rather than
  /// unpacking `selectionStroke`.
  css::RGBA accentColor;

  /// Build the dark theme for the given accent (design doc 0054 palette).
  /// Defaults to the shipped SignalTeal (variant B).
  static EditorTheme Dark(Accent accent = Accent::SignalTeal);

  /// The accent tinted to `alpha` (0-255) as a `css::RGBA`, for overlay
  /// stroke/fill draws that use Donner's css color type.
  css::RGBA selectionRgba(uint8_t alpha = 0xFF) const;

  /// Map these tokens onto an `ImGuiStyle` (colors + rounding / padding /
  /// spacing vars) and publish this theme as the process-wide active theme.
  /// Replaces `ImGui::StyleColorsDark()` at the editor's init site.
  void applyToImGuiStyle(ImGuiStyle& style) const;

  /// The process-wide active theme, published by `applyToImGuiStyle` so raw
  /// `ImDrawList` widgets read the same tokens without threading the struct
  /// through every call site. Defaults to `Dark(SignalTeal)` before any apply.
  static const EditorTheme& Active();

  /// Publish `theme` as the active theme (called by `applyToImGuiStyle`).
  static void SetActive(const EditorTheme& theme);
};

/// Returns `color` with its alpha channel replaced by `alpha` (ImGui packing).
/// Lets raw `ImDrawList` chrome reuse an opaque theme token at reduced opacity
/// (e.g. translucent scrollbar rails and info chips).
ImU32 WithAlpha(ImU32 color, uint8_t alpha);

}  // namespace donner::editor
