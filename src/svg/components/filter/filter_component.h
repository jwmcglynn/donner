#pragma once
/// @file

#include <optional>

#include "src/base/length.h"
#include "src/svg/components/filter/filter_effect.h"
#include "src/svg/components/filter/filter_units.h"

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

/**
 * Computed filter parameters parsed by \ref FilterSystem, represents the resolved DOM hierarchy of
 * a \ref xml_filter element.
 */
struct ComputedFilterComponent {
  Lengthd x = Lengthd(-10.0, Lengthd::Unit::Percent);
  Lengthd y = Lengthd(-10.0, Lengthd::Unit::Percent);
  Lengthd width = Lengthd(120.0, Lengthd::Unit::Percent);
  Lengthd height = Lengthd(120.0, Lengthd::Unit::Percent);

  FilterUnits filterUnits = FilterUnits::Default;
  PrimitiveUnits primitiveUnits = PrimitiveUnits::Default;

  std::vector<FilterEffect> effectChain;
};

}  // namespace donner::svg::components
