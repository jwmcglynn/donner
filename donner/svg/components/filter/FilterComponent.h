#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/components/filter/FilterUnits.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_filter element.
 *
 * Contains the filter bounds, which determines how large the canvas needs to be when applying the
 * filter, and filter configuration such as units for its parameters.
 */
struct FilterComponent {
  /// The x-coordinate of the filter, defaults to -10% (outside the element itself).
  std::optional<Lengthd> x;
  /// The y-coordinate of the filter, defaults to -10% (outside the element itself).
  std::optional<Lengthd> y;
  /// Width of the filter, defaults to 120% (outside of the element itself).
  std::optional<Lengthd> width;
  /// Height of the filter, defaults to 120% (outside of the element itself).
  std::optional<Lengthd> height;

  /// The parsed value of the "filterUnits" attribute, which defines the coordinate system for the
  /// `x`, `y`, `width`, and `height` attributes of the mask.
  FilterUnits filterUnits = FilterUnits::Default;

  /// The parsed value of the "primitiveUnits" attribute, which defines the coordinate system for
  /// the various attributes of the filter effects.
  PrimitiveUnits primitiveUnits = PrimitiveUnits::Default;
};

/**
 * Computed filter parameters parsed by \ref FilterSystem, represents the resolved DOM hierarchy of
 * a \ref xml_filter element.
 */
struct ComputedFilterComponent {
  Lengthd x = Lengthd(-10.0, Lengthd::Unit::Percent);  ///< The computed x-coordinate of the filter.
  Lengthd y = Lengthd(-10.0, Lengthd::Unit::Percent);  ///< The computed y-coordinate of the filter.
  Lengthd width = Lengthd(120.0, Lengthd::Unit::Percent);   ///< The computed width of the filter.
  Lengthd height = Lengthd(120.0, Lengthd::Unit::Percent);  ///< The computed height of the filter.

  FilterUnits filterUnits = FilterUnits::Default;           ///< The computed filter units.
  PrimitiveUnits primitiveUnits = PrimitiveUnits::Default;  ///< The computed primitive units.

  /// Parsed list of effects, which can be chained together to create complex effects. These are
  /// evaluated in order.
  std::vector<FilterEffect> effectChain;
};

}  // namespace donner::svg::components
