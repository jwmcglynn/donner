#pragma once
/// @file

#include <vector>

#include "donner/svg/core/Gradient.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

/**
 * Common parameters for gradient elements, \ref xml_linearGradient and \ref xml_radialGradient.
 *
 * When this component is present, either \ref LinearGradientComponent or \ref
 * RadialGradientComponent must be present.
 */
struct GradientComponent {
  /// The parsed value of the "gradientUnits" attribute, which specifies how the coordinate system
  /// for linear/radial gradient positional attributes (such as `x1`, `y1`, `cx`, `cy`). Empty if no
  /// attribute was specified.
  std::optional<GradientUnits> gradientUnits;

  /// The parsed value of the "spreadMethod" attribute, which specifies how the gradient is repeated
  /// on its edges (such as spread, reflect, or repeat). Empty if no attribute was specified.
  std::optional<GradientSpreadMethod> spreadMethod;

  /// The parsed value of the "href" attribute, which specifies a reference to a gradient element to
  /// inherit from.
  std::optional<Reference> href;
};

/**
 * Created by \ref PaintSystem during render tree instantiation.
 *
 * - Resolves the inheritance hierarchy from \ref GradientComponent::href
 * - Collects gradient stop information from \ref xml_stop child elements into \ref
 *   ComputedGradientComponent::stops
 *
 * Since this component may instantiate dependencies on construction, it constructs with two-phase
 * initialization.
 *
 * Upon construction, fields are initialized to default values. The `initialize` method must be
 * called to complete initialization.
 *
 * To construct this object, call \ref PaintSystem::instantiateAllComputedComponents.
 */
struct ComputedGradientComponent {
  /// True if this component has been initialized (it has two-phase construction). If this is false,
  /// all other fields of this component will be invalid.
  bool initialized = false;

  /// Resolved value of the "gradientsUnits" attribute, considering inheritance and the default
  /// value fallback.
  GradientUnits gradientUnits = GradientUnits::Default;

  /// Resolved value of the "spreadMethod" attribute, considering inheritance and the default value
  /// fallback.
  GradientSpreadMethod spreadMethod = GradientSpreadMethod::Default;

  /// Parsed gradient stops from \ref xml_stop child elements.
  std::vector<GradientStop> stops;

  /**
   * Resolve unspecified attributes to default values or inherit from the given base gradient
   * element. This method is used to propagate attributes such as `x1`, `y1`, `cx`,
   * `cy`, `r`, etc from the base element to the current element.
   *
   * @param handle Current entity handle attached to this component.
   * @param base Base entity handle to inherit from, if any. If no base is specified, unspecified
   * attributes are resolved to default values.
   */
  void inheritAttributesFrom(EntityHandle handle, EntityHandle base);
};

}  // namespace donner::svg::components
