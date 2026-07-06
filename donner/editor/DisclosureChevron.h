#pragma once
/// @file
/// Shared tree-disclosure chevron used by both the inspector tree and the
/// LayersPanel so the two trees present one consistent disclosure style.
///
/// The chevron art is editor-authored SVG (donner/editor/icons/chevron.svg and
/// chevron-down.svg), Donner-rendered to a tintable white mask via
/// `RenderEmbeddedSvgIcon`. Two axis-aligned assets (right for collapsed, down
/// for expanded) are used so the mask can be blitted with the sanctioned
/// `AddImage` path rather than a rotated quad.

#include <optional>

#include "donner/base/Vector2.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

/// Raster size (device px) for the shared chevron masks. Rendered once each and
/// scaled down at draw time.
inline constexpr int kDisclosureChevronRasterSizePx = 32;

/// Cached white-mask bitmap for the disclosure chevron in the given state
/// (right-pointing when collapsed, down-pointing when expanded), or
/// `std::nullopt` if rendering failed. Rendered lazily on first use.
[[nodiscard]] const std::optional<svg::RendererBitmap>& CachedDisclosureChevronBitmap(
    bool expanded);

/// Stable texture-cache key suffix (0 collapsed, 1 expanded) so callers upload
/// and reuse the two chevron masks as distinct textures.
[[nodiscard]] int DisclosureChevronTextureVariant(bool expanded);

/// Blit an already-uploaded chevron @p texture centered at @p center within a
/// @p sizePx square, tinted @p tint. The texture's valid payload UV range is
/// `[0, uvBottomRight]`.
void DrawDisclosureChevron(ImDrawList* drawList, ImTextureID texture, const Vector2d& uvBottomRight,
                           const ImVec2& center, float sizePx, ImU32 tint);

}  // namespace donner::editor
