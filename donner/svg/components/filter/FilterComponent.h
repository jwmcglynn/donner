#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/svg/components/filter/FilterEffect.h"
#include "donner/svg/components/filter/FilterGraph.h"
#include "donner/svg/components/filter/FilterUnits.h"
#include "donner/svg/graph/Reference.h"

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

  /// An optional href to another filter, used to inherit attributes and primitive children.
  std::optional<Reference> href;

  /// The parsed value of the "filterUnits" attribute, which defines the coordinate system for the
  /// `x`, `y`, `width`, and `height` attributes of the mask.
  std::optional<FilterUnits> filterUnits;

  /// The parsed value of the "primitiveUnits" attribute, which defines the coordinate system for
  /// the various attributes of the filter effects.
  std::optional<PrimitiveUnits> primitiveUnits;

  /// The parsed value of the "color-interpolation-filters" property, which specifies the color
  /// space for filter operations.
  std::optional<ColorInterpolationFilters> colorInterpolationFilters;
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
  /// The computed color interpolation mode for filter operations.
  ColorInterpolationFilters colorInterpolationFilters = ColorInterpolationFilters::Default;

  /// The filter graph built from child filter primitives.
  FilterGraph filterGraph;

  /// Parsed list of effects for backward-compatibility with existing renderer push/pop interface.
  /// Will be removed once the graph-based renderer API is complete.
  std::vector<FilterEffect> effectChain;
};

}  // namespace donner::svg::components
