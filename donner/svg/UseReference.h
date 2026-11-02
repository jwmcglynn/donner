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
 * Point a `<use>` at the reference its stored attributes resolve to, `href` taking precedence over
 * `xlink:href` (an empty `href` still wins and means no reference), then invalidate its instance.
 * This is the only DOM writer of a use element's reference, so the attributes and the rendered
 * target never disagree; SMIL animation of `href` overrides the reference temporarily on its own.
 *
 * @param handle The `<use>` element.
 */
void RetargetUseFromAttributes(EntityHandle handle);

}  // namespace donner::svg
