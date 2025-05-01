#pragma once
/// @file

#include "donner/base/EcsRegistry.h"

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

/**
 * Indicates root of an instantiated shadow tree, where the light entity is is the target of the
 * href, e.g. a \ref xml_symbol element.
 */
struct ShadowTreeRootComponent {
  /// The entity of the source (e.g. \ref xml_use) element that instantiated this shadow tree.
  Entity sourceEntity;
};

}  // namespace donner::svg::components
