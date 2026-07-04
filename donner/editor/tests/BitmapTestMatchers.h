#pragma once
/// @file

#include <gmock/gmock.h>

#include "donner/svg/renderer/RendererInterface.h"

namespace donner::editor::tests {

/// Matches a non-empty \ref svg::RendererBitmap and prints shape details on failure.
MATCHER(NonEmptyRendererBitmap, "a non-empty renderer bitmap") {
  if (!arg.empty()) {
    return true;
  }

  *result_listener << "dimensions=(" << arg.dimensions.x << ", " << arg.dimensions.y
                   << "), rowBytes=" << arg.rowBytes << ", pixels=" << arg.pixels.size();
  return false;
}

}  // namespace donner::editor::tests
