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
 * Stores the properties of a sized element, `x`, `y`, `width`, `height`. Used for \ref xml_svg,
 * \ref xml_image and \ref xml_foreignObject by the standard, and also internally with \ref xml_use
 * for Donner.
 */
struct SizedElementComponent {
  /// The properties of the sized element, `x`, `y`, `width`, `height`.
  SizedElementProperties properties;
  /// Set to true for \ref xml_use elements, so that `x`/`y` are applied as a translation.
  bool applyTranslationForUseElement = false;
  /// Set to true for \ref xml_symbol elements, so that `width`/`height` are inherited from the \ref
  /// xml_use element.
  bool canOverrideWidthHeightForSymbol = false;
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
 * Stores a shadow tree's computed SizedElementComponent, where a \ref xml_use element overrides the
 * width or height on \ref xml_symbol or \ref xml_svg which use \ref SizedElementComponent.
 *
 * From https://www.w3.org/TR/SVG2/struct.html#UseElement:
 * > The width and height attributes only have an effect if the referenced element defines a viewport
 * > (i.e., if it is a 'svg' or 'symbol'); if so, a value other than auto for the 'use' element
 * > overrides the value of the corresponding geometric property on that element.
 *
 */
struct ComputedShadowSizedElementComponent {
  Boxd bounds;  ///< The computed rect of this sized element.
};

}  // namespace donner::svg::components
