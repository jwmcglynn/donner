#pragma once
/// @file

#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/svg/core/CssTransform.h"
#include "donner/svg/properties/Property.h"

namespace donner::svg::components {

/**
 * Stores the raw transform value set on an entity, for the transform presentation attribute. This
 * can be sourced from the `transform="..."` XML attribute, or from the `transform` CSS property.
 */
struct TransformComponent {
  /// Parsed value of the `transform="…"` presentation attribute / CSS
  /// `transform` property. Applied to entity-local coords, returns
  /// parent-space coords (i.e. `parentFromEntity`).
  Property<CssTransform> transform{"transform",
                                   []() -> std::optional<CssTransform> { return std::nullopt; }};
};

/**
 * Stores the computed local transform for an entity (after CSS cascade +
 * percentage/viewport resolution).
 */
struct ComputedLocalTransformComponent {
  /// Local transform that maps entity coords to the parent's coord system.
  /// Applying `parentFromEntity` to a point expressed in the entity's own
  /// coord system yields its position in the parent coord system. For
  /// `<rect x=0 y=0 … transform="translate(100, 0)"/>`, this matrix is
  /// `Translate(100, 0)`, and the rect ends up at x=100 in the parent.
  Transform2d parentFromEntity;
  /// Raw CSS transform value, before resolving percentages relative to the viewport.
  CssTransform rawCssTransform;
  /// Resolved transform origin in pixels.
  Vector2d transformOrigin;
};

/**
 * Stores the computed absolute transform for an entity — the full cascade of
 * ancestor transforms composed together.
 */
struct ComputedAbsoluteTransformComponent {
  /// Transform that maps entity-local coords through every ancestor's local
  /// transform to world (canvas) coords. This is the cascade of every
  /// `parentFromEntity` up the tree, composed together.
  Transform2d worldFromEntity;
  /// Set to false if this entity rebases the coordinate system and is not
  /// relative to the canvas (e.g. sub-document boundaries).
  bool worldIsCanvas = true;
};

}  // namespace donner::svg::components
