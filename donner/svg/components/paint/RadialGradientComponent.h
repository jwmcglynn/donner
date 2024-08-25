#pragma once
/// @file

#include <optional>

#include "donner/base/Length.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_radialGradient element. Defines a circle ( \ref cx, \ref cy, \ref r)
 * and a focus circle ( \ref fx, \ref fy, \ref fr) for the gradient.
 */
struct RadialGradientComponent {
  /// The x-coordinate of the gradient center. If not specified, defaults to 50%.
  std::optional<Lengthd> cx;
  /// The y-coordinate of the gradient center. If not specified, defaults to 50%.
  std::optional<Lengthd> cy;
  /// The radius of the gradient. If not specified, defaults to 50%.
  std::optional<Lengthd> r;

  /// The x-coordinate of the gradient focus point. If not specified, defaults to \ref cx.
  std::optional<Lengthd> fx;
  /// The y-coordinate of the gradient focus point. If not specified, defaults to \ref cy.
  std::optional<Lengthd> fy;
  /// The radius of the gradient focus point. If not specified, defaults to 0%.
  std::optional<Lengthd> fr;

  /**
   * Create a \ref ComputedRadialGradientComponent on the current entity, and inherit unset
   * attributes from entity \p base.
   *
   * @param handle Current entity to modify.
   * @param base The entity to inherit unset attributes from.
   */
  void inheritAttributes(EntityHandle handle, EntityHandle base);
};

/**
 * Computed properties for a \ref xml_radialGradient element. This is used to store the resolved
 * properties, replacing unset values with defaults and inheriting from parent elements.
 *
 * For \ref fx and \ref fy, if they are not specified they will coincide with \ref cx \ref cy, see
 * https://www.w3.org/TR/SVG2/pservers.html#RadialGradientElementFXAttribute. Represent this by \c
 * using std::nullopt, which will be resolved to cx/cy at the time of rendering.
 */
struct ComputedRadialGradientComponent {
  /// The x-coordinate of the gradient center, defaults to 50%.
  Lengthd cx = Lengthd(50, Lengthd::Unit::Percent);
  /// The y-coordinate of the gradient center, defaults to 50%.
  Lengthd cy = Lengthd(50, Lengthd::Unit::Percent);
  /// The radius of the gradient, defaults to 50%.
  Lengthd r = Lengthd(50, Lengthd::Unit::Percent);

  /// The x-coordinate of the gradient focus point, defaults to \ref cx.
  std::optional<Lengthd> fx;
  /// The y-coordinate of the gradient focus point, defaults to \ref cy.
  std::optional<Lengthd> fy;
  /// The radius of the gradient focus point, defaults to 0%.
  Lengthd fr = Lengthd(0, Lengthd::Unit::Percent);

  /**
   * Inherit unset attributes from entity \p base.
   *
   * @param handle Current entity to modify.
   * @param base The entity to inherit unset attributes from.
   */
  void inheritAttributes(EntityHandle handle, EntityHandle base);
};

}  // namespace donner::svg::components
