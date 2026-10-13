#pragma once
/// @file

#include <optional>
#include <string>
#include <vector>

#include "donner/svg/components/animation/AnimationInterpolation.h"

namespace donner::svg::components {

/// Declarative motion-path values and timing.
struct AnimateMotionComponent : AnimationInterpolation {
  /// Motion defaults to constant distance per unit time.
  AnimateMotionComponent() { calcMode = CalcMode::Paced; }
  std::optional<std::string> href;  ///< Optional animation target.
  std::optional<std::string> path;  ///< SVG path data.
  std::optional<std::string> from;  ///< Initial point.
  std::optional<std::string> to;    ///< Final point.
  std::optional<std::string> by;    ///< Point offset.
  std::vector<std::string> values;  ///< Point keyframes.
  std::vector<double> keyPoints;    ///< Normalized distances corresponding to keyTimes.
  std::string rotate = "0";         ///< Angle in degrees, auto, or auto-reverse.
};

/// Reference from an mpath child to a local path element.
struct MPathComponent {
  std::string href;  ///< Local fragment reference.
};

}  // namespace donner::svg::components
