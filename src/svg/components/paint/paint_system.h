#pragma once
/// @file

#include "src/svg/components/paint/gradient_component.h"
#include "src/svg/components/paint/pattern_component.h"
#include "src/svg/components/paint/stop_component.h"
#include "src/svg/components/style/computed_style_component.h"
#include "src/svg/components/style/style_system.h"
#include "src/svg/registry/registry.h"

namespace donner::svg::components {

/**
 * Manages Paint Servers for gradients and patterns, handling style and structural inheritance and
 * creating "computed" state and shadow trees for these elements.
 *
 * @ingroup ecs_systems
 * @see https://www.w3.org/TR/SVG2/pservers.html
 */
class PaintSystem {
public:
  /**
   * Create a \ref ComputedStopComponent for the given entity handle and its attached \ref
   * StopComponent, which applies CSS styling information and presentation attributes.
   *
   * @param handle Entity handle to create the computed gradient for
   * @param stop Stop component attached to \ref handle
   * @param outWarnings Containing any warnings found
   */
  const ComputedStopComponent& createComputedStop(EntityHandle handle, const StopComponent& stop,
                                                  std::vector<parser::ParseError>* outWarnings) {
    const ComputedStyleComponent& style = StyleSystem().computeStyle(handle, outWarnings);
    return createComputedStopWithStyle(handle, stop, style, outWarnings);
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
  void instantiateAllComputedComponents(Registry& registry,
                                        std::vector<parser::ParseError>* outWarnings);

  /**
   * Create shadow trees for all gradient and pattern elements in the registry that have a valid
   * href="" attribute.
   *
   * This must be called before \ref instantiateAllComputedComponents.
   *
   * @param registry Registry used to find all \ref GradientComponent and \ref PatternComponent
   * components to compute shadow trees
   * @param outWarnings Containing any warnings found
   */
  void createShadowTrees(Registry& registry, std::vector<parser::ParseError>* outWarnings);

private:
  /**
   * Initialize \ref computedGradient with the given entity handle. This method must be called after
   * construction to complete initialization.
   *
   * This method:
   * - Looks up \ref GradientComponent and other gradient components attached to the entity.
   * - Resolves the href reference and inherits attributes from the referenced gradient element.
   * - Aggregates \ref xml_stop information into the \ref stops field.
   *
   * @param handle Entity handle to initialize
   * @param computedGradient Computed gradient component to initialize
   * @param outWarnings Containing any warnings found
   */
  void initializeComputedGradient(EntityHandle handle, ComputedGradientComponent& computedGradient,
                                  std::vector<parser::ParseError>* outWarnings);

  /**
   * Initialize \ref computedPattern with the given entity handle. This method must be called after
   * construction to complete initialization.
   *
   * This method:
   * - Looks up \ref PatternComponent and other pattern components attached to the entity.
   * - Resolves the href reference and inherits attributes from the referenced pattern element.
   *
   * @param handle Entity handle to initialize
   * @param computedPattern Computed pattern component to initialize
   * @param outWarnings Containing any warnings found
   */
  void initializeComputedPattern(EntityHandle handle, ComputedPatternComponent& computedPattern,
                                 std::vector<parser::ParseError>* outWarnings);

  /**
   * Follow the href="" attributes of the given entity handle to find all entities in the
   * inheritance chain, returning a list from current -> base (inclusive).
   *
   * If recursion is detected, a warning will be emitted and the inheritance change except for the
   * first recursion is returned as-is.
   *
   * @param handle Entity handle to start the inheritance chain from
   * @param outWarnings Containing any warnings found
   */
  std::vector<Entity> getInheritanceChain(EntityHandle handle,
                                          std::vector<parser::ParseError>* outWarnings);

  /**
   * Create a \ref ComputedStopComponent for a given entity handle and its attached \ref
   * StopComponent, which applies CSS styling information and presentation attributes.
   *
   * @param handle Entity handle to create the computed stop for
   * @param stop Stop component attached to \ref handle
   * @param style Computed style component for the entity, created from \ref StyleSystem
   * @param outWarnings Containing any warnings found
   */
  const ComputedStopComponent& createComputedStopWithStyle(
      EntityHandle handle, const StopComponent& stop, const ComputedStyleComponent& style,
      std::vector<parser::ParseError>* outWarnings);

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
   *
   * This is represented by adding a `ShadowTreeComponent` to the `#b` element, references the
   * children of `#a`. During \ref instantiateAllComputedComponents, the shadow tree will be
   * traversed to find the \ref xml_stop elements to inherit.
   *
   * @see https://www.w3.org/TR/SVG2/pservers.html#PaintServerTemplates
   *
   * During instantiation, warnings may be emitted if the "href" attribute does not reference a
   * valid gradient element.
   *
   * @param registry Registry used to find all \ref GradientComponent to compute shadow trees
   * @param outWarnings Containing any warnings found
   */
  void createGradientShadowTrees(Registry& registry, std::vector<parser::ParseError>* outWarnings);

  /**
   * Instantiate shadow trees for valid "href" attributes in pattern elements for all elements in
   * the registry.
   *
   * For pattern inheritance, graphical elements may be inherited from a referenced pattern element,
   * assuming that the current element has no structural children.
   *
   * > if the current element does not have any child content other than descriptive elements, than
   * > the child content of the template element is cloned to replace it.
   *
   * When this occurs, this is represented by instantiating a new shadow tree on the current
   * element, by creating a \ref ShadowTreeComponent.
   *
   * For example, given the following gradients:
   * ```xml
   * <pattern id="a" patternUnits="userSpaceOnUse" width="20" height="20">
   *   <rect x="0" y="0" width="10" height="10" fill="grey"/>
   *   <rect x="10" y="10" width="10" height="10" fill="green"/>
   * </pattern>
   * <pattern id="b" href="#a" />
   * ```
   *
   * Conceptually this represents a tree where all elements of `#a` are cloned under `#b`:
   * ```
   * <!-- From -->
   * <pattern id="b" href="#a" />
   *
   * <!-- To -->
   * <pattern id="b">
   * + - copy  - paste - - - - - - - - - - - - - - - - - - - - - +
   * | <rect x="0" y="0" width="10" height="10" fill="grey"/>    |
   * | <rect x="10" y="10" width="10" height="10" fill="green"/> |
   * + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
   * </linearGradient>
   * ```
   *
   * This is represented by adding a `ShadowTreeComponent` to the `#b` element, references the
   * children of `#a`. During rendering, the shadow tree will be traversed to find the pattern
   * contents.
   *
   * @see https://www.w3.org/TR/SVG2/pservers.html#PaintServerTemplates
   *
   * During instantiation, warnings may be emitted if the "href" attribute does not reference a
   * valid gradient element.
   *
   * @param registry Registry used to find all \ref PatternComponent to compute shadow trees
   * @param outWarnings Containing any warnings found
   */
  void createPatternShadowTrees(Registry& registry, std::vector<parser::ParseError>* outWarnings);
};

}  // namespace donner::svg::components
