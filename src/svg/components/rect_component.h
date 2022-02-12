#pragma once

#include <optional>

#include "src/base/length.h"
#include "src/svg/components/computed_path_component.h"
#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

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
                   const FontMetrics& fontMetrics);
};

}  // namespace donner::svg
