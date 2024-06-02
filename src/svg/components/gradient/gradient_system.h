#pragma once
/// @file

#include "src/svg/components/gradient/gradient_component.h"
#include "src/svg/components/gradient/stop_component.h"
#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/components/style/style_system.h"

namespace donner::svg::components {

class GradientSystem {
public:
  template <typename T>
  void createComputedType(EntityHandle handle, const T& component) {
    const ComputedStyleComponent& style = StyleSystem().computeProperties(handle);
    createComputedTypeWithStyle(handle, component, style, nullptr);
  }

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
  void instantiateAllComputedComponents(Registry& registry, std::vector<ParseError>* outWarnings);

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
   * When this occurs, this is represented by instantiating a new shadow tree on the current
   * element, by creating a \ref ShadowTreeComponent.
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
   * children of `#a`. During \ref InstantiateGradientComponents, the shadow tree will be traversed
   * to find the \ref xml_stop elements to inherit.
   *
   * @see https://www.w3.org/TR/SVG2/pservers.html#PaintServerTemplates
   *
   * During instantiation, warnings may be emitted if the "href" attribute does not reference a
   * valid gradient element.
   *
   * @param registry Registry used to find all \ref GradientComponent to compute shadow trees
   * @param outWarnings Containing any warnings found
   */
  void createGradientShadowTrees(Registry& registry, std::vector<ParseError>* outWarnings);

private:
  /**
   * Initialize \ref computedGradient with the given entity handle. This method must be called after
   * construction to complete initialization.
   *
   * This method:
   * - Looks up \ref GradientComponent and \ref LinearGradientComponent components attached to the
   *   entity.
   * - Resolves the href reference and inherits attributes from the referenced gradient element.
   * - Aggregates \ref xml_stop information into the \ref stops field.
   */
  void initializeComputedGradient(EntityHandle handle, ComputedGradientComponent& computedGradient,
                                  std::vector<ParseError>* outWarnings);

  const ComputedGradientComponent& createComputedTypeWithStyle(
      EntityHandle handle, const GradientComponent& gradient, const ComputedStyleComponent& style,
      std::vector<ParseError>* outWarnings);

  const ComputedStopComponent& createComputedTypeWithStyle(EntityHandle handle,
                                                           const StopComponent& stop,
                                                           const ComputedStyleComponent& style,
                                                           std::vector<ParseError>* outWarnings);
};

}  // namespace donner::svg::components
