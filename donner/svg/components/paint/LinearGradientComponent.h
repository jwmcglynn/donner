#pragma once
/// @file

#include <optional>

#include "donner/base/EcsRegistry.h"
#include "donner/base/Length.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_linearGradient element.
 */
struct LinearGradientComponent {
  /// The x-coordinate of the gradient start point. If not specified, defaults to 0%.
  std::optional<Lengthd> x1;
  /// The y-coordinate of the gradient start point. If not specified, defaults to 0%.
  std::optional<Lengthd> y1;
  /// The x-coordinate of the gradient end point. If not specified, defaults to 100%.
  std::optional<Lengthd> x2;
  /// The y-coordinate of the gradient end point. If not specified, defaults to 0%.
  std::optional<Lengthd> y2;

  /**
   * Create a \ref ComputedLinearGradientComponent on the current entity, and inherit unset
   * attributes from entity \p base.
   *
   * @param handle Current entity to modify.
   * @param base The entity to inherit unset attributes from.
   */
  void inheritAttributes(EntityHandle handle, EntityHandle base);
};

/**
 * Computed properties for a \ref xml_linearGradient element. This is used to store the resolved
 * properties, replacing unset values with defaults and inheriting from parent elements.
 */

struct ComputedLinearGradientComponent {
  /// The x-coordinate of the gradient start point, defaults to 0%.
  Lengthd x1 = Lengthd(0, Lengthd::Unit::Percent);
  /// The y-coordinate of the gradient start point, defaults to 0%.
  Lengthd y1 = Lengthd(0, Lengthd::Unit::Percent);
  /// The x-coordinate of the gradient end point, defaults to 100%.
  Lengthd x2 = Lengthd(100, Lengthd::Unit::Percent);
  /// The y-coordinate of the gradient end point, defaults to 0%.
  Lengthd y2 = Lengthd(0, Lengthd::Unit::Percent);

  /**
   * Inherit unset attributes from entity \p base.
   *
   * @param handle Current entity to modify.
   * @param base The entity to inherit unset attributes from.
   */
  void inheritAttributes(EntityHandle handle, EntityHandle base);
};

}  // namespace donner::svg::components
