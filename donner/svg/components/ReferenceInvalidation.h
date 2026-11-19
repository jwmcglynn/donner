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
 * - An element whose filter, or one `url()` stage of its filter list, draws a changed element or a
 *   changed descendant of it through an `<feImage>` fragment reference is marked \ref
 *   DirtyFlagsComponent::ReferencedContent, which redraws it without recomputing its own
 *   components. Filters that take their primitives from such a filter through `href` count too,
 *   and a marked element is itself a change for other `feImage` references.
 *
 * A change is a \ref DirtyFlagsComponent left by a DOM edit. A text path is relaid out only for a
 * change that can move its path: a shape, layout or transform write on the path, or a restyle of
 * the path itself; an ancestor's transform or inherited style and compositing changes do not
 * count. Only the referencing elements are marked, so a renderer that redraws by dirty element
 * (such as the compositor) redraws just them. Call this before the dirty set is read, each frame;
 * it does nothing when nothing is dirty.
 *
 * @param registry Document registry.
 */
void InvalidateReferenceDependents(Registry& registry);

}  // namespace donner::svg::components
