#pragma once
/// @file

#include <optional>

#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_stop element.
 */
struct StopProperties {
  /// The offset of the stop, defaults to 0. Range is [0, 1], if it is outside the range it will be
  /// clamped.
  float offset = 0.0f;

  /// The color of the stop, defaults to black.
  Property<css::Color> stopColor{"stop-color", []() -> std::optional<css::Color> {
                                   return css::Color(css::RGBA(0, 0, 0, 0xFF));
                                 }};

  /// The opacity of the stop, defaults to 1. Range is [0, 1], if it is outside the range it will be
  /// clamped.
  Property<double> stopOpacity{"stop-opacity", []() -> std::optional<double> { return 1.0; }};

  /// Get all properties as a tuple.
  auto allProperties() { return std::forward_as_tuple(stopColor, stopOpacity); }
};

/**
 * Stores the computed properties of a \ref xml_stop element. This is used to store the resolved
 * properties combining both the XML tree and the CSS tree, with currentColor resolved.
 *
 */
struct ComputedStopComponent {
  /**
   * Compute the computed properties of a \ref xml_stop element.
   *
   * @param inputProperties The properties of the \ref xml_stop element.
   * @param style The computed style of the stop element.
   * @param unparsedProperties The unparsed properties of the \ref xml_stop element.
   * @param outWarnings The warnings that were generated during parsing.
   */
  ComputedStopComponent(const StopProperties& inputProperties, const ComputedStyleComponent& style,
                        const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
                        std::vector<ParseError>* outWarnings);

  /// Computed properties of the \ref xml_stop element.
  StopProperties properties;
};

/**
 * Stores the properties of a \ref xml_stop element.
 */
struct StopComponent {
  /// The properties of the \ref xml_stop element.
  StopProperties properties;
};

}  // namespace donner::svg::components
