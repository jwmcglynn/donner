#pragma once
/// @file

#include <optional>

#include "donner/base/EcsRegistry.h"
#include "donner/css/Selector.h"
#include "donner/svg/SVGElement.h"

namespace donner::svg::details {

/**
 * Find the first descendant of \p root that matches \p selector.
 *
 * The caller must already hold appropriate document access for \p root.
 *
 * @param selector Parsed CSS selector.
 * @param root Root element whose descendants should be searched.
 */
std::optional<SVGElement> QuerySelectorSearch(const css::Selector& selector, EntityHandle root);

/// Test seam that uses an explicitly reduced shared traversal budget.
std::optional<SVGElement> QuerySelectorSearch(const css::Selector& selector, EntityHandle root,
                                              css::SelectorTraversalBudget& traversalBudget);

}  // namespace donner::svg::details
