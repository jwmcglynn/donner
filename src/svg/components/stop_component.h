#pragma once
/// @file

#include <optional>

#include "src/base/length.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/core/gradient.h"

namespace donner::svg {

/**
 * Parameters for a <stop> element.
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
                        const std::map<RcString, UnparsedProperty>& unparsedProperties,
                        std::vector<ParseError>* outWarnings);

  StopProperties properties;
};

struct StopComponent {
  StopProperties properties;

  void computeWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                   std::vector<ParseError>* outWarnings) {
    handle.emplace_or_replace<ComputedStopComponent>(
        properties, style, style.properties().unparsedProperties, outWarnings);
  }

  void compute(EntityHandle handle) {
    ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
    style.computeProperties(handle);

    return computeWithPrecomputedStyle(handle, style, nullptr);
  }
};

void InstantiateStopComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg
