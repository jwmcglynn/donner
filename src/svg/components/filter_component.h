#pragma once
/// @file

#include <optional>

#include "src/base/length.h"
#include "src/svg/core/filter.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_filter element.
 */
struct FilterComponent {
  std::optional<Lengthd> x;
  std::optional<Lengthd> y;
  std::optional<Lengthd> width;
  std::optional<Lengthd> height;

  FilterUnits filterUnits = FilterUnits::Default;
  PrimitiveUnits primitiveUnits = PrimitiveUnits::Default;
};

}  // namespace donner::svg::components
