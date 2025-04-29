#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/properties/Property.h"
#include "donner/svg/properties/PropertyParsing.h"
#include "donner/svg/properties/RxRyProperties.h"  // IWYU pragma: keep, defines CalculateRadiusMaybeAuto

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_ellipse element.
 */
struct EllipseProperties {
  /// The center x-coordinate of the ellipse, defaults to 0.
  Property<Lengthd> cx{"cx",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  /// The center y-coordinate of the ellipse, defaults to 0.
  Property<Lengthd> cy{"cy",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  /// The x-radius of the ellipse, defaults to 0.
  Property<Lengthd> rx{"rx", []() -> std::optional<Lengthd> { return std::nullopt; }};
  /// The y-radius of the ellipse, defaults to 0.
  Property<Lengthd> ry{"ry", []() -> std::optional<Lengthd> { return std::nullopt; }};

  /// Get all properties as a tuple.
  auto allProperties() { return std::forward_as_tuple(cx, cy, rx, ry); }

  /**
   * Calculate the actual value of \ref rx in pixels, taking into account the viewBox and handling
   * "auto".
   *
   * @param viewBox The viewBox of the SVG document.
   * @param fontMetrics The font metrics of the SVG document.
   * @return The actual value of \ref rx in pixels.
   */
  std::tuple<Lengthd, double> calculateRx(const Boxd& viewBox,
                                          const FontMetrics& fontMetrics) const {
    return CalculateRadiusMaybeAuto(rx, ry, viewBox, fontMetrics);
  }

  /**
   * Calculate the actual value of \ref ry in pixels, taking into account the viewBox and handling
   * "auto".
   *
   * @param viewBox The viewBox of the SVG document.
   * @param fontMetrics The font metrics of the SVG document.
   * @return The actual value of \ref ry in pixels.
   */
  std::tuple<Lengthd, double> calculateRy(const Boxd& viewBox,
                                          const FontMetrics& fontMetrics) const {
    return CalculateRadiusMaybeAuto(ry, rx, viewBox, fontMetrics);
  }
};

/**
 * Computed properties for a \ref xml_ellipse element, which applies values from the CSS cascade.
 */
struct ComputedEllipseComponent {
  /**
   * Construct a \ref ComputedEllipseComponent from the input properties and unparsed properties.
   *
   * @param inputProperties The input properties for the ellipse.
   * @param unparsedProperties The unparsed properties for the ellipse, which may contain
   * presentation attributes set in the CSS stylesheet which need to be applied to the element.
   * @param outWarnings A vector to append any warnings to.
   */
  ComputedEllipseComponent(const EllipseProperties& inputProperties,
                           const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
                           std::vector<ParseError>* outWarnings);

  /// The computed properties for the ellipse.
  EllipseProperties properties;
};

/**
 * Parameters for a \ref xml_ellipse element.
 */
struct EllipseComponent {
  /// The properties of the ellipse.
  EllipseProperties properties;
};

}  // namespace donner::svg::components
