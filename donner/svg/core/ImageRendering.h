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
  Auto,             ///< [DEFAULT] UA picks the algorithm; typically bilinear/trilinear.
  OptimizeSpeed,    ///< Legacy alias with the same behavior as `crisp-edges`.
  OptimizeQuality,  ///< Legacy alias with the same behavior as `smooth`.
  CrispEdges,       ///< Preserves contrast without blending source colors.
  Pixelated,        ///< Integer nearest-neighbor scale followed by smooth scaling.
  Smooth,           ///< Bilinear/trilinear (same as `auto`).
  HighQuality,      ///< Smooth scaling retained as a distinct author preference.
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
    case ImageRendering::HighQuality: return os << "high-quality";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
