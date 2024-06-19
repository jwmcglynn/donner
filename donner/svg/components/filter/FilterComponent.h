#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/components/filter/FilterUnits.h"

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
