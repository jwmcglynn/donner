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

  void computePath(ComputedPathComponent& component, const Boxd& viewbox,
                   const FontMetrics& fontMetrics) {
    const Vector2d pos(x.toPixels(viewbox, fontMetrics), y.toPixels(viewbox, fontMetrics));
    const Vector2d size(width.toPixels(viewbox, fontMetrics),
                        height.toPixels(viewbox, fontMetrics));

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
