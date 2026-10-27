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
  Auto,             ///< [DEFAULT] UA-defined; Donner uses the Mitchell cubic, like resvg.
  OptimizeSpeed,    ///< Legacy alias with the same behavior as `crisp-edges`.
  OptimizeQuality,  ///< CSS Images aliases this to `smooth`; Donner follows resvg, which
                    ///< renders it with the Mitchell cubic.
  CrispEdges,       ///< Preserves contrast without blending source colors.
  Pixelated,        ///< Integer nearest-neighbor scale followed by smooth scaling.
  Smooth,           ///< Bilinear interpolation.
  HighQuality,      ///< High-quality smooth scaling; Donner samples it like `auto`.
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
