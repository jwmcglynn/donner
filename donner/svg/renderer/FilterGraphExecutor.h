#pragma once
/// @file

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "tiny_skia/Pixmap.h"

namespace donner::svg {

/**
 * Converts straight-alpha RGBA bytes to premultiplied RGBA.
 *
 * @param rgbaPixels Straight-alpha RGBA pixel data.
 * @return Premultiplied RGBA bytes.
 */
[[nodiscard]] std::vector<std::uint8_t> PremultiplyRgba(std::span<const std::uint8_t> rgbaPixels);

/**
 * Applies a Donner filter graph to a premultiplied RGBA pixmap in place.
 *
 * @param pixmap Premultiplied RGBA pixmap containing SourceGraphic on entry.
 * @param filterGraph Filter graph to execute.
 * @param deviceFromFilter Transform from filter local coordinates to device coordinates.
 * @param filterRegion Optional filter region in filter local coordinates.
 */
void ApplyFilterGraphToPixmap(tiny_skia::Pixmap& pixmap, const components::FilterGraph& filterGraph,
                              const Transform2d& deviceFromFilter,
                              const std::optional<Box2d>& filterRegion,
                              bool clipSourceToFilterRegion = false,
                              const tiny_skia::Pixmap* fillPaintInput = nullptr,
                              const tiny_skia::Pixmap* strokePaintInput = nullptr);

/**
 * Clears pixels outside the transformed filter region.
 *
 * @param pixmap Premultiplied RGBA pixmap to modify in place.
 * @param filterRegion Optional filter region in filter local coordinates.
 * @param deviceFromFilter Transform from filter local coordinates to device coordinates.
 */
void ClipFilterOutputToRegion(tiny_skia::Pixmap& pixmap, const std::optional<Box2d>& filterRegion,
                              const Transform2d& deviceFromFilter);

}  // namespace donner::svg
