#pragma once
/// @file

#include "src/base/length.h"
#include "src/svg/properties/property.h"
#include "src/svg/properties/property_parsing.h"

namespace donner::svg::components {

struct CircleProperties {
  Property<Lengthd> cx{"cx",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> cy{"cy",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> r{"r",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};

  auto allProperties() { return std::forward_as_tuple(cx, cy, r); }
};

struct ComputedCircleComponent {
  ComputedCircleComponent(const CircleProperties& inputProperties,
                          const std::map<RcString, parser::UnparsedProperty>& unparsedProperties,
                          std::vector<parser::ParseError>* outWarnings);

  CircleProperties properties;
};

/**
 * Parameters for a \ref xml_circle element.
 */
struct CircleComponent {
  CircleProperties properties;
};

}  // namespace donner::svg::components
