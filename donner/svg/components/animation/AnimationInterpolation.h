#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace donner::svg::components {

/// How keyframe values are sampled over time.
enum class CalcMode : uint8_t {
  Discrete,  ///< Hold a value until the next key time.
  Linear,    ///< Linear interpolation between keyframes.
  Paced,     ///< Constant speed according to value distances.
  Spline,    ///< Cubic Bezier timing between keyframes.
};

/// Shared keyframe timing and composition attributes.
struct AnimationInterpolation {
  CalcMode calcMode = CalcMode::Linear;  ///< Interpolation mode.
  std::vector<double> keyTimes;          ///< Normalized keyframe times.
  std::vector<double> keySplines;        ///< Four control coordinates per interval.
  bool additive = false;                 ///< Add to the underlying value.
  bool accumulate = false;               ///< Add completed iterations' final values.
  bool valid = true;                     ///< False after a malformed interpolation attribute.
};

/// Selected keyframe interval and its eased progress.
struct AnimationSegment {
  std::size_t from = 0;  ///< First keyframe index.
  std::size_t to = 0;    ///< Second index, equal to from for a discrete value.
  double progress = 0;   ///< Progress within the interval.
};

/** Validate keyframe timing and select the interval for a normalized time.
 * @param progress Normalized simple time.
 * @param count Number of effective values.
 * @param options Timing and validation state.
 * @param distances Distance for each consecutive pair, required for paced mode.
 * @return Selected interval, or nullopt for invalid timing.
 */
std::optional<AnimationSegment> SelectAnimationSegment(double progress, std::size_t count,
                                                       const AnimationInterpolation& options,
                                                       std::span<const double> distances = {});

}  // namespace donner::svg::components
