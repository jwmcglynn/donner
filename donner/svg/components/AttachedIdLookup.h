#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/base/RcString.h"

namespace donner::svg::components {

/**
 * Finds an attached document-tree entity with the given ID.
 *
 * Uses \ref SVGDocumentContext's ID map as the fast path. If that map points to a detached or
 * stale entity, falls back to a document-order tree scan so detached-but-held duplicate IDs do not
 * mask an attached element.
 *
 * @param registry Document registry.
 * @param id ID to resolve.
 * @return The attached entity with \p id, or \ref entt::null when none exists.
 */
Entity FindAttachedEntityById(Registry& registry, const RcString& id);

}  // namespace donner::svg::components
