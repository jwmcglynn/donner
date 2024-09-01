#pragma once
/// @file

#include "donner/base/parser/ParseError.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

/**
 * Handles parsing and instantiating SVG filter effects from the SVG DOM.
 *
 * @ingroup ecs_systems
 * @see https://drafts.fxtf.org/filter-effects/
 */
class FilterSystem {
public:
  /**
   * Create \ref ComputedFilterComponent for the given entity, applying style information and style
   * inheritance.
   *
   * @param handle Entity handle to apply the filter to.
   * @param component Filter effect for the current entity, so that this may only be called if one
   * is present.
   * @param outWarnings Warnings generated during parsing.
   */
  void createComputedFilter(EntityHandle handle, const FilterComponent& component,
                            std::vector<parser::ParseError>* outWarnings);

  /**
   * Create all \ref ComputedFilterComponent in the tree.
   *
   * @param registry Registry to operate on.
   * @param outWarnings Warnings generated during parsing.
   */
  void instantiateAllComputedComponents(Registry& registry,
                                        std::vector<parser::ParseError>* outWarnings);
};

}  // namespace donner::svg::components
