#pragma once
/// @file

#include "donner/base/EcsRegistry.h"

namespace donner::css {
class SelectorTraversalBudget;
}
namespace donner::xml {
class XMLNode;
}
namespace donner::svg::components {

/// Non-whitespace direct XML text consumed by SVG projection without a live text component.
/// Cleared when authored contents are replaced; conversion pruning must preserve it.
struct ProjectedNonWhitespaceTextTag {};

/**
 * Preserve direct text that is about to be consumed by SVG conversion.
 * Existing projected content remains when no raw chunks are available during reprojection.
 * @param parent Projected parent in the XML node's registry.
 * @param node XML parent before non-element children are pruned.
 */
void RecordDiscardedSelectorText(EntityHandle parent, const xml::XMLNode& node);

/**
 * Begin a content-projection pass, preserving summaries during attribute-only reprojection.
 * @param parent Element whose content is being projected.
 * @param contentsReparsed True only when authored content was replaced from a new XML parse.
 */
void BeginSelectorContentProjection(EntityHandle parent, bool contentsReparsed);

/**
 * Inspect CSS emptiness using the adapter's tree and its authoritative data entity.
 * @param registry Registry shared by the tree and data entities.
 * @param treeEntity Element-child view, or entt::null to omit instanced shadow children.
 * @param dataEntity Element containing authoritative text and projection metadata.
 * @param budget Shared selector-work budget, or nullptr for an unbudgeted DOM inspection.
 * @return True if no element or non-whitespace text children exist; false on budget exhaustion.
 */
bool IsSelectorEmpty(Registry& registry, Entity treeEntity, Entity dataEntity,
                     css::SelectorTraversalBudget* budget);

}  // namespace donner::svg::components
