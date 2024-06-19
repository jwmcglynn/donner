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
  void createComputedFilter(EntityHandle handle, const FilterComponent& component,
                            std::vector<parser::ParseError>* outWarnings);

  void instantiateAllComputedComponents(Registry& registry,
                                        std::vector<parser::ParseError>* outWarnings);
};

}  // namespace donner::svg::components
