#pragma once
/// @file

#include <optional>

#include "src/base/length.h"
#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/properties/presentation_attribute_parsing.h"
#include "src/svg/properties/property.h"
#include "src/svg/properties/rx_ry_properties.h"

namespace donner::svg::components {

/**
 * Parameters for a <rect> element.
 */
struct RectProperties {
  Property<Lengthd> x{"x",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> y{"y",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> width{
      "width", []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> height{
      "height", []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> rx{"rx", []() -> std::optional<Lengthd> { return std::nullopt; }};
  Property<Lengthd> ry{"ry", []() -> std::optional<Lengthd> { return std::nullopt; }};

  auto allProperties() { return std::forward_as_tuple(x, y, width, height, rx, ry); }

  std::tuple<Lengthd, double> calculateRx(const Boxd& viewbox,
                                          const FontMetrics& fontMetrics) const {
    return CalculateRadiusMaybeAuto(rx, ry, viewbox, fontMetrics);
  }

  std::tuple<Lengthd, double> calculateRy(const Boxd& viewbox,
                                          const FontMetrics& fontMetrics) const {
    return CalculateRadiusMaybeAuto(ry, rx, viewbox, fontMetrics);
  }
};

struct ComputedRectComponent {
  ComputedRectComponent(const RectProperties& inputProperties,
                        const std::map<RcString, UnparsedProperty>& unparsedProperties,
                        std::vector<ParseError>* outWarnings);

  RectProperties properties;
};

struct RectComponent {
  RectProperties properties;

  void computePathWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                       const FontMetrics& fontMetrics,
                                       std::vector<ParseError>* outWarnings);

  void computePath(EntityHandle handle, const FontMetrics& fontMetrics) {
    ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
    style.computeProperties(handle);

    return computePathWithPrecomputedStyle(handle, style, fontMetrics, nullptr);
  }
};

void InstantiateComputedRectComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg::components
