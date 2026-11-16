#pragma once
/// @file
/// Bilinear texel mapping that a whole-pixel move of the target or the source cannot change.

#include <array>
#include <optional>

#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"

namespace donner::geode {

/**
 * Maps a target pixel position p to source texel coordinates as
 * `texel = linear * (p - anchor) + fraction + whole`, where `linear` is the single-precision
 * linear part of the inverse transform, `fraction` lies in [0, 1) and `whole` is an integer texel
 * offset. The shader evaluates only `linear * (p - anchor) + fraction` in single precision; the
 * whole part is added as an integer.
 */
struct AnchoredTexelRows {
  /// x and y terms of the texel x coordinate, then of the texel y coordinate.
  std::array<float, 4> linear{};
  /// Fractional texel coordinate of the anchor, each component in [0, 1).
  std::array<float, 2> fraction{};
  /// Whole texel coordinate of the anchor.
  Vector2i whole = Vector2i::Zero();
  /// Target pixel the mapping is relative to.
  Vector2i anchor = Vector2i::Zero();
};

/**
 * Splits the texel mapping of \p targetFromTexel at \p anchor. The fraction is computed in double
 * precision from the anchor's exact texel coordinate, so moving the source by whole texels changes
 * only `whole`, and moving the target and the anchor together by whole pixels changes nothing.
 * Bilinear weights computed from these rows are then the same for corresponding pixels of two
 * renders that differ by such a move, which a texture coordinate interpolated across the quad, or a
 * single-precision transform of the absolute pixel position, does not guarantee.
 *
 * @param targetFromTexel Transform from source texel coordinates to target pixels.
 * @param anchor A target pixel that moves with the content the target pixels are aligned to.
 * @return The rows, or `std::nullopt` when the transform is not invertible or the anchor's texel
 *   coordinate does not fit a 32-bit integer.
 */
std::optional<AnchoredTexelRows> AnchoredTexelRowsFor(const Transform2d& targetFromTexel,
                                                      const Vector2i& anchor);

/**
 * The target pixel containing \p position, for use as an anchor that moves with content placed at
 * \p position. A position on a pixel boundary up to double rounding counts as that boundary's
 * pixel, so two placements a whole number of pixels apart select anchors the same distance apart.
 *
 * @param position Target-space position of a point fixed to the content.
 */
Vector2i AnchorPixelFor(const Vector2d& position);

}  // namespace donner::geode
