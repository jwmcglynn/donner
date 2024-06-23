#pragma once
/// @file

#include <optional>

#include "donner/base/Box.h"
#include "donner/base/Transform.h"
#include "donner/svg/core/PreserveAspectRatio.h"

namespace donner::svg::components {

/**
 * A component attached to entities that have a `viewbox` attribute, such as \ref xml_svg and \ref
 * xml_pattern.
 */
struct ViewboxComponent {
  /// Stored viewbox, if any.
  std::optional<Boxd> viewbox;
};

}  // namespace donner::svg::components
