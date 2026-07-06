#pragma once
/// @file

#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * Values for the `vector-effect` property, which modifies how an element is affected by the
 * coordinate system transformations in effect (such as viewBox scaling and `transform`).
 *
 * Only `non-scaling-stroke` is honored during rendering. The remaining values are at-risk in the
 * SVG 2 Candidate Recommendation; they parse successfully but fall back to `none` behavior.
 *
 * @see https://www.w3.org/TR/SVG2/coords.html#VectorEffects
 */
enum class VectorEffect : uint8_t {
  None,  ///< [DEFAULT] No vector effect; the element scales normally with the coordinate system.
  NonScalingStroke,  ///< The stroke width (and dash pattern) is held constant in the coordinate
                     ///< system of the referencing viewport, ignoring the element's own transform
                     ///< and viewBox scaling.
  NonScalingSize,  ///< At-risk in SVG 2 CR. Parses but is treated as `none`.
  NonRotation,     ///< At-risk in SVG 2 CR. Parses but is treated as `none`.
  FixedPosition,   ///< At-risk in SVG 2 CR. Parses but is treated as `none`.
};

/// Output stream operator for \ref VectorEffect, outputs the CSS string representation for this
/// enum, e.g. "none", "non-scaling-stroke", etc.
inline std::ostream& operator<<(std::ostream& os, VectorEffect value) {
  switch (value) {
    case VectorEffect::None: return os << "none";
    case VectorEffect::NonScalingStroke: return os << "non-scaling-stroke";
    case VectorEffect::NonScalingSize: return os << "non-scaling-size";
    case VectorEffect::NonRotation: return os << "non-rotation";
    case VectorEffect::FixedPosition: return os << "fixed-position";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

}  // namespace donner::svg
