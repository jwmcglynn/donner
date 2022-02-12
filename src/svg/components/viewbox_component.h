#pragma once

#include <optional>

#include "src/base/box.h"
#include "src/base/transform.h"
#include "src/svg/core/preserve_aspect_ratio.h"

namespace donner::svg {

struct ViewboxComponent {
  std::optional<Boxd> viewbox;
  PreserveAspectRatio preserveAspectRatio;

  /**
   * Computes the transform for the given Viewbox per
   * https://www.w3.org/TR/SVG2/coords.html#ComputingAViewportsTransform
   *
   * @param size The position and size of the element.
   */
  Transformd computeTransform(Boxd size) const;
};

}  // namespace donner::svg
