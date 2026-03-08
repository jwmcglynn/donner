#pragma once

#include <cstdint>

#include "tiny_skia/Pixmap.h"

namespace tiny_skia::filter {

/// Channel selector for feDisplacementMap.
enum class DisplacementChannel : std::uint8_t { R, G, B, A };

/**
 * Apply displacement mapping: displaces pixels from \p src using channel values from \p map.
 *
 * For each pixel (x, y):
 *   dst[x, y] = src[x + scale * (map[x,y].xCh/255.0 - 0.5),
 *                    y + scale * (map[x,y].yCh/255.0 - 0.5)]
 *
 * Source sampling uses bilinear interpolation on premultiplied RGBA.
 * Out-of-bounds samples produce transparent black.
 *
 * @param src Source pixmap (the image to displace).
 * @param map Displacement map pixmap (same size as src).
 * @param dst Destination pixmap (same size as src).
 * @param scale Displacement scale factor.
 * @param xCh Channel from map to use for X displacement.
 * @param yCh Channel from map to use for Y displacement.
 */
void displacementMap(const Pixmap& src, const Pixmap& map, Pixmap& dst, double scale,
                     DisplacementChannel xCh, DisplacementChannel yCh);

}  // namespace tiny_skia::filter
