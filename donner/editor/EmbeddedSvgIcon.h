#pragma once
/// @file

#include <optional>
#include <span>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

/**
 * Use an editor-owned renderer for subsequent embedded-SVG rasterization.
 *
 * The editor calls this before its first UI frame so icon rendering reuses the
 * already-created backend/device instead of creating a second headless device.
 * The renderer must outlive the matching reset call.
 */
void ConfigureEmbeddedSvgIconRenderer(svg::RendererInterface& renderer);

/**
 * Stop using the configured renderer before its owner is destroyed.
 *
 * The reset is ignored when another editor has since installed a different
 * renderer, which keeps overlapping editor lifetimes safe.
 */
void ResetEmbeddedSvgIconRenderer(const svg::RendererInterface& expectedRenderer);

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
 * Render embedded SVG artwork while preserving its authored RGBA colors.
 *
 * Use this for small multi-color assets such as the editor's black-core,
 * white-halo tool icons. Single-color affordances should continue to use
 * \ref RenderEmbeddedSvgIcon so ImGui can tint their alpha mask.
 *
 * @param svgBytes Embedded SVG source bytes.
 * @param outputSizePx Square output bitmap size in device pixels.
 * @return Rendered artwork, or `std::nullopt` if parsing/rendering fails.
 */
[[nodiscard]] std::optional<svg::RendererBitmap> RenderEmbeddedSvgArtwork(
    std::span<const unsigned char> svgBytes, int outputSizePx);

}  // namespace donner::editor
