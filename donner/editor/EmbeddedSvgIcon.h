#pragma once
/// @file

#include <optional>
#include <span>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

/**
 * Render an embedded SVG icon resource into a tintable RGBA bitmap.
 *
 * The source SVG is parsed and rasterized through Donner. The returned bitmap
 * is normalized to a white premultiplied-alpha mask so callers can tint it with
 * the current ImGui text color when displaying the resulting texture.
 *
 * @param svgBytes Embedded SVG source bytes.
 * @param outputSizePx Square output bitmap size in device pixels.
 * @return Rendered icon bitmap, or `std::nullopt` if parsing/rendering fails.
 */
[[nodiscard]] std::optional<svg::RendererBitmap> RenderEmbeddedSvgIcon(
    std::span<const unsigned char> svgBytes, int outputSizePx);

/**
 * Render an embedded SVG icon resource into a full-color RGBA bitmap.
 *
 * Unlike `RenderEmbeddedSvgIcon`, the source SVG's authored colors are
 * preserved (no collapse to a tintable white mask). This is what lets a
 * two-tone icon (solid black core with a white outline) read on the toolbar the
 * same way it does as an OS cursor: the white outline carries its own contrast
 * on any button state, so the icon is drawn with an identity (white) tint
 * rather than recolored. See `donner/editor/art/STYLE.md` (QA-F7).
 *
 * @param svgBytes Embedded SVG source bytes.
 * @param outputSizePx Square output bitmap size in device pixels.
 * @return Rendered icon bitmap (premultiplied RGBA), or `std::nullopt` if
 *   parsing/rendering fails.
 */
[[nodiscard]] std::optional<svg::RendererBitmap> RenderEmbeddedSvgIconColor(
    std::span<const unsigned char> svgBytes, int outputSizePx);

}  // namespace donner::editor
