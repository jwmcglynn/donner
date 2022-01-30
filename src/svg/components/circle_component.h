#pragma once

#include "src/base/length.h"
#include "src/svg/components/computed_path_component.h"

namespace donner {

/**
 * Parameters for a <circle> element.
 */
struct CircleComponent {
  Lengthd cx;
  Lengthd cy;
  Lengthd r;

  void computePath(ComputedPathComponent& component, const Boxd& viewbox,
                   const FontMetrics& fontMetrics) {
    const Vector2d center(cx.toPixels(viewbox, fontMetrics), cy.toPixels(viewbox, fontMetrics));
    const double radius = r.toPixels(viewbox, fontMetrics);

    component.setSpline(PathSpline::Builder().circle(center, radius).build());
  }
};

}  // namespace donner
