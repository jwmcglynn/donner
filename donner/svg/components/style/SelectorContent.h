#pragma once
/// @file

#include <string_view>

#include "donner/base/EcsRegistry.h"

namespace donner::css {
class SelectorTraversalBudget;
}
namespace donner::xml {
class XMLNode;
}
namespace donner::svg::components {

/// Non-whitespace authored text not represented by the active raw-text child view.
/// Covers conversion pruning and typed appends after incremental source projection.
struct ProjectedNonWhitespaceTextTag {};

/// Incremental content projection retained a complete raw-text child view for selectors.
struct SelectorRawTextAuthorityTag {};

/// A rejected source projection leaves retained typed text authoritative.
struct SelectorRejectedTextTag {};

/**
 * Preserve the last accepted typed text when source projection rejects its replacement.
 * @param parent Element whose source content projection failed.
 */
void RejectSelectorContentProjection(EntityHandle parent);

/**
 * Accept a value-only edit without inferring a complete raw view from attached text deltas.
 * @param parent Element whose typed value projection succeeded.
 */
void AcceptSelectorTextValueProjection(EntityHandle parent);

/**
 * Replace typed text, discarding direct raw text left by incremental source projection.
 * Call under document write access, before assigning the replacement retained string.
 * @param parent Element whose typed text is being replaced.
 */
void ResetSelectorTextContent(EntityHandle parent);

/**
 * Preserve a typed append when raw children are the active selector text representation.
 * @param parent Element receiving appended text.
 * @param text Appended text.
 */
void RecordAppendedSelectorText(EntityHandle parent, std::string_view text);

/**
 * Preserve direct text that is about to be consumed by SVG conversion.
 * Existing projected content remains when no raw chunks are available during reprojection.
 * @param parent Projected parent in the XML node's registry.
 * @param node XML parent before non-element children are pruned.
 */
void RecordDiscardedSelectorText(EntityHandle parent, const xml::XMLNode& node);

/**
 * Complete a successful content-projection pass, preserving attribute-only summaries.
 * @param parent Element whose content is being projected.
 * @param contentsReparsed True only when authored content was replaced from a new XML parse.
 */
void CompleteSelectorContentProjection(EntityHandle parent, bool contentsReparsed);

/**
 * Inspect CSS emptiness using authored children and text on the authoritative data entity.
 * @param registry Registry shared by the tree and data entities.
 * @param dataEntity Element containing authoritative text and projection metadata.
 * @param budget Shared selector-work budget, or nullptr for an unbudgeted DOM inspection.
 * @return True if no element or non-whitespace text children exist; false on budget exhaustion.
 */
bool IsSelectorEmpty(Registry& registry, Entity dataEntity, css::SelectorTraversalBudget* budget);

}  // namespace donner::svg::components
