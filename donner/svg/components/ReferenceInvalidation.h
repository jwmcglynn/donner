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
 *   components. Filters that take their primitives from such a filter through `href` count too.
 * - An element clipped or masked by a `<clipPath>` or `<mask>` whose content draws a changed
 *   element is marked \ref DirtyFlagsComponent::ReferencedContent as well. The content draws it
 *   when it holds the element, or a `<use>` that copies the element or an ancestor of it, directly
 *   or through a chain of `<use>` copies. A clip path or mask also draws what it clips or masks its
 *   own content by, through `clip-path` and `mask`.
 *
 * A marked element is itself a change for the `feImage`, `<use>`, clip path and mask references.
 *
 * A change is a \ref DirtyFlagsComponent left by a DOM edit. A text path is relaid out for a
 * change that can move its path: a shape, layout or transform write on the path, or any restyle
 * that reaches the path other than a compositing one, including one from an ancestor. An
 * ancestor's transform and compositing changes do not count. A `<use>` copy is placed by the
 * `<use>`, so an element moved only by its ancestor's transform does not change a copy of it. Only
 * the referencing elements are marked, so a renderer that redraws by dirty element (such as the
 * compositor) redraws just them. The walk reaches each element once, so its cost stays linear in
 * the elements and references it reaches, and reference cycles end it. The map from elements to
 * the `<use>` copies a clip path or mask can draw is built only once the walk reaches an element
 * with an `id`, and is kept across frames until the document is rebuilt as a whole. Call this
 * before the dirty set is read, each frame; it does nothing when nothing is dirty.
 *
 * @param registry Document registry.
 */
void InvalidateReferenceDependents(Registry& registry);

}  // namespace donner::svg::components
