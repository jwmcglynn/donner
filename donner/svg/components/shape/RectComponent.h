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
                        const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
                        std::vector<parser::ParseError>* outWarnings);

  RectProperties properties;
};

struct RectComponent {
  RectProperties properties;
};

}  // namespace donner::svg::components
