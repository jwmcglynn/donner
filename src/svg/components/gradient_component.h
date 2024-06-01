#pragma once
/// @file

#include <vector>

#include "src/base/parser/parse_error.h"
#include "src/base/transform.h"
#include "src/svg/core/gradient.h"
#include "src/svg/graph/reference.h"
#include "src/svg/registry/registry.h"

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

  /**
   * Create a \ref ComputedGradientComponent from this component. Has no effect if the computed
   * component has already been created for this entity.
   *
   * @param handle Current entity handle attached to this component.
   */
  void compute(EntityHandle handle);
};

/**
 * Created by \ref InstantiateGradientComponents during render tree instantiation.
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
 * To construct this object, call \ref GradientComponent::compute.
 */
struct ComputedGradientComponent {
  /// True if \ref initialize has been called on this component. If this is false, all other fields
  /// of this component will be invalid.
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
   * Initialize this component with the given entity handle. This method must be called after
   * construction to complete initialization.
   *
   * This method:
   * - Looks up \ref GradientComponent and \ref LinearGradientComponent components attached to the
   *   entity.
   * - Resolves the href reference and inherits attributes from the referenced gradient element.
   * - Aggregates \ref xml_stop information into the \ref stops field.
   */
  void initialize(EntityHandle handle);

  /**
   * Resolve unspecified attributes to default values or inherit from the given base gradient
   * element. This method is used to propagate attributes such as `x1`, `y1`, `cx`,
   * `cy`, `r`, etc from the base element to the current element.
   *
   * @param handle Current entity handle attached to this component.
   * @param base Base entity handle to inherit from, if any. If no base is specified, unspecified
   * attributes are resolved to default values.
   */
  void resolveAndInheritAttributes(EntityHandle handle, EntityHandle base);
};

/**
 * Instantiate shadow trees for valid "href" attributes in gradient elements for all elements in
 * the registry.
 *
 * For gradient inheritance, \ref xml_stop elements may be inherited from a referenced
 * gradient element, assuming that the current element has no structural children.
 *
 * > if the current element does not have any child content other than descriptive elements, than
 * > the child content of the template element is cloned to replace it.
 *
 * When this occurs, this is represented by instantiating a new shadow tree on the current element,
 * by creating a \ref ShadowTreeComponent.
 *
 * For example, given the following gradients:
 * ```xml
 * <linearGradient id="a">
 *   <stop offset="0" stop-color="red" />
 *   <stop offset="100" stop-color="blue" />
 * </linearGradient>
 * <linearGradient id="b" href="#a" />
 * ```
 *
 * Conceptually this represents a tree where all elements of `#a` are cloned under `#b`:
 * ```
 * <!-- From -->
 * <linearGradient id="b" href="#a" />
 *
 * <!-- To -->
 * <linearGradient id="b">
 * + - copy  - paste - - - - - - - - - - - - +
 * | <stop offset="0" stop-color="red" />    |
 * | <stop offset="100" stop-color="blue" /> |
 * + - - - - - - - - - - - - - - - - - - - - +
 * </linearGradient>
 * ```
 * This is represented by adding a `ShadowTreeComponent` to the `#b` element, references the
 * children of `#a`. During \ref InstantiateGradientComponents, the shadow tree will be traversed to
 * find the \ref xml_stop elements to inherit.
 *
 * @see https://www.w3.org/TR/SVG2/pservers.html#PaintServerTemplates
 *
 * During instantiation, warnings may be emitted if the "href" attribute does not reference a valid
 * gradient element.
 *
 * @param registry Registry used to find all \ref GradientComponent to compute shadow trees
 * @param outWarnings Containing any warnings found
 */
void EvaluateConditionalGradientShadowTrees(Registry& registry,
                                            std::vector<ParseError>* outWarnings);

/**
 * Create \ref ComputedGradientComponent for all entities in the registry that have a \ref
 * GradientComponent.
 *
 * This assumes that \ref EvaluateConditionalGradientShadowTrees has already been called.
 *
 * Note that this function does not produce any warnings, its signature is used to create a common
 * API pattern.
 *
 * @param registry Registry used to find all \ref GradientComponent
 * @param outWarnings Containing any warnings found
 */
void InstantiateGradientComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg::components
