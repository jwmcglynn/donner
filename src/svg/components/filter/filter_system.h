#pragma once
/// @file

#include "src/base/parser/parse_error.h"
#include "src/svg/components/filter/filter_component.h"
#include "src/svg/registry/registry.h"

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
