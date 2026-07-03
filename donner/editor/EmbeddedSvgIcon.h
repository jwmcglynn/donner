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

}  // namespace donner::editor
