#pragma once
/// @file

#include <optional>

#include "src/base/length.h"
#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/core/gradient.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_stop element.
 */
struct StopProperties {
  float offset = 0.0f;
  Property<css::Color> stopColor{"stop-color", []() -> std::optional<css::Color> {
                                   return css::Color(css::RGBA(0, 0, 0, 0xFF));
                                 }};
  Property<double> stopOpacity{"stop-opacity", []() -> std::optional<double> { return 1.0; }};

  auto allProperties() { return std::forward_as_tuple(stopColor, stopOpacity); }
};

struct ComputedStopComponent {
  ComputedStopComponent(const StopProperties& inputProperties, const ComputedStyleComponent& style,
                        const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
                        std::vector<parser::ParseError>* outWarnings);

  StopProperties properties;
};

struct StopComponent {
  StopProperties properties;
};

}  // namespace donner::svg::components
