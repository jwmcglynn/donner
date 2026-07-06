#pragma once
/// @file
///
/// Text-region "rope" chip glyph icons, rendered from embedded SVG art through
/// Donner's `EmbeddedSvgIcon` white-mask pipeline. The focus-reference rope
/// chips used to draw their marks as Unicode font glyphs ("✦" / "✱") that the
/// editor's embedded fonts (Roboto, Fira Code) do not contain, so they rendered
/// as the missing-glyph "?" placeholder (QA-F22). These are real SVG icons in
/// the shared editor art family (see `donner/editor/art/STYLE.md`), tinted with
/// the chip/rope color like the other affordance icons.

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/ToolbarIconSet.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

/// A text-region chip mark that has an SVG icon. The single source of truth for
/// the chip-icon registry and its coverage test.
enum class TextChipIcon : std::uint8_t {
  StyleSource,  ///< Style-source chip mark (was the "✦" glyph).
  Overflow,     ///< Overflow-count marker (was the "✱" glyph).
};

/// Every text-region chip icon. Iterated by the registry-coverage test, which
/// fails if any known chip mark lacks real SVG art (a regression back to the
/// "?" placeholder fallback).
inline constexpr std::array<TextChipIcon, 2> kTextChipIcons = {
    TextChipIcon::StyleSource,
    TextChipIcon::Overflow,
};

/// Embedded SVG source bytes for @p icon.
[[nodiscard]] std::span<const unsigned char> TextChipIconSvg(TextChipIcon icon);

/// Stable texture-cache key for @p icon.
[[nodiscard]] std::uint64_t TextChipIconTextureKey(TextChipIcon icon);

/// The Donner-rendered white tintable mask bitmap for @p icon, rendered once
/// and cached for the process lifetime. `std::nullopt` if parsing/rendering
/// failed.
[[nodiscard]] const std::optional<svg::RendererBitmap>& CachedTextChipIconBitmap(TextChipIcon icon);

/// Draw @p icon into the rect `[min, max]` of @p drawList, tinted with
/// @p tintColor (the rope/chip color). Reuses the toolbar icon texture-upload
/// seam. Returns true if the icon was drawn; false when @p provider is null or
/// the icon can't be uploaded (e.g. headless tests with no GL context), so the
/// caller can fall back to a text glyph.
[[nodiscard]] bool DrawTextChipIcon(ImDrawList* drawList, TextChipIcon icon, const ImVec2& min,
                                    const ImVec2& max, ImU32 tintColor,
                                    const ToolbarIconTextureProvider& provider);

}  // namespace donner::editor
