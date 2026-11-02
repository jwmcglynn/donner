#pragma once
/// @file
/// Internal helpers shared by \ref donner::svg::SVGUseElement and the generic attribute write.

#include "donner/base/EcsRegistry.h"

namespace donner::svg {

/**
 * Drop a `<use>` element's layout cache and instantiated shadow tree or external instance, and
 * request a render tree rebuild. Call after anything that changes what or where the use renders.
 *
 * @param handle The `<use>` element.
 */
void InvalidateUseInstance(EntityHandle handle);

/**
 * Invalidate a `<use>` element after its `x`, `y`, `width` or `height` changed. These place the
 * instance and size the viewport a referenced `<symbol>` or `<svg>` gets. An external instance
 * takes that viewport when it is drawn, so a presented use without a shadow tree only drops its
 * own layout; otherwise the instance is invalidated as by \ref InvalidateUseInstance.
 *
 * @param handle The `<use>` element.
 */
void InvalidateUseGeometry(EntityHandle handle);

/**
 * Point a `<use>` at the reference its stored attributes resolve to, `href` taking precedence over
 * `xlink:href` (an empty `href` still wins and means no reference), then invalidate its instance.
 * A presented use that moves from one external document instance to another, and owns no shadow
 * tree, is only marked for a retarget: the next render attaches the new instance without
 * rebuilding the rest of the render tree.
 * This is the only DOM writer of a use element's reference, so the attributes and the rendered
 * target never disagree; SMIL animation of `href` overrides the reference temporarily on its own.
 *
 * @param handle The `<use>` element.
 */
void RetargetUseFromAttributes(EntityHandle handle);

}  // namespace donner::svg
