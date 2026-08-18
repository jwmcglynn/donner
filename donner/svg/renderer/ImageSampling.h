#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "donner/base/Transform.h"
#include "donner/svg/core/ImageRendering.h"

namespace donner::svg {

/// Maximum axis of a materialized image-sampling surface.
inline constexpr std::int64_t kMaxImageSamplingDimension = 16384;
/// Maximum pixels in one materialized image-sampling surface.
inline constexpr std::size_t kMaxImageSamplingSurfacePixels = 16 * 1024 * 1024;

/**
 * Rasterizes a premultiplied image through an affine transform into a bounded device surface.
 *
 * @param premultipliedPixels Tightly packed premultiplied RGBA8 source pixels.
 * @param sourceWidth Source width in pixels.
 * @param sourceHeight Source height in pixels.
 * @param deviceFromImage Transform from source-pixel coordinates to output pixels.
 * @param outputWidth Output width in pixels.
 * @param outputHeight Output height in pixels.
 * @param imageRendering Resolved image sampling policy.
 * @return Tightly packed premultiplied RGBA8 output, or empty when inputs exceed the surface
 * budget.
 */
std::vector<std::uint8_t> RasterizeImagePremultiplied(
    std::span<const std::uint8_t> premultipliedPixels, int sourceWidth, int sourceHeight,
    const Transform2d& deviceFromImage, int outputWidth, int outputHeight,
    ImageRendering imageRendering);

}  // namespace donner::svg
