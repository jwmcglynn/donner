#pragma once
/// @file

#include "donner/base/Transform.h"
#include "donner/svg/core/CssTransform.h"
#include "donner/svg/properties/Property.h"

namespace donner::svg::components {

/**
 * Stores the raw transform value set on an entity, for the transform presentation attribute. This
 * can be sourced from the `transform="..."` XML attribute, or from the `transform` CSS property.
 */
struct TransformComponent {
  /// Value of the transform, if it is set. Defaults to `std::nullopt`. Represents the
  /// entity-from-parent transform.
  Property<CssTransform> transform{"transform",
                                   []() -> std::optional<CssTransform> { return std::nullopt; }};
};

/**
 * Stores the computed transform value for an entity, relative to the parent. This resolves
 * presentation attributes and the CSS cascade and stores the resulting value for the current
 * entity.
 */
struct ComputedLocalTransformComponent {
  Transformd entityFromParent;   //!< Transform from the entity from its parent.
  CssTransform rawCssTransform;  //!< Raw CSS transform value, before resolving percentages relative
                                 //!< to the viewport.
};

/**
 * Stores the computed transform value for an entity, relative to the world. This applies the
 * transform from the from all parent entities, and represents the transform of the entity from the
 * root.
 */
struct ComputedAbsoluteTransformComponent {
  Transformd entityFromWorld;  //!< Transform from the entity from the world.
  bool worldIsCanvas =
      true;  //<!< Set to false if this entity rebases the coordinate system and is not relative to the canvas.
};

}  // namespace donner::svg::components
