#pragma once
/// @file

#include <map>
#include <optional>

#include "donner/base/Length.h"
#include "donner/svg/properties/Property.h"
#include "donner/svg/properties/PropertyParsing.h"
#include "donner/svg/properties/RxRyProperties.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_rect element.
 */
struct RectProperties {
  /// The x-coordinate of the rectangle, defaults to 0.
  Property<Lengthd> x{"x",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  /// The y-coordinate of the rectangle, defaults to 0.
  Property<Lengthd> y{"y",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  /// The width of the rectangle, defaults to 0.
  Property<Lengthd> width{
      "width", []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  /// The height of the rectangle, defaults to 0.
  Property<Lengthd> height{
      "height", []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  /// The corner radius of the rectangle, to enable creating rounded corners. Defaults to 0 (angled
  /// corners).
  Property<Lengthd> rx{"rx", []() -> std::optional<Lengthd> { return std::nullopt; }};
  /// The corner radius of the rectangle, to enable creating rounded corners. Defaults to 0 (angled
  /// corners).
  Property<Lengthd> ry{"ry", []() -> std::optional<Lengthd> { return std::nullopt; }};

  /// Get all properties as a tuple.
  auto allProperties() { return std::forward_as_tuple(x, y, width, height, rx, ry); }

  /**
   * Calculate the actual value of \ref rx in pixels, taking into account the viewbox and handling
   * "auto".
   *
   * @param viewbox The viewbox of the SVG document.
   * @param fontMetrics The font metrics of the SVG document.
   * @return The actual value of \ref rx in pixels.
   */
  std::tuple<Lengthd, double> calculateRx(const Boxd& viewbox,
                                          const FontMetrics& fontMetrics) const {
    return CalculateRadiusMaybeAuto(rx, ry, viewbox, fontMetrics);
  }

  /**
   * Calculate the actual value of \ref ry in pixels, taking into account the viewbox and handling
   * "auto".
   *
   * @param viewbox The viewbox of the SVG document.
   * @param fontMetrics The font metrics of the SVG document.
   * @return The actual value of \ref ry in pixels.
   */
  std::tuple<Lengthd, double> calculateRy(const Boxd& viewbox,
                                          const FontMetrics& fontMetrics) const {
    return CalculateRadiusMaybeAuto(ry, rx, viewbox, fontMetrics);
  }
};

/**
 * Computed properties for a \ref xml_rect element, which applies values from the CSS cascade.
 */
struct ComputedRectComponent {
  /**
   * Construct a \ref ComputedRectComponent from the input properties and unparsed properties.
   *
   * @param inputProperties The input properties for the rectangle.
   * @param unparsedProperties The unparsed properties for the rectangle, which may contain
   * presentation attributes set in the CSS stylesheet which need to be applied to the element.
   * @param outWarnings A vector to append any warnings to.
   */
  ComputedRectComponent(const RectProperties& inputProperties,
                        const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
                        std::vector<ParseError>* outWarnings);

  /// The computed properties for the rectangle.
  RectProperties properties;
};

/**
 * Parameters for a \ref xml_rect element.
 */
struct RectComponent {
  /// The properties of the rectangle.
  RectProperties properties;
};

}  // namespace donner::svg::components
