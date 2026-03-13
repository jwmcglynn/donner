#pragma once
/// @file

#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/renderer/CompositingLayer.h"

namespace donner::svg {

/**
 * Decompose the flat render tree into compositing layers.
 *
 * The algorithm:
 * 1. Expands each promoted entity to its full subtree range (using SubtreeInfo).
 * 2. Escalates promotion when an ancestor has opacity, filter, mask, or clip-path
 *    (compositing context escalation).
 * 3. Merges overlapping promoted ranges.
 * 4. Slices the flat entity list into contiguous layers (static or dynamic).
 * 5. Assigns a LayerMembershipComponent to every entity.
 *
 * @param registry The ECS registry containing the render tree.
 * @param promotedEntities Entities to promote to dynamic layers.
 * @param reason The reason for promotion (Animation, Selection, etc.).
 * @return Ordered list of layers covering the entire render tree.
 */
LayerDecompositionResult decomposeIntoLayers(Registry& registry,
                                             const std::vector<Entity>& promotedEntities,
                                             CompositingLayer::Reason reason =
                                                 CompositingLayer::Reason::Animation);

}  // namespace donner::svg
