#pragma once
/// @file
/// Invalidation of elements that draw another element through a reference.

#include "donner/base/EcsRegistry.h"

namespace donner::svg::components {

/**
 * Mark the elements that draw a changed element through a reference, so the next frame redraws
 * them as well as the changed element:
 * - The text root of a `<textPath>` whose path changed drops its layout, see \ref
 *   InvalidateTextLayout.
 * - An element whose filter draws a changed element, or a changed descendant of it, through an
 *   `<feImage>` fragment reference is flagged to redraw its filter result.
 *
 * A change is a \ref DirtyFlagsComponent left by a DOM edit; for a text path, flags that only an
 * ancestor's transform left on the path do not count, since text along a path ignores the
 * transforms of the path's ancestors. Only the referencing elements are flagged, so a renderer
 * that redraws by dirty element (such as the compositor) redraws just them. Call this before the
 * dirty set is read, each frame; it does nothing when nothing is dirty.
 *
 * @param registry Document registry.
 */
void InvalidateReferenceDependents(Registry& registry);

}  // namespace donner::svg::components
