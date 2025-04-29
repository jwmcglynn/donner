#pragma once
/// @file

#include <optional>
#include <tuple>

#include "donner/base/Box.h"
#include "donner/base/Length.h"
#include "donner/svg/properties/Property.h"

namespace donner::svg::components {

/**
 * Stores an offset/size for elements that are positioned with `x`/`y`/`width`/`height` attributes
 * with respect to their parent. Used for \ref xml_svg, \ref xml_image and \ref xml_foreignObject by
 * the standard, and also internally with \ref xml_use for Donner.
 *
 * If not specified, `x`/`y` default to 0, and `width`/`height` are `std::nullopt`.
 */
struct SizedElementProperties {
  /// The x-coordinate of the element, defaults to 0.
  Property<Lengthd> x{"x",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  /// The y-coordinate of the element, defaults to 0.
  Property<Lengthd> y{"y",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  /// The width of the element, defaults to none.
  Property<Lengthd> width{"width", []() -> std::optional<Lengthd> { return std::nullopt; }};

  /// The height of the element, defaults to none.
  Property<Lengthd> height{"height", []() -> std::optional<Lengthd> { return std::nullopt; }};

  /// Get all properties as a tuple.
  auto allProperties() { return std::forward_as_tuple(x, y, width, height); }
};

/**
 * Stores the computed bounds of a sized element, resolving units and percentages. Contains the
 * computed rect and inherited viewBox of the parent element.
 */
struct ComputedSizedElementComponent {
  Boxd bounds;            ///< The computed rect of this sized element.
  Boxd inheritedViewBox;  ///< The viewBox of the parent element, used for preserveAspectRatio
                          ///< transformations.
};

/**
 * Stores the properties of a sized element, `x`, `y`, `width`, `height`. Used for \ref xml_svg,
 * \ref xml_image and \ref xml_foreignObject by the standard, and also internally with \ref xml_use
 * for Donner.
 */
struct SizedElementComponent {
  /// The properties of the sized element, `x`, `y`, `width`, `height`.
  SizedElementProperties properties;
};

}  // namespace donner::svg::components
