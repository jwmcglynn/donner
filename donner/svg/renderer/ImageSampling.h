#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

#include "donner/base/Transform.h"
#include "donner/base/Utils.h"
#include "donner/svg/core/ImageRendering.h"

namespace donner::svg {

/// Resampling kernel a backend applies when it scales a raster `<image>`.
enum class ImageSamplingKernel : std::uint8_t {
  Nearest,         ///< Nearest neighbor, for `crisp-edges` and its `optimizeSpeed` alias.
  Bilinear,        ///< Bilinear interpolation, for `smooth`.
  MitchellCubic,   ///< Mitchell-Netravali cubic (B = C = 1/3) on premultiplied sRGB texels.
  PixelatedSmooth  ///< Integer nearest-neighbor scale followed by bilinear, for `pixelated`.
};

/// ostream output operator for \ref ImageSamplingKernel.
inline std::ostream& operator<<(std::ostream& os, ImageSamplingKernel kernel) {
  switch (kernel) {
    case ImageSamplingKernel::Nearest: return os << "Nearest";
    case ImageSamplingKernel::Bilinear: return os << "Bilinear";
    case ImageSamplingKernel::MitchellCubic: return os << "MitchellCubic";
    case ImageSamplingKernel::PixelatedSmooth: return os << "PixelatedSmooth";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

/**
 * Selects the raster `<image>` resampling kernel for a resolved `image-rendering` value. Both
 * renderer backends map through this function, so they cannot drift apart.
 *
 * `auto` is UA-defined; Donner, like resvg, uses the Mitchell cubic, which is also the kernel
 * `feImage` applies, and gives `optimizeQuality` and `high-quality` the same quality kernel.
 *
 * @param imageRendering The resolved `image-rendering` value.
 */
constexpr ImageSamplingKernel ImageSamplingKernelFor(ImageRendering imageRendering) {
  switch (imageRendering) {
    case ImageRendering::CrispEdges:
    case ImageRendering::OptimizeSpeed: return ImageSamplingKernel::Nearest;
    case ImageRendering::Pixelated: return ImageSamplingKernel::PixelatedSmooth;
    case ImageRendering::Smooth: return ImageSamplingKernel::Bilinear;
    case ImageRendering::Auto:
    case ImageRendering::OptimizeQuality:
    case ImageRendering::HighQuality: return ImageSamplingKernel::MitchellCubic;
  }

  return ImageSamplingKernel::MitchellCubic;
}

/// Maximum output axis of a materialized procedural image-sampling surface.
inline constexpr std::int64_t kMaxImageSamplingDimension = 16384;
/// Maximum pixels in one materialized image-sampling surface.
inline constexpr std::size_t kMaxImageSamplingSurfacePixels = 16 * 1024 * 1024;

/**
 * Rasterizes a premultiplied image through an affine transform into a bounded device surface.
 *
 * @param premultipliedPixels Exact tightly packed premultiplied RGBA8 source payload.
 * @param sourceWidth Source width in pixels.
 * @param sourceHeight Source height in pixels.
 * @param deviceFromImage Transform from source-pixel coordinates to output pixels.
 * @param outputWidth Output width in pixels.
 * @param outputHeight Output height in pixels.
 * @param imageRendering Resolved image sampling policy.
 * @return Tightly packed premultiplied RGBA8 output. Malformed payloads, invalid dimensions, and
 * over-budget surfaces return empty; invalid transforms return a correctly sized transparent
 * output.
 */
std::vector<std::uint8_t> RasterizeImagePremultiplied(
    std::span<const std::uint8_t> premultipliedPixels, int sourceWidth, int sourceHeight,
    const Transform2d& deviceFromImage, int outputWidth, int outputHeight,
    ImageRendering imageRendering);

}  // namespace donner::svg
