#pragma once
/// @file
/// Shared tree-disclosure chevron used by both the inspector tree and the
/// LayersPanel so the two trees present one consistent disclosure style.
///
/// The chevron art is an editor-authored SVG (donner/editor/icons/chevron.svg),
/// Donner-rendered to a tintable white mask via `RenderEmbeddedSvgIcon`. The
/// same right-pointing mask is reused for the expanded state by rotating it a
/// quarter turn at draw time, so callers only manage a single texture.

#include <optional>

#include "donner/base/Vector2.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

/// Raster size (device px) for the shared chevron mask. Rendered once and
/// scaled down at draw time.
inline constexpr int kDisclosureChevronRasterSizePx = 32;

/// Cached white-mask bitmap for the (right-pointing, collapsed) chevron, or
/// `std::nullopt` if rendering failed. Rendered lazily on first use.
[[nodiscard]] const std::optional<svg::RendererBitmap>& CachedDisclosureChevronBitmap();

/// Quarter turns (clockwise) to apply to the right-pointing chevron for a given
/// disclosure state: 0 (points right) when collapsed, 1 (points down) when
/// expanded.
[[nodiscard]] int DisclosureChevronQuarterTurns(bool expanded);

/// Draw the chevron centered at @p center within a @p sizePx square, rotated for
/// @p expanded and tinted @p tint, from an already-uploaded @p texture whose
/// valid payload UV range is `[0, uvBottomRight]`.
void DrawDisclosureChevron(ImDrawList* drawList, ImTextureID texture, const Vector2d& uvBottomRight,
                           const ImVec2& center, float sizePx, bool expanded, ImU32 tint);

}  // namespace donner::editor
