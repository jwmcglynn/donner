#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * CSS `image-rendering` property values. Controls the sampling filter used when
 * a raster image is scaled.
 *
 * @see https://drafts.csswg.org/css-images/#the-image-rendering
 */
enum class ImageRendering : uint8_t {
  Auto,         ///< [DEFAULT] UA picks the algorithm; typically bilinear/trilinear.
  OptimizeSpeed,///< Legacy SVG 1.1 alias; treated as `pixelated` here.
  OptimizeQuality, ///< Legacy SVG 1.1 alias; treated as `auto`.
  CrispEdges,   ///< Preserves sharp edges; treated as `pixelated` for nearest-neighbor.
  Pixelated,    ///< Nearest-neighbor sampling for blocky upscaling.
  Smooth,       ///< Bilinear/trilinear (same as `auto`).
};

/// ostream output operator for \ref ImageRendering.
inline std::ostream& operator<<(std::ostream& os, ImageRendering value) {
  switch (value) {
    case ImageRendering::Auto: return os << "auto";
    case ImageRendering::OptimizeSpeed: return os << "optimizeSpeed";
    case ImageRendering::OptimizeQuality: return os << "optimizeQuality";
    case ImageRendering::CrispEdges: return os << "crisp-edges";
    case ImageRendering::Pixelated: return os << "pixelated";
    case ImageRendering::Smooth: return os << "smooth";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
