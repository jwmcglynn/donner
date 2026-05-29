#pragma once
/// @file

#include <array>
#include <cstdint>
#include <ostream>

#include "donner/base/Utils.h"

namespace donner::svg {

/**
 * One of the three paint operations whose draw order is controlled by the CSS
 * `paint-order` property.
 *
 * @see https://www.w3.org/TR/SVG2/painting.html#PaintOrder
 */
enum class PaintComponent : uint8_t {
  Fill,     ///< The fill of a shape or glyph.
  Stroke,   ///< The stroke of a shape or glyph.
  Markers,  ///< The markers drawn along a shape's vertices.
};

/// ostream output operator for \ref PaintComponent.
inline std::ostream& operator<<(std::ostream& os, PaintComponent value) {
  switch (value) {
    case PaintComponent::Fill: return os << "fill";
    case PaintComponent::Stroke: return os << "stroke";
    case PaintComponent::Markers: return os << "markers";
  }

  UTILS_UNREACHABLE();  // LCOV_EXCL_LINE
}

/**
 * CSS `paint-order` property value: the order in which the fill, stroke, and
 * markers of a shape/text are painted.
 *
 * Per the spec, fewer than three keywords fills the remainder in the canonical
 * order (fill, stroke, markers), and `normal` is equivalent to `fill stroke
 * markers`.
 *
 * @see https://www.w3.org/TR/SVG2/painting.html#PaintOrder
 */
struct PaintOrder {
  /// The painting order. Always a permutation of {Fill, Stroke, Markers}.
  std::array<PaintComponent, 3> order = {PaintComponent::Fill, PaintComponent::Stroke,
                                         PaintComponent::Markers};

  /// Equality operator.
  bool operator==(const PaintOrder& other) const = default;
};

/// ostream output operator for \ref PaintOrder.
inline std::ostream& operator<<(std::ostream& os, const PaintOrder& value) {
  os << "PaintOrder(" << value.order[0] << " " << value.order[1] << " " << value.order[2] << ")";
  return os;
}

}  // namespace donner::svg
