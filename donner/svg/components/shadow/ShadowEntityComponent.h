#pragma once
/// @file

#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

/**
 * A component attached to entities in the shadow tree, indicating which light entity they are
 * mirroring. This component exists on every shadow entity, forming a one-to-one mapping between
 * shadow entities and light entities.
 */
struct ShadowEntityComponent {
  /// The entity that this shadow entity is mirroring.
  Entity lightEntity;
};

}  // namespace donner::svg::components
