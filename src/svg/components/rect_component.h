#pragma once

#include <optional>

#include "src/base/length.h"
#include "src/svg/components/computed_path_component.h"

namespace donner {

/**
 * Parameters for a <rect> element.
 */
struct RectComponent {
  Lengthd x;
  Lengthd y;
  Lengthd width;
  Lengthd height;
  std::optional<Lengthd> rx;
  std::optional<Lengthd> ry;

  void computePath(ComputedPathComponent& component) {
    Vector2d pos(x.value, y.value);
    Vector2d size(width.value, height.value);
    component.setSpline(PathSpline::Builder()
                            .moveTo(pos)
                            .lineTo(pos + Vector2d(size.x, 0))
                            .lineTo(pos + size)
                            .lineTo(pos + Vector2d(0, size.y))
                            .closePath()
                            .build());
  }
};

}  // namespace donner