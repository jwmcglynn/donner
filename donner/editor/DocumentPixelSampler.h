#pragma once
/// @file
/// Checked access to document-only RGBA pixels captured by the render worker.

#include <cstddef>
#include <optional>

#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/editor/ViewportState.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor {

/// Maximum retained CPU pixel payload for one eyedropper capture.
inline constexpr std::size_t kDocumentPixelCaptureMaxBytes = 256u * 1024u * 1024u;

/// Return whether the planned RGBA8 capture fits after GPU row alignment.
[[nodiscard]] bool CanCaptureDocumentPixelSize(const Vector2i& dimensions) noexcept;

/// Return whether a bitmap has complete RGBA8 rows within the capture limit.
[[nodiscard]] bool IsValidDocumentPixelBitmap(const svg::RendererBitmap& bitmap) noexcept;

/**
 * Map one logical screen point to a captured document pixel.
 *
 * @param viewport Viewport used to present the document.
 * @param rasterViewport Raster mapping that produced the bitmap.
 * @param screenPoint Pointer position in logical screen pixels.
 * @return Pixel index, or nothing outside the document, pane, or raster.
 */
[[nodiscard]] std::optional<Vector2i> DocumentPixelIndexAtScreenPoint(
    const ViewportState& viewport, const EditorRasterViewport& rasterViewport,
    const Vector2d& screenPoint) noexcept;

/**
 * Read a straight-alpha SVG color from a captured pixel.
 *
 * @param bitmap Complete RGBA8 renderer bitmap.
 * @param pixel Integer pixel index in the bitmap.
 * @return Color, or nothing when the bitmap or index is invalid.
 */
[[nodiscard]] std::optional<css::RGBA> ReadDocumentPixel(const svg::RendererBitmap& bitmap,
                                                         const Vector2i& pixel) noexcept;

}  // namespace donner::editor
