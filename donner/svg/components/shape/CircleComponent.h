#pragma once
/// @file

#include "donner/base/Length.h"
#include "donner/svg/properties/Property.h"
#include "donner/svg/properties/PropertyParsing.h"

namespace donner::svg::components {

/**
 * Properties for a \ref xml_circle element.
 */
struct CircleProperties {
  /// The center x-coordinate of the circle, defaults to 0.
  Property<Lengthd> cx{"cx",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  /// The center y-coordinate of the circle, defaults to 0.
  Property<Lengthd> cy{"cy",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  /// The radius of the circle, defaults to 0.
  Property<Lengthd> r{"r",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};

  /// Get all properties as a tuple.
  auto allProperties() { return std::forward_as_tuple(cx, cy, r); }
};

/**
 * Computed properties for a \ref xml_circle element, which applies values from the CSS cascade.
 */
struct ComputedCircleComponent {
  /**
   * Construct a \ref ComputedCircleComponent from the input properties and unparsed properties.
   *
   * @param inputProperties The input properties for the circle.
   * @param unparsedProperties The unparsed properties for the circle, which may contain
   * presentation attributes set in the CSS stylesheet which need to be applied to the element.
   * @param outWarnings A vector to append any warnings to.
   */
  ComputedCircleComponent(const CircleProperties& inputProperties,
                          const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
                          std::vector<parser::ParseError>* outWarnings);

  /// The computed properties for the circle.
  CircleProperties properties;
};

/**
 * Parameters for a \ref xml_circle element.
 */
struct CircleComponent {
  /// The properties for the circle.
  CircleProperties properties;
};

}  // namespace donner::svg::components
