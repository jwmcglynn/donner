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
 * @param filterTransform Transform active when the filter layer was pushed.
 * @param filterRegion Optional filter region in filter local coordinates.
 */
void ApplyFilterGraphToPixmap(tiny_skia::Pixmap& pixmap, const components::FilterGraph& filterGraph,
                              const Transformd& filterTransform,
                              const std::optional<Boxd>& filterRegion);

/**
 * Clears pixels outside the transformed filter region.
 *
 * @param pixmap Premultiplied RGBA pixmap to modify in place.
 * @param filterRegion Optional filter region in filter local coordinates.
 * @param filterTransform Transform active when the filter layer was pushed.
 */
void ClipFilterOutputToRegion(tiny_skia::Pixmap& pixmap, const std::optional<Boxd>& filterRegion,
                              const Transformd& filterTransform);

}  // namespace donner::svg
