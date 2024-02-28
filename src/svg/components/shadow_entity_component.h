#pragma once
/// @file

#include "src/svg/registry/registry.h"

namespace donner::svg::components {

/**
 * A component attached to entities in the shadow tree, indicating which light entity they are
 * mirroring.
 */
struct ShadowEntityComponent {
  Entity lightEntity;
};

}  // namespace donner::svg::components
