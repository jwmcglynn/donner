#pragma once
/// @file
///
/// Toolbar tool icons, rendered from embedded SVG art through Donner's
/// `EmbeddedSvgIcon` COLOR pipeline (`RenderEmbeddedSvgIconColor`). Unlike the
/// affordance icons in `LayersPanel` / `SidebarPresenter` (single-color tintable
/// masks), tool icons preserve their authored two-tone paint - a solid black
/// core with a white outline - so a tool's toolbar icon reads as the exact same
/// glyph as its OS cursor (QA-F7). The white outline carries its own contrast on
/// any button state, so the icon is drawn with an identity (white) tint rather
/// than recolored. This replaces the hand-drawn `ImDrawList` primitives the tool
/// palette used to stroke per frame; toolbar icons and OS cursors are authored
/// from one SVG art family (see `donner/editor/art/STYLE.md`).

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "donner/base/Vector2.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

/// A tool that has a toolbar icon. Mirrors the palette buttons in display
/// order; kept standalone so the registry-coverage test can iterate it without
/// a live ImGui context.
enum class ToolbarIcon : std::uint8_t {
  Select,      ///< Selection / arrow tool.
  Pen,         ///< Pen (path) tool.
  Text,        ///< Type / text tool.
  PathModify,  ///< Path-modify (anchor-point) tool.
};

/// Every toolbar icon, in palette order. The single source of truth for the
/// icon registry and its coverage test.
inline constexpr std::array<ToolbarIcon, 4> kToolbarIcons = {
    ToolbarIcon::Select,
    ToolbarIcon::Pen,
    ToolbarIcon::Text,
    ToolbarIcon::PathModify,
};

/// An uploaded icon texture plus the valid payload UV range (icon bitmaps may
/// be padded to a texture-friendly size).
struct ToolbarIconTexture {
  ImTextureID texture = 0;                      ///< ImGui texture handle.
  Vector2d uvBottomRight = Vector2d(1.0, 1.0);  ///< Bottom-right valid payload UV.
};

/// Uploads a Donner-rendered icon mask bitmap to an ImGui texture. Mirrors the
/// `IconTextureProvider` callbacks the layers/inspector panels already pass
/// down, so the shell reuses one texture-upload path.
using ToolbarIconTextureProvider =
    std::function<ToolbarIconTexture(std::uint64_t stableId, const svg::RendererBitmap& bitmap)>;

/// Embedded SVG source bytes for @p icon.
[[nodiscard]] std::span<const unsigned char> ToolbarIconSvg(ToolbarIcon icon);

/// Stable texture-cache key for @p icon.
[[nodiscard]] std::uint64_t ToolbarIconTextureKey(ToolbarIcon icon);

/// The Donner-rendered two-tone (black-core/white-outline) color bitmap for
/// @p icon, rendered once and cached for the process lifetime. `std::nullopt`
/// if parsing/rendering failed.
[[nodiscard]] const std::optional<svg::RendererBitmap>& CachedToolbarIconBitmap(ToolbarIcon icon);

/// Draw @p icon centered within the button rect `[min, max]`. The icon is a
/// two-tone glyph carrying its own contrast, so it is drawn with an identity
/// (white) tint rather than recolored. No-op when @p provider is null or the
/// icon can't be uploaded (e.g. headless tests with no GL context), leaving the
/// button blank rather than crashing.
void DrawToolbarIcon(ToolbarIcon icon, const ImVec2& min, const ImVec2& max,
                     const ToolbarIconTextureProvider& provider);

}  // namespace donner::editor
