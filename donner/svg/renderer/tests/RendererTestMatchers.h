#pragma once
/// @file

#include <gmock/gmock.h>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg::test {

/// Matches an empty \ref RendererBitmap and prints shape details on failure.
MATCHER(EmptyRendererBitmap, "an empty renderer bitmap") {
  if (arg.empty()) {
    return true;
  }

  *result_listener << "dimensions=(" << arg.dimensions.x << ", " << arg.dimensions.y
                   << "), rowBytes=" << arg.rowBytes << ", pixels=" << arg.pixels.size()
                   << ", alphaType=" << static_cast<int>(arg.alphaType);
  return false;
}

/// Matches a non-empty \ref RendererBitmap and prints shape details on failure.
MATCHER(NonEmptyRendererBitmap, "a non-empty renderer bitmap") {
  if (!arg.empty()) {
    return true;
  }

  *result_listener << "dimensions=(" << arg.dimensions.x << ", " << arg.dimensions.y
                   << "), rowBytes=" << arg.rowBytes << ", pixels=" << arg.pixels.size()
                   << ", alphaType=" << static_cast<int>(arg.alphaType);
  return false;
}

/// Matches a null \ref Entity and prints the entity value on failure.
MATCHER(NullEntity, "a null entity") {
  if (arg == entt::null) {
    return true;
  }

  *result_listener << "entity=" << arg;
  return false;
}

/// Matches a non-null \ref Entity and prints the entity value on failure.
MATCHER(NonNullEntity, "a non-null entity") {
  if (arg != entt::null) {
    return true;
  }

  *result_listener << "entity=" << arg;
  return false;
}

}  // namespace donner::svg::test
