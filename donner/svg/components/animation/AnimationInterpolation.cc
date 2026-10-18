#include "donner/svg/components/animation/AnimationInterpolation.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace donner::svg::components {
namespace {

bool ValidKeyTimes(std::size_t count, const AnimationInterpolation& options) {
  if (options.keyTimes.empty() || options.calcMode == CalcMode::Paced) {
    return true;
  }
  if (options.keyTimes.size() != count || options.keyTimes.front() != 0 ||
      !std::is_sorted(options.keyTimes.begin(), options.keyTimes.end())) {
    return false;
  }
  for (double value : options.keyTimes) {
    if (!std::isfinite(value) || value < 0 || value > 1) {
      return false;
    }
  }
  return options.calcMode == CalcMode::Discrete || count == 1 || options.keyTimes.back() == 1;
}

bool ValidSplines(std::size_t count, const AnimationInterpolation& options) {
  if (options.calcMode != CalcMode::Spline || count == 1) {
    return true;
  }
  if (options.keySplines.size() != (count - 1) * 4) {
    return false;
  }
  return std::all_of(options.keySplines.begin(), options.keySplines.end(),
                     [](double value) { return std::isfinite(value) && value >= 0 && value <= 1; });
}

double Bezier(double t, double first, double second) {
  const double inverse = 1 - t;
  return 3 * inverse * inverse * t * first + 3 * inverse * t * t * second + t * t * t;
}

double SplineProgress(double progress, std::span<const double> controls) {
  double low = 0;
  double high = 1;
  // Bisection handles flat derivatives at either endpoint without a Newton fallback.
  for (int iteration = 0; iteration < 48; ++iteration) {
    const double middle = (low + high) * 0.5;
    if (Bezier(middle, controls[0], controls[2]) < progress) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return Bezier((low + high) * 0.5, controls[1], controls[3]);
}

std::optional<AnimationSegment> PacedSegment(double progress, std::size_t count,
                                             std::span<const double> distances) {
  if (distances.size() != count - 1) {
    return std::nullopt;
  }
  double total = 0;
  for (double distance : distances) {
    if (!std::isfinite(distance) || distance < 0) {
      return std::nullopt;
    }
    total += distance;
  }
  if (!std::isfinite(total)) {
    return std::nullopt;
  }
  if (total == 0) {
    return AnimationSegment{};
  }
  double remaining = progress * total;
  for (std::size_t i = 0; i < distances.size(); ++i) {
    if (distances[i] > 0 && (remaining < distances[i] || i + 1 == distances.size())) {
      return AnimationSegment{i, i + 1, std::clamp(remaining / distances[i], 0.0, 1.0)};
    }
    remaining -= distances[i];
  }
  return AnimationSegment{count - 1, count - 1, 0};
}

struct IntervalProgress {
  std::size_t interval = 0;
  double local = 0;
};

IntervalProgress SelectIntervalProgress(double progress, std::size_t count,
                                        const AnimationInterpolation& options, bool discrete) {
  if (options.keyTimes.empty()) {
    const double scaled = progress * static_cast<double>(discrete ? count : count - 1);
    return {std::min(static_cast<std::size_t>(scaled), count - 1),
            scaled - std::min(static_cast<std::size_t>(scaled), count - 1)};
  }
  const auto next = std::upper_bound(options.keyTimes.begin(), options.keyTimes.end(), progress);
  const std::size_t interval = static_cast<std::size_t>(next - options.keyTimes.begin() - 1);
  const double start = options.keyTimes[interval];
  const double end = interval + 1 < count ? options.keyTimes[interval + 1] : 1;
  return {interval, end > start ? (progress - start) / (end - start) : 1};
}

double ApplySplineProgress(double local, std::size_t interval,
                           const AnimationInterpolation& options) {
  return options.calcMode == CalcMode::Spline
             ? SplineProgress(local, std::span(options.keySplines).subspan(interval * 4, 4))
             : local;
}

}  // namespace

std::optional<AnimationSegment> SelectAnimationSegment(double progress, std::size_t count,
                                                       const AnimationInterpolation& options,
                                                       std::span<const double> distances) {
  if (!options.valid || count == 0 || !std::isfinite(progress) || !ValidKeyTimes(count, options) ||
      !ValidSplines(count, options)) {
    return std::nullopt;
  }
  progress = std::clamp(progress, 0.0, 1.0);
  if (count == 1 || progress == 1) {
    return AnimationSegment{count - 1, count - 1, 0};
  }
  if (options.calcMode == CalcMode::Paced) {
    return PacedSegment(progress, count, distances);
  }
  const bool discrete = options.calcMode == CalcMode::Discrete;
  const IntervalProgress selected = SelectIntervalProgress(progress, count, options, discrete);
  if (discrete || selected.interval + 1 == count) {
    return AnimationSegment{selected.interval, selected.interval, 0};
  }
  const double local = ApplySplineProgress(selected.local, selected.interval, options);
  return AnimationSegment{selected.interval, selected.interval + 1, local};
}

}  // namespace donner::svg::components
