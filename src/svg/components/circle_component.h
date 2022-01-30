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

  void computePath(ComputedPathComponent& component) {
    const Vector2d center(cx.value, cy.value);
    const double radius = r.value;

    component.setSpline(PathSpline::Builder().circle(center, radius).build());
  }
};

}  // namespace donner
