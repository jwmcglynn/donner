#pragma once

#include "src/svg/components/registry.h"

namespace donner {

/**
 * A component attached to entities in the shadow tree, indicating which light entity they are
 * mirroring.
 */
struct ShadowEntityComponent {
  Entity lightEntity;
};

}  // namespace donner
