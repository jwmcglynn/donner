#pragma once
/// @file
///
/// Toolbar tool icons, rendered from embedded two-tone SVG art through Donner.
/// Black cores and white halos are preserved so the toolbar and OS cursors use
/// one contrast system on every surface (see `donner/editor/art/STYLE.md`).

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
  Select,  ///< Selection / arrow tool.
  Pen,     ///< Pen (path) tool.
  Text,    ///< Type / text tool.
};

/// Every toolbar icon, in palette order. The single source of truth for the
/// icon registry and its coverage test.
inline constexpr std::array<ToolbarIcon, 3> kToolbarIcons = {
    ToolbarIcon::Select,
    ToolbarIcon::Pen,
    ToolbarIcon::Text,
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

/// The Donner-rendered two-tone bitmap for @p icon, rendered once and cached
/// for the process lifetime. `std::nullopt` if parsing/rendering failed.
[[nodiscard]] const std::optional<svg::RendererBitmap>& CachedToolbarIconBitmap(ToolbarIcon icon);

/// Draw @p icon centered within the button rect `[min, max]`, tinted with
/// @p tintColor. No-op when @p provider is null or the icon can't be uploaded
/// (e.g. headless tests with no GL context), leaving the button blank rather
/// than crashing.
void DrawToolbarIcon(ToolbarIcon icon, const ImVec2& min, const ImVec2& max, ImU32 tintColor,
                     const ToolbarIconTextureProvider& provider);

}  // namespace donner::editor
