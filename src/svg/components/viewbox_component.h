#pragma once
/// @file

#include <optional>

#include "src/base/box.h"
#include "src/base/transform.h"
#include "src/svg/core/preserve_aspect_ratio.h"

namespace donner::svg::components {

/**
 * A component attached to entities that have a `viewbox` attribute, such as \ref xml_svg and \ref
 * xml_pattern.
 */
struct ViewboxComponent {
  /// Stored viewbox, if any.
  std::optional<Boxd> viewbox;

  /**
   * Computes the transform for the given Viewbox per
   * https://www.w3.org/TR/SVG2/coords.html#ComputingAViewportsTransform
   *
   * @param size The position and size of the element.
   * @param preserveAspectRatio The preserveAspectRatio property.
   */
  Transformd computeTransform(Boxd size, PreserveAspectRatio preserveAspectRatio) const;
};

}  // namespace donner::svg::components
