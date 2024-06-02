#pragma once
/// @file

#include "src/base/length.h"
#include "src/svg/properties/property.h"
#include "src/svg/properties/property_parsing.h"
#include "src/svg/properties/rx_ry_properties.h"  // IWYU pragma: keep, defines CalculateRadiusMaybeAuto

namespace donner::svg::components {

/**
 * Parameters for a <ellipse> element.
 */
struct EllipseProperties {
  Property<Lengthd> cx{"cx",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> cy{"cy",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> rx{"rx", []() -> std::optional<Lengthd> { return std::nullopt; }};
  Property<Lengthd> ry{"ry", []() -> std::optional<Lengthd> { return std::nullopt; }};

  auto allProperties() { return std::forward_as_tuple(cx, cy, rx, ry); }

  std::tuple<Lengthd, double> calculateRx(const Boxd& viewbox,
                                          const FontMetrics& fontMetrics) const {
    return CalculateRadiusMaybeAuto(rx, ry, viewbox, fontMetrics);
  }

  std::tuple<Lengthd, double> calculateRy(const Boxd& viewbox,
                                          const FontMetrics& fontMetrics) const {
    return CalculateRadiusMaybeAuto(ry, rx, viewbox, fontMetrics);
  }
};

struct ComputedEllipseComponent {
  ComputedEllipseComponent(const EllipseProperties& inputProperties,
                           const std::map<RcString, UnparsedProperty>& unparsedProperties,
                           std::vector<ParseError>* outWarnings);

  EllipseProperties properties;
};

struct EllipseComponent {
  EllipseProperties properties;
};

}  // namespace donner::svg::components
