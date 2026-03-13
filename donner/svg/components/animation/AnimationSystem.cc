#include "donner/svg/components/animation/AnimationSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/core/PathSpline.h"
#include "donner/svg/parser/PathParser.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationStateComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/svg/ElementType.h"
#include "donner/svg/components/ElementTypeComponent.h"

namespace donner::svg::components {

namespace {

/// Resolve the target entity for an animation by href or parent.
Entity resolveTargetByHrefOrParent(Registry& registry, Entity animationEntity,
                                   const std::optional<std::string>& href) {
  if (href.has_value()) {
    const auto& docContext = registry.ctx().get<SVGDocumentContext>();
    std::string_view id = href.value();
    if (!id.empty() && id[0] == '#') {
      id.remove_prefix(1);
    }
    return docContext.getEntityById(RcString(id));
  }

  if (auto* tree = registry.try_get<donner::components::TreeComponent>(animationEntity)) {
    return tree->parent();
  }

  return entt::null;
}

/// Compute the simple duration from the `dur` attribute.
/// For `<set>` without dur, returns indefinite. For `<animate>` without dur, returns 0 (inactive).
double computeSimpleDuration(const AnimationTimingComponent& timing, bool isSetElement) {
  if (timing.dur.has_value()) {
    return timing.dur->seconds();
  }
  // Per spec: if dur is not specified, simple duration is indefinite for <set>,
  // but for <animate> there's no animation effect without dur.
  return isSetElement ? std::numeric_limits<double>::infinity() : 0.0;
}

/// Compute the active duration from timing attributes.
double computeActiveDuration(const AnimationTimingComponent& timing, double simpleDuration) {
  if (simpleDuration == 0.0) {
    return 0.0;
  }

  double repeatDuration = std::numeric_limits<double>::infinity();
  if (timing.repeatCount.has_value() && std::isfinite(simpleDuration)) {
    repeatDuration = simpleDuration * timing.repeatCount.value();
  }
  if (timing.repeatDur.has_value()) {
    repeatDuration = std::min(repeatDuration, timing.repeatDur->seconds());
  }

  if (!timing.repeatCount.has_value() && !timing.repeatDur.has_value()) {
    repeatDuration = simpleDuration;
  }

  double activeDuration = repeatDuration;

  if (timing.endOffset.has_value()) {
    double endTime = timing.endOffset->seconds();
    double beginTime = timing.beginOffset.has_value() ? timing.beginOffset->seconds() : 0.0;
    activeDuration = std::min(activeDuration, endTime - beginTime);
  }

  if (timing.min.has_value()) {
    activeDuration = std::max(activeDuration, timing.min->seconds());
  }
  if (timing.max.has_value()) {
    activeDuration = std::min(activeDuration, timing.max->seconds());
  }

  return activeDuration;
}

/// Determine the animation phase given the current document time.
AnimationPhase computePhase(double documentTime, double beginTime, double activeDuration) {
  if (documentTime < beginTime) {
    return AnimationPhase::Before;
  }
  double elapsed = documentTime - beginTime;
  if (std::isfinite(activeDuration) && elapsed >= activeDuration) {
    return AnimationPhase::After;
  }
  return AnimationPhase::Active;
}

/// Compute timing state for an animation entity.
void computeTimingState(AnimationStateComponent& state, const AnimationTimingComponent& timing,
                        double documentTime, bool isSetElement) {
  state.beginTime = timing.beginOffset.has_value() ? timing.beginOffset->seconds() : 0.0;
  state.simpleDuration = computeSimpleDuration(timing, isSetElement);
  state.activeDuration = computeActiveDuration(timing, state.simpleDuration);
  AnimationPhase newPhase = computePhase(documentTime, state.beginTime, state.activeDuration);

  // Enforce restart attribute.
  // restart="never": once the animation has completed, it cannot restart.
  if (timing.restart == AnimationRestart::Never && state.hasCompleted) {
    // Keep the After phase — animation stays frozen or removed.
    newPhase = AnimationPhase::After;
  }
  // restart="whenNotActive": can only restart when not currently active.
  // If the animation was active and is trying to restart (Before→Active transition at a new
  // begin time), suppress the restart while it was active in the previous frame.
  if (timing.restart == AnimationRestart::WhenNotActive && state.wasActive &&
      newPhase == AnimationPhase::Active && state.phase == AnimationPhase::After) {
    // Was active, ended, now a new begin wants to make it active again — suppress.
    newPhase = AnimationPhase::After;
  }

  // Track completion for restart="never".
  if (newPhase == AnimationPhase::After && state.phase == AnimationPhase::Active) {
    state.hasCompleted = true;
  }

  state.wasActive = (newPhase == AnimationPhase::Active);
  state.phase = newPhase;
}

/// Resolve syncbase begin time for an animation.
/// If the animation has a syncbase begin reference, look up the referenced animation's
/// begin or end time and compute this animation's begin time from it.
/// Returns true if the begin time was resolved (or no syncbase exists).
bool resolveSyncbaseBeginTime(Registry& registry, const AnimationTimingComponent& timing,
                              AnimationStateComponent& state) {
  if (!timing.beginSyncbase.has_value()) {
    return true;  // No syncbase, nothing to resolve.
  }

  const auto& syncRef = timing.beginSyncbase.value();
  const auto& docContext = registry.ctx().get<SVGDocumentContext>();
  Entity refEntity = docContext.getEntityById(RcString(syncRef.id));
  if (refEntity == entt::null) {
    return false;  // Referenced element not found.
  }

  // The referenced element might not be the animation itself — it could be the target element.
  // We need to find the animation entity that has this ID. In SVG, animation elements can have
  // IDs, so getEntityById should return the animation entity directly.
  auto* refState = registry.try_get<AnimationStateComponent>(refEntity);
  if (!refState) {
    return false;  // Referenced element has no animation state.
  }

  double refTime = 0.0;
  switch (syncRef.event) {
    case SyncbaseRef::Event::Begin:
      refTime = refState->beginTime;
      break;
    case SyncbaseRef::Event::End:
      refTime = refState->beginTime + refState->activeDuration;
      break;
  }

  state.beginTime = refTime + syncRef.offsetSeconds;
  return true;
}

/// Resolve syncbase end time for an animation.
bool resolveSyncbaseEndTime(Registry& registry, const AnimationTimingComponent& timing,
                            AnimationStateComponent& state) {
  if (!timing.endSyncbase.has_value()) {
    return true;
  }

  const auto& syncRef = timing.endSyncbase.value();
  const auto& docContext = registry.ctx().get<SVGDocumentContext>();
  Entity refEntity = docContext.getEntityById(RcString(syncRef.id));
  if (refEntity == entt::null) {
    return false;
  }

  auto* refState = registry.try_get<AnimationStateComponent>(refEntity);
  if (!refState) {
    return false;
  }

  double refTime = 0.0;
  switch (syncRef.event) {
    case SyncbaseRef::Event::Begin:
      refTime = refState->beginTime;
      break;
    case SyncbaseRef::Event::End:
      refTime = refState->beginTime + refState->activeDuration;
      break;
  }

  // Override the active duration based on resolved end time.
  double endTime = refTime + syncRef.offsetSeconds;
  state.activeDuration = std::max(0.0, endTime - state.beginTime);
  return true;
}

/// Returns true if the animation value should be applied (active or frozen).
bool shouldApplyValue(AnimationPhase phase, AnimationFill fill) {
  return phase == AnimationPhase::Active ||
         (phase == AnimationPhase::After && fill == AnimationFill::Freeze);
}

/// Try to parse a string as a double. Returns false on failure.
bool tryParseDouble(const std::string& str, double& out) {
  if (str.empty()) {
    return false;
  }
  char* endPtr = nullptr;
  out = std::strtod(str.c_str(), &endPtr);
  if (endPtr == str.c_str()) {
    return false;
  }
  // Skip trailing whitespace.
  while (*endPtr == ' ' || *endPtr == '\t') {
    ++endPtr;
  }
  // Only succeed if the entire string was consumed (single number, not a list).
  return *endPtr == '\0';
}

/// Evaluate a cubic Bezier curve at parameter t.
/// The curve is defined by control points (0,0), (x1,y1), (x2,y2), (1,1).
/// Returns the y value for a given x using Newton-Raphson iteration to invert x(t).
double evaluateCubicBezier(double x1, double y1, double x2, double y2, double x) {
  if (x <= 0.0) {
    return 0.0;
  }
  if (x >= 1.0) {
    return 1.0;
  }

  // Newton-Raphson to find t where bezierX(t) = x.
  // bezierX(t) = 3*(1-t)^2*t*x1 + 3*(1-t)*t^2*x2 + t^3
  double t = x;  // Initial guess.
  for (int i = 0; i < 8; ++i) {
    double t2 = t * t;
    double t3 = t2 * t;
    double mt = 1.0 - t;
    double mt2 = mt * mt;

    double xAtT = 3.0 * mt2 * t * x1 + 3.0 * mt * t2 * x2 + t3;
    double dx = xAtT - x;
    if (std::abs(dx) < 1e-7) {
      break;
    }

    // Derivative: dx/dt = 3*(1-t)^2*x1 + 6*(1-t)*t*(x2-x1) + 3*t^2*(1-x2)
    double dxdt = 3.0 * mt2 * x1 + 6.0 * mt * t * (x2 - x1) + 3.0 * t2 * (1.0 - x2);
    if (std::abs(dxdt) < 1e-12) {
      break;
    }

    t -= dx / dxdt;
    t = std::clamp(t, 0.0, 1.0);
  }

  // Evaluate y at t.
  double mt = 1.0 - t;
  return 3.0 * mt * mt * t * y1 + 3.0 * mt * t * t * y2 + t * t * t;
}

// Forward declaration — defined later in the file.
std::vector<double> parseNumbers(const std::string& str);

/// Compute cumulative distances between consecutive numeric values.
/// Returns a vector of size numValues with distances[0] = 0.
/// Returns empty if any value is non-numeric.
std::vector<double> computeCumulativeDistances(
    const std::vector<const std::string*>& effectiveValues) {
  std::vector<double> distances;
  distances.reserve(effectiveValues.size());
  distances.push_back(0.0);

  for (size_t i = 1; i < effectiveValues.size(); ++i) {
    double prev = 0.0;
    double curr = 0.0;
    if (tryParseDouble(*effectiveValues[i - 1], prev) &&
        tryParseDouble(*effectiveValues[i], curr)) {
      distances.push_back(distances.back() + std::abs(curr - prev));
      continue;
    }

    // Try number-list distance (Euclidean).
    auto prevNums = parseNumbers(*effectiveValues[i - 1]);
    auto currNums = parseNumbers(*effectiveValues[i]);
    if (prevNums.size() >= 2 && prevNums.size() == currNums.size()) {
      double sumSq = 0.0;
      for (size_t j = 0; j < prevNums.size(); ++j) {
        double d = currNums[j] - prevNums[j];
        sumSq += d * d;
      }
      distances.push_back(distances.back() + std::sqrt(sumSq));
      continue;
    }

    return {};  // Non-numeric, can't compute distances.
  }
  return distances;
}

/// Format a double as string, avoiding unnecessary trailing zeros.
std::string formatDouble(double value) {
  // Use a fixed precision that avoids scientific notation for typical SVG values.
  std::ostringstream oss;
  oss.precision(6);
  oss << value;
  return oss.str();
}

/// Compute the current repeat iteration index (0-based).
int computeIteration(double documentTime, double beginTime, double simpleDuration,
                     double activeDuration) {
  double elapsed = documentTime - beginTime;
  if (std::isfinite(activeDuration)) {
    elapsed = std::min(elapsed, activeDuration);
  }
  if (!std::isfinite(simpleDuration) || simpleDuration <= 0.0) {
    return 0;
  }
  int iteration = static_cast<int>(elapsed / simpleDuration);
  // At exact end of last iteration, count as previous iteration.
  if (std::fmod(elapsed, simpleDuration) == 0.0 && elapsed > 0.0) {
    iteration = std::max(0, iteration - 1);
  }
  return iteration;
}

/// Compute the interpolation progress [0,1] within the active interval.
/// Handles repeat iterations and keyTimes.
double computeProgress(double documentTime, double beginTime, double simpleDuration,
                       double activeDuration, const AnimateValueComponent& valueComp) {
  double elapsed = documentTime - beginTime;

  // Clamp to active duration.
  if (std::isfinite(activeDuration)) {
    elapsed = std::min(elapsed, activeDuration);
  }

  // Compute simple time within current iteration.
  double simpleTime = elapsed;
  if (std::isfinite(simpleDuration) && simpleDuration > 0.0) {
    // Handle repeat: simpleTime = elapsed mod simpleDuration
    simpleTime = std::fmod(elapsed, simpleDuration);
    // At exact end of last iteration, use full duration.
    if (simpleTime == 0.0 && elapsed > 0.0) {
      simpleTime = simpleDuration;
    }
  }

  // Compute progress [0,1] within simple duration.
  double progress = 0.0;
  if (std::isfinite(simpleDuration) && simpleDuration > 0.0) {
    progress = simpleTime / simpleDuration;
  }

  progress = std::clamp(progress, 0.0, 1.0);
  return progress;
}

/// Check if two PathSplines have structurally compatible command sequences for interpolation.
bool arePathsCompatible(const PathSpline& a, const PathSpline& b) {
  const auto& aCmds = a.commands();
  const auto& bCmds = b.commands();
  if (aCmds.size() != bCmds.size()) {
    return false;
  }
  for (size_t i = 0; i < aCmds.size(); ++i) {
    if (aCmds[i].type != bCmds[i].type) {
      return false;
    }
  }
  return a.points().size() == b.points().size();
}

/// Interpolate two structurally compatible PathSplines at parameter t.
/// Returns the interpolated path as an SVG path data string.
std::string interpolatePaths(const PathSpline& from, const PathSpline& to, double t) {
  std::ostringstream oss;
  oss.precision(6);

  const auto& cmds = from.commands();
  const auto& fromPts = from.points();
  const auto& toPts = to.points();

  for (size_t i = 0; i < cmds.size(); ++i) {
    if (i > 0) {
      oss << ' ';
    }

    switch (cmds[i].type) {
      case PathSpline::CommandType::MoveTo: {
        size_t pi = cmds[i].pointIndex;
        double x = fromPts[pi].x + (toPts[pi].x - fromPts[pi].x) * t;
        double y = fromPts[pi].y + (toPts[pi].y - fromPts[pi].y) * t;
        oss << "M" << x << "," << y;
        break;
      }
      case PathSpline::CommandType::LineTo: {
        size_t pi = cmds[i].pointIndex;
        double x = fromPts[pi].x + (toPts[pi].x - fromPts[pi].x) * t;
        double y = fromPts[pi].y + (toPts[pi].y - fromPts[pi].y) * t;
        oss << "L" << x << "," << y;
        break;
      }
      case PathSpline::CommandType::CurveTo: {
        size_t pi = cmds[i].pointIndex;
        for (int j = 0; j < 3; ++j) {
          double x = fromPts[pi + j].x + (toPts[pi + j].x - fromPts[pi + j].x) * t;
          double y = fromPts[pi + j].y + (toPts[pi + j].y - fromPts[pi + j].y) * t;
          if (j == 0) {
            oss << "C";
          } else {
            oss << " ";
          }
          oss << x << "," << y;
        }
        break;
      }
      case PathSpline::CommandType::ClosePath:
        oss << "Z";
        break;
    }
  }

  return oss.str();
}

/// Try to interpolate SVG path data strings.
/// Returns empty string if paths are not structurally compatible.
std::string tryInterpolatePaths(const std::string& fromStr, const std::string& toStr, double t) {
  auto fromResult = parser::PathParser::Parse(fromStr);
  auto toResult = parser::PathParser::Parse(toStr);
  if (!fromResult.hasResult() || !toResult.hasResult()) {
    return {};
  }

  const auto& fromPath = fromResult.result();
  const auto& toPath = toResult.result();

  if (!arePathsCompatible(fromPath, toPath)) {
    return {};
  }

  return interpolatePaths(fromPath, toPath, t);
}

/// Interpolate a value for `<animate>` at the given progress.
/// Returns the interpolated value as a string, or empty if interpolation failed.
std::string interpolateAnimateValue(double progress, const AnimateValueComponent& valueComp) {
  // Build the effective value list.
  std::vector<const std::string*> effectiveValues;

  if (!valueComp.values.empty()) {
    for (const auto& v : valueComp.values) {
      effectiveValues.push_back(&v);
    }
  } else if (valueComp.from.has_value() && valueComp.to.has_value()) {
    effectiveValues.push_back(&valueComp.from.value());
    effectiveValues.push_back(&valueComp.to.value());
  } else if (valueComp.to.has_value()) {
    // to-only animation: use "to" as a 1-value list with discrete behavior.
    return valueComp.to.value();
  } else if (valueComp.from.has_value() && valueComp.by.has_value()) {
    // from/by: try numeric addition.
    double fromVal = 0.0;
    double byVal = 0.0;
    if (tryParseDouble(valueComp.from.value(), fromVal) &&
        tryParseDouble(valueComp.by.value(), byVal)) {
      double result = fromVal + (byVal * progress);
      return formatDouble(result);
    }
    return valueComp.from.value();
  } else {
    return {};
  }

  if (effectiveValues.size() < 2) {
    return effectiveValues.empty() ? std::string{} : *effectiveValues[0];
  }

  // Discrete mode: jump between values.
  if (valueComp.calcMode == CalcMode::Discrete) {
    size_t numValues = effectiveValues.size();
    size_t index = static_cast<size_t>(progress * static_cast<double>(numValues));
    if (index >= numValues) {
      index = numValues - 1;
    }
    return *effectiveValues[index];
  }

  size_t numValues = effectiveValues.size();
  size_t numIntervals = numValues - 1;

  // Paced mode: distribute values at constant velocity using cumulative distances.
  if (valueComp.calcMode == CalcMode::Paced) {
    auto distances = computeCumulativeDistances(effectiveValues);
    if (!distances.empty() && distances.back() > 0.0) {
      double totalDist = distances.back();
      double targetDist = progress * totalDist;

      // Find the interval containing targetDist.
      size_t interval = 0;
      for (size_t i = 1; i < distances.size(); ++i) {
        if (targetDist <= distances[i] || i == distances.size() - 1) {
          interval = i - 1;
          break;
        }
      }

      double segStart = distances[interval];
      double segEnd = distances[interval + 1];
      double segLen = segEnd - segStart;
      double localT = (segLen > 0.0) ? (targetDist - segStart) / segLen : 0.0;
      localT = std::clamp(localT, 0.0, 1.0);

      double fromVal = 0.0;
      double toVal = 0.0;
      if (tryParseDouble(*effectiveValues[interval], fromVal) &&
          tryParseDouble(*effectiveValues[interval + 1], toVal)) {
        return formatDouble(fromVal + (toVal - fromVal) * localT);
      }

      // Try number-list interpolation for paced mode.
      auto fromNums = parseNumbers(*effectiveValues[interval]);
      auto toNums = parseNumbers(*effectiveValues[interval + 1]);
      if (fromNums.size() >= 2 && fromNums.size() == toNums.size()) {
        std::ostringstream oss;
        oss.precision(6);
        for (size_t j = 0; j < fromNums.size(); ++j) {
          if (j > 0) {
            oss << " ";
          }
          oss << (fromNums[j] + (toNums[j] - fromNums[j]) * localT);
        }
        return oss.str();
      }

      return (localT < 0.5) ? *effectiveValues[interval] : *effectiveValues[interval + 1];
    }
    // Fall through to linear if distances can't be computed.
  }

  // Linear/Spline mode: interpolate between adjacent values.
  // Use keyTimes if provided, otherwise evenly space.
  size_t interval = 0;
  double localT = 0.0;

  if (!valueComp.keyTimes.empty() && valueComp.keyTimes.size() == numValues) {
    // Use keyTimes to determine interval.
    for (size_t i = 0; i < numIntervals; ++i) {
      if (progress <= valueComp.keyTimes[i + 1] || i == numIntervals - 1) {
        interval = i;
        double keyStart = valueComp.keyTimes[i];
        double keyEnd = valueComp.keyTimes[i + 1];
        double keyRange = keyEnd - keyStart;
        localT = (keyRange > 0.0) ? (progress - keyStart) / keyRange : 0.0;
        break;
      }
    }
  } else {
    // Evenly spaced.
    double scaled = progress * static_cast<double>(numIntervals);
    interval = static_cast<size_t>(scaled);
    if (interval >= numIntervals) {
      interval = numIntervals - 1;
    }
    localT = scaled - static_cast<double>(interval);
  }

  localT = std::clamp(localT, 0.0, 1.0);

  // Spline mode: apply cubic Bezier easing to localT.
  if (valueComp.calcMode == CalcMode::Spline && valueComp.keySplines.size() >= (interval + 1) * 4) {
    size_t base = interval * 4;
    localT = evaluateCubicBezier(valueComp.keySplines[base], valueComp.keySplines[base + 1],
                                 valueComp.keySplines[base + 2], valueComp.keySplines[base + 3],
                                 localT);
  }

  // Try numeric interpolation.
  double fromVal = 0.0;
  double toVal = 0.0;
  if (tryParseDouble(*effectiveValues[interval], fromVal) &&
      tryParseDouble(*effectiveValues[interval + 1], toVal)) {
    double result = fromVal + (toVal - fromVal) * localT;
    return formatDouble(result);
  }

  // Try path data interpolation (for animating the 'd' attribute).
  auto pathResult = tryInterpolatePaths(*effectiveValues[interval],
                                        *effectiveValues[interval + 1], localT);
  if (!pathResult.empty()) {
    return pathResult;
  }

  // Try number-list interpolation (for dasharray, points, viewBox, etc.).
  {
    auto fromNums = parseNumbers(*effectiveValues[interval]);
    auto toNums = parseNumbers(*effectiveValues[interval + 1]);
    if (fromNums.size() >= 2 && fromNums.size() == toNums.size()) {
      std::ostringstream oss;
      oss.precision(6);
      for (size_t i = 0; i < fromNums.size(); ++i) {
        if (i > 0) {
          oss << " ";
        }
        oss << (fromNums[i] + (toNums[i] - fromNums[i]) * localT);
      }
      return oss.str();
    }
  }

  // Fall back to discrete for non-interpolable values.
  return (localT < 0.5) ? *effectiveValues[interval] : *effectiveValues[interval + 1];
}

/// Parse a space-separated list of doubles from a string.
std::vector<double> parseNumbers(const std::string& str) {
  std::vector<double> result;
  const char* ptr = str.c_str();
  const char* end = ptr + str.size();
  while (ptr < end) {
    while (ptr < end && (*ptr == ' ' || *ptr == '\t' || *ptr == ',')) {
      ++ptr;
    }
    if (ptr >= end) {
      break;
    }
    char* endPtr = nullptr;
    double val = std::strtod(ptr, &endPtr);
    if (endPtr == ptr) {
      break;
    }
    result.push_back(val);
    ptr = endPtr;
  }
  return result;
}

/// Get the expected number of values per entry for a given transform type.
int expectedNumberCount(TransformAnimationType type) {
  switch (type) {
    case TransformAnimationType::Translate: return 2;  // tx [ty]
    case TransformAnimationType::Scale: return 2;      // sx [sy]
    case TransformAnimationType::Rotate: return 3;     // angle [cx cy]
    case TransformAnimationType::SkewX: return 1;      // angle
    case TransformAnimationType::SkewY: return 1;      // angle
  }
  return 1;
}

/// Pad a number list to the expected size with defaults.
void padToExpected(std::vector<double>& nums, TransformAnimationType type) {
  int expected = expectedNumberCount(type);
  if (type == TransformAnimationType::Translate && nums.size() == 1) {
    nums.push_back(0.0);  // ty defaults to 0
  } else if (type == TransformAnimationType::Scale && nums.size() == 1) {
    nums.push_back(nums[0]);  // sy defaults to sx
  } else if (type == TransformAnimationType::Rotate && nums.size() == 1) {
    nums.push_back(0.0);  // cx defaults to 0
    nums.push_back(0.0);  // cy defaults to 0
  }
  while (static_cast<int>(nums.size()) < expected) {
    nums.push_back(0.0);
  }
}

/// Format a transform string from type and interpolated values.
std::string formatTransform(TransformAnimationType type, const std::vector<double>& values) {
  std::ostringstream oss;
  oss.precision(6);

  switch (type) {
    case TransformAnimationType::Translate:
      oss << "translate(" << values[0];
      if (values.size() > 1 && values[1] != 0.0) {
        oss << ", " << values[1];
      }
      oss << ")";
      break;
    case TransformAnimationType::Scale:
      oss << "scale(" << values[0];
      if (values.size() > 1 && values[1] != values[0]) {
        oss << ", " << values[1];
      }
      oss << ")";
      break;
    case TransformAnimationType::Rotate:
      oss << "rotate(" << values[0];
      if (values.size() > 2 && (values[1] != 0.0 || values[2] != 0.0)) {
        oss << ", " << values[1] << ", " << values[2];
      }
      oss << ")";
      break;
    case TransformAnimationType::SkewX:
      oss << "skewX(" << values[0] << ")";
      break;
    case TransformAnimationType::SkewY:
      oss << "skewY(" << values[0] << ")";
      break;
  }
  return oss.str();
}

/// Interpolate transform values for <animateTransform>.
std::string interpolateTransformValue(double progress,
                                      const AnimateTransformComponent& transformComp) {
  // Build effective value list.
  std::vector<const std::string*> effectiveValues;

  if (!transformComp.values.empty()) {
    for (const auto& v : transformComp.values) {
      effectiveValues.push_back(&v);
    }
  } else if (transformComp.from.has_value() && transformComp.to.has_value()) {
    effectiveValues.push_back(&transformComp.from.value());
    effectiveValues.push_back(&transformComp.to.value());
  } else if (transformComp.to.has_value()) {
    effectiveValues.push_back(&transformComp.to.value());
  } else {
    return {};
  }

  if (effectiveValues.empty()) {
    return {};
  }

  if (effectiveValues.size() == 1) {
    auto nums = parseNumbers(*effectiveValues[0]);
    padToExpected(nums, transformComp.type);
    return formatTransform(transformComp.type, nums);
  }

  // Find interval and local t (evenly spaced).
  size_t numValues = effectiveValues.size();
  size_t numIntervals = numValues - 1;
  double scaled = progress * static_cast<double>(numIntervals);
  size_t interval = static_cast<size_t>(scaled);
  if (interval >= numIntervals) {
    interval = numIntervals - 1;
  }
  double localT = std::clamp(scaled - static_cast<double>(interval), 0.0, 1.0);

  // Parse and interpolate the number tuples.
  auto fromNums = parseNumbers(*effectiveValues[interval]);
  auto toNums = parseNumbers(*effectiveValues[interval + 1]);
  padToExpected(fromNums, transformComp.type);
  padToExpected(toNums, transformComp.type);

  size_t count = std::min(fromNums.size(), toNums.size());
  std::vector<double> result(count);
  for (size_t i = 0; i < count; ++i) {
    result[i] = fromNums[i] + (toNums[i] - fromNums[i]) * localT;
  }

  return formatTransform(transformComp.type, result);
}

/// Parse a point "x y" or "x,y" from a string. Returns {0,0} on failure.
Vector2d parsePoint(const std::string& str) {
  auto nums = parseNumbers(str);
  if (nums.size() >= 2) {
    return Vector2d(nums[0], nums[1]);
  }
  if (nums.size() == 1) {
    return Vector2d(nums[0], 0.0);
  }
  return Vector2d(0.0, 0.0);
}

/// Compute segment lengths for a PathSpline.
std::vector<double> computeSegmentLengths(const PathSpline& spline) {
  std::vector<double> lengths;
  const auto& commands = spline.commands();
  for (size_t i = 0; i < commands.size(); ++i) {
    if (commands[i].type == PathSpline::CommandType::MoveTo) {
      lengths.push_back(0.0);
    } else {
      // Use pointAt sampling to estimate segment length.
      double len = 0.0;
      constexpr int kSamples = 32;
      Vector2d prev = spline.pointAt(i, 0.0);
      for (int s = 1; s <= kSamples; ++s) {
        double t = static_cast<double>(s) / kSamples;
        Vector2d pt = spline.pointAt(i, t);
        len += (pt - prev).length();
        prev = pt;
      }
      lengths.push_back(len);
    }
  }
  return lengths;
}

/// Evaluate a point and tangent on a PathSpline at global progress [0,1].
/// Uses arc-length parameterization.
struct MotionResult {
  Vector2d position;
  double angle = 0.0;  // Tangent angle in degrees.
};

MotionResult evaluateMotionOnPath(const PathSpline& spline, double progress) {
  const auto& commands = spline.commands();
  if (commands.empty()) {
    return {Vector2d(0, 0), 0.0};
  }

  auto segLengths = computeSegmentLengths(spline);
  double totalLength = 0.0;
  for (double l : segLengths) {
    totalLength += l;
  }

  if (totalLength <= 0.0) {
    // Degenerate path - just return first point.
    return {spline.pointAt(0, 0.0), 0.0};
  }

  double targetLen = progress * totalLength;
  double accumulated = 0.0;

  for (size_t i = 0; i < commands.size(); ++i) {
    if (segLengths[i] <= 0.0) {
      continue;
    }

    if (accumulated + segLengths[i] >= targetLen || i == commands.size() - 1) {
      double localProgress = (segLengths[i] > 0.0)
                                 ? (targetLen - accumulated) / segLengths[i]
                                 : 0.0;
      localProgress = std::clamp(localProgress, 0.0, 1.0);

      Vector2d pos = spline.pointAt(i, localProgress);
      Vector2d tangent = spline.tangentAt(i, localProgress);
      double angle = std::atan2(tangent.y, tangent.x) * 180.0 / M_PI;

      return {pos, angle};
    }
    accumulated += segLengths[i];
  }

  // Shouldn't reach here, but return end point.
  size_t last = commands.size() - 1;
  Vector2d pos = spline.pointAt(last, 1.0);
  Vector2d tangent = spline.tangentAt(last, 1.0);
  double angle = std::atan2(tangent.y, tangent.x) * 180.0 / M_PI;
  return {pos, angle};
}

/// Resolve an <mpath> child of an <animateMotion> entity and return its path data.
std::optional<std::string> resolveMPathChild(Registry& registry, Entity animateMotionEntity) {
  auto* tree = registry.try_get<donner::components::TreeComponent>(animateMotionEntity);
  if (!tree) {
    return std::nullopt;
  }

  // Walk children looking for an <mpath> element.
  for (Entity child = tree->firstChild(); child != entt::null;
       child = registry.get<donner::components::TreeComponent>(child).nextSibling()) {
    auto* typeComp = registry.try_get<ElementTypeComponent>(child);
    if (typeComp && typeComp->type() == ElementType::MPath) {
      // Found <mpath>. Get its href attribute to find the referenced <path>.
      auto* attrs = registry.try_get<donner::components::AttributesComponent>(child);
      if (!attrs) {
        continue;
      }

      auto href = attrs->getAttribute(xml::XMLQualifiedNameRef("href"));
      if (!href.has_value()) {
        href = attrs->getAttribute(xml::XMLQualifiedNameRef("xlink", "href"));
      }
      if (!href.has_value()) {
        continue;
      }

      // Resolve the referenced element by ID.
      std::string_view id = href.value();
      if (!id.empty() && id[0] == '#') {
        id.remove_prefix(1);
      }

      const auto& docContext = registry.ctx().get<SVGDocumentContext>();
      Entity refEntity = docContext.getEntityById(RcString(id));
      if (refEntity == entt::null) {
        continue;
      }

      // Get the `d` attribute from the referenced element.
      auto* refAttrs = registry.try_get<donner::components::AttributesComponent>(refEntity);
      if (!refAttrs) {
        continue;
      }

      auto pathData = refAttrs->getAttribute(xml::XMLQualifiedNameRef("d"));
      if (pathData.has_value()) {
        return std::string(pathData.value());
      }
    }
  }

  return std::nullopt;
}

/// Build a PathSpline from animateMotion from/to/by/values, path, or mpath child.
std::optional<PathSpline> buildMotionPath(Registry& registry, Entity animateMotionEntity,
                                          const AnimateMotionComponent& motionComp) {
  // <mpath> child takes highest precedence per SVG spec.
  auto mpathData = resolveMPathChild(registry, animateMotionEntity);
  if (mpathData.has_value()) {
    auto result = parser::PathParser::Parse(mpathData.value());
    if (result.hasResult()) {
      return std::move(result.result());
    }
  }

  if (motionComp.path.has_value()) {
    auto result = parser::PathParser::Parse(motionComp.path.value());
    if (result.hasResult()) {
      return std::move(result.result());
    }
    return std::nullopt;
  }

  // Build implicit linear path from from/to/by/values.
  PathSpline spline;

  if (!motionComp.values.empty()) {
    // values is a list of points.
    for (size_t i = 0; i < motionComp.values.size(); ++i) {
      Vector2d pt = parsePoint(motionComp.values[i]);
      if (i == 0) {
        spline.moveTo(pt);
      } else {
        spline.lineTo(pt);
      }
    }
    return spline;
  }

  if (motionComp.from.has_value() && motionComp.to.has_value()) {
    Vector2d from = parsePoint(motionComp.from.value());
    Vector2d to = parsePoint(motionComp.to.value());
    spline.moveTo(from);
    spline.lineTo(to);
    return spline;
  }

  if (motionComp.from.has_value() && motionComp.by.has_value()) {
    Vector2d from = parsePoint(motionComp.from.value());
    Vector2d by = parsePoint(motionComp.by.value());
    spline.moveTo(from);
    spline.lineTo(from + by);
    return spline;
  }

  if (motionComp.to.has_value()) {
    Vector2d to = parsePoint(motionComp.to.value());
    spline.moveTo(Vector2d(0, 0));
    spline.lineTo(to);
    return spline;
  }

  if (motionComp.by.has_value()) {
    Vector2d by = parsePoint(motionComp.by.value());
    spline.moveTo(Vector2d(0, 0));
    spline.lineTo(by);
    return spline;
  }

  return std::nullopt;
}

/// Map animation progress to path progress using keyPoints/keyTimes.
double applyKeyPoints(double progress, const AnimateMotionComponent& motionComp,
                      const AnimationTimingComponent& timing) {
  if (motionComp.keyPoints.empty() || motionComp.keyPoints.size() < 2) {
    return progress;
  }

  // keyPoints requires keyTimes of the same size to function.
  // Use timing component's keyTimes (stored during parsing as beginValue/etc).
  // Actually, keyTimes for animateMotion is stored in the AnimateMotionComponent
  // or parsed from timing. Let's check if timing has what we need.
  // In SVG, keyTimes is on the animation element — we need to find it.
  // For <animateMotion>, keyTimes is parsed into valueComp (if it exists) but
  // animateMotion doesn't have an AnimateValueComponent. We'll use a local search.

  // Look for keyTimes data. For animateMotion the keyTimes should match keyPoints length.
  // We'll interpolate between keyPoints entries using keyTimes as the input domain.
  // Without explicit keyTimes, assume evenly spaced.
  const auto& kp = motionComp.keyPoints;
  size_t n = kp.size();
  size_t numIntervals = n - 1;

  // Find which interval the progress falls into (evenly-spaced keyTimes if none provided).
  size_t interval = 0;
  double localT = 0.0;

  // Evenly spaced.
  double scaled = progress * static_cast<double>(numIntervals);
  interval = static_cast<size_t>(scaled);
  if (interval >= numIntervals) {
    interval = numIntervals - 1;
  }
  localT = scaled - static_cast<double>(interval);
  localT = std::clamp(localT, 0.0, 1.0);

  // Interpolate between keyPoints[interval] and keyPoints[interval+1].
  double pathProgress = kp[interval] + (kp[interval + 1] - kp[interval]) * localT;
  return std::clamp(pathProgress, 0.0, 1.0);
}

/// Generate the transform string for animateMotion at given progress.
std::string computeMotionTransform(Registry& registry, Entity animateMotionEntity,
                                   double progress, const AnimateMotionComponent& motionComp,
                                   const AnimationTimingComponent& timing) {
  auto maybePath = buildMotionPath(registry, animateMotionEntity, motionComp);
  if (!maybePath.has_value()) {
    return {};
  }

  // Apply keyPoints mapping if present.
  double pathProgress = applyKeyPoints(progress, motionComp, timing);

  auto motionResult = evaluateMotionOnPath(maybePath.value(), pathProgress);
  std::ostringstream oss;
  oss.precision(6);
  oss << "translate(" << motionResult.position.x << ", " << motionResult.position.y << ")";

  // Apply rotation if needed.
  if (motionComp.rotate == "auto") {
    oss << " rotate(" << motionResult.angle << ")";
  } else if (motionComp.rotate == "auto-reverse") {
    oss << " rotate(" << (motionResult.angle + 180.0) << ")";
  } else {
    // Fixed angle.
    double fixedAngle = 0.0;
    if (tryParseDouble(motionComp.rotate, fixedAngle) && fixedAngle != 0.0) {
      oss << " rotate(" << fixedAngle << ")";
    }
  }

  return oss.str();
}

}  // namespace

void AnimationSystem::advance(Registry& registry, double documentTime,
                              std::vector<ParseError>* /*outWarnings*/) {
  // Clear all animated overrides from previous frame.
  for (auto [entity, animValues] : registry.view<AnimatedValuesComponent>().each()) {
    animValues.overrides.clear();
  }

  // Pre-pass: compute timing states for all animation entities so that syncbase
  // references can be resolved. Process in two passes: first non-syncbase, then syncbase.
  {
    // Compute timing for <set> entities.
    for (auto [entity, setComp, timing] :
         registry.view<SetAnimationComponent, AnimationTimingComponent>().each()) {
      auto& state = registry.get_or_emplace<AnimationStateComponent>(entity);
      if (state.targetEntity == entt::null) {
        state.targetEntity = resolveTargetByHrefOrParent(registry, entity, setComp.href);
      }
      computeTimingState(state, timing, documentTime, /*isSetElement=*/true);
    }

    // Compute timing for <animate> entities.
    for (auto [entity, valueComp, timing] :
         registry.view<AnimateValueComponent, AnimationTimingComponent>().each()) {
      auto& state = registry.get_or_emplace<AnimationStateComponent>(entity);
      if (state.targetEntity == entt::null) {
        state.targetEntity = resolveTargetByHrefOrParent(registry, entity, valueComp.href);
      }
      computeTimingState(state, timing, documentTime, /*isSetElement=*/false);
    }

    // Compute timing for <animateTransform> entities.
    for (auto [entity, transformComp, timing] :
         registry.view<AnimateTransformComponent, AnimationTimingComponent>().each()) {
      auto& state = registry.get_or_emplace<AnimationStateComponent>(entity);
      if (state.targetEntity == entt::null) {
        state.targetEntity = resolveTargetByHrefOrParent(registry, entity, transformComp.href);
      }
      computeTimingState(state, timing, documentTime, /*isSetElement=*/false);
    }

    // Compute timing for <animateMotion> entities.
    for (auto [entity, motionComp, timing] :
         registry.view<AnimateMotionComponent, AnimationTimingComponent>().each()) {
      auto& state = registry.get_or_emplace<AnimationStateComponent>(entity);
      if (state.targetEntity == entt::null) {
        state.targetEntity = resolveTargetByHrefOrParent(registry, entity, motionComp.href);
      }
      computeTimingState(state, timing, documentTime, /*isSetElement=*/false);
    }

    // Now resolve syncbase references. Iterate to handle chains (a.end → b.begin → c.begin).
    // Max 8 iterations to avoid infinite loops from malformed input.
    for (int pass = 0; pass < 8; ++pass) {
      bool changed = false;
      for (auto [entity, timing, state] :
           registry.view<AnimationTimingComponent, AnimationStateComponent>().each()) {
        double oldBegin = state.beginTime;
        double oldActive = state.activeDuration;

        if (timing.beginSyncbase.has_value()) {
          resolveSyncbaseBeginTime(registry, timing, state);
        }
        if (timing.endSyncbase.has_value()) {
          resolveSyncbaseEndTime(registry, timing, state);
        }

        // Recompute phase after resolving syncbase times.
        if (timing.beginSyncbase.has_value() || timing.endSyncbase.has_value()) {
          state.phase = computePhase(documentTime, state.beginTime, state.activeDuration);
        }

        if (state.beginTime != oldBegin || state.activeDuration != oldActive) {
          changed = true;
        }
      }
      if (!changed) {
        break;
      }
    }
  }

  // Process all <set> animation entities in document order (ascending entity ID).
  {
    auto view = registry.view<SetAnimationComponent, AnimationTimingComponent>();
    std::vector<Entity> setEntities;
    for (auto entity : view) {
      setEntities.push_back(entity);
    }
    std::sort(setEntities.begin(), setEntities.end());

    for (auto entity : setEntities) {
      auto& setComp = registry.get<SetAnimationComponent>(entity);
      auto& timing = registry.get<AnimationTimingComponent>(entity);
      auto& state = registry.get<AnimationStateComponent>(entity);

      if (state.targetEntity == entt::null) {
        continue;
      }

      if (shouldApplyValue(state.phase, timing.fill) && !setComp.attributeName.empty() &&
          registry.valid(state.targetEntity)) {
        auto& animValues = registry.get_or_emplace<AnimatedValuesComponent>(state.targetEntity);
        animValues.overrides[setComp.attributeName] = setComp.to;
      }
    }
  }

  // Process all <animate> animation entities in document order (ascending entity ID).
  {
    auto view = registry.view<AnimateValueComponent, AnimationTimingComponent>();
    std::vector<Entity> animateEntities;
    for (auto entity : view) {
      animateEntities.push_back(entity);
    }
    std::sort(animateEntities.begin(), animateEntities.end());

    for (auto entity : animateEntities) {
      auto& valueComp = registry.get<AnimateValueComponent>(entity);
      auto& timing = registry.get<AnimationTimingComponent>(entity);
      auto& state = registry.get<AnimationStateComponent>(entity);

      if (state.targetEntity == entt::null) {
        continue;
      }

      if (!shouldApplyValue(state.phase, timing.fill) || valueComp.attributeName.empty() ||
          !registry.valid(state.targetEntity)) {
        continue;
      }

      // Compute interpolation progress.
      double progress = 0.0;
      if (state.phase == AnimationPhase::Active) {
        progress = computeProgress(documentTime, state.beginTime, state.simpleDuration,
                                   state.activeDuration, valueComp);
      } else {
        // Frozen: use final value (progress = 1.0).
        progress = 1.0;
      }

      std::string interpolated = interpolateAnimateValue(progress, valueComp);
      if (interpolated.empty()) {
        continue;
      }

      // Handle accumulate="sum": add (iteration * last-iteration value) to the result.
      if (valueComp.accumulate && state.phase == AnimationPhase::Active) {
        int iteration = computeIteration(documentTime, state.beginTime, state.simpleDuration,
                                         state.activeDuration);
        if (iteration > 0) {
          // Get the final value of one iteration (progress = 1.0).
          std::string finalVal = interpolateAnimateValue(1.0, valueComp);
          double finalNum = 0.0;
          double interpNum = 0.0;
          if (tryParseDouble(finalVal, finalNum) && tryParseDouble(interpolated, interpNum)) {
            interpolated = formatDouble(interpNum + finalNum * iteration);
          }
        }
      }

      auto& animValues = registry.get_or_emplace<AnimatedValuesComponent>(state.targetEntity);

      // Handle additive="sum": add to existing value for this attribute.
      if (valueComp.additive) {
        auto it = animValues.overrides.find(valueComp.attributeName);
        if (it != animValues.overrides.end()) {
          double existingNum = 0.0;
          double newNum = 0.0;
          if (tryParseDouble(it->second, existingNum) && tryParseDouble(interpolated, newNum)) {
            it->second = formatDouble(existingNum + newNum);
            continue;
          }
        }
      }

      animValues.overrides[valueComp.attributeName] = std::move(interpolated);
    }
  }

  // Process all <animateTransform> animation entities in document order.
  {
    auto view = registry.view<AnimateTransformComponent, AnimationTimingComponent>();
    std::vector<Entity> transformEntities;
    for (auto entity : view) {
      transformEntities.push_back(entity);
    }
    std::sort(transformEntities.begin(), transformEntities.end());

    for (auto entity : transformEntities) {
      auto& transformComp = registry.get<AnimateTransformComponent>(entity);
      auto& timing = registry.get<AnimationTimingComponent>(entity);
      auto& state = registry.get<AnimationStateComponent>(entity);

      if (state.targetEntity == entt::null) {
        continue;
      }

      if (!shouldApplyValue(state.phase, timing.fill) ||
          !registry.valid(state.targetEntity)) {
        continue;
      }

      double progress = 0.0;
      if (state.phase == AnimationPhase::Active) {
        // Use simple progress computation for transforms.
        double elapsed = documentTime - state.beginTime;
        if (std::isfinite(state.activeDuration)) {
          elapsed = std::min(elapsed, state.activeDuration);
        }
        double simpleTime = elapsed;
        if (std::isfinite(state.simpleDuration) && state.simpleDuration > 0.0) {
          simpleTime = std::fmod(elapsed, state.simpleDuration);
          if (simpleTime == 0.0 && elapsed > 0.0) {
            simpleTime = state.simpleDuration;
          }
        }
        if (std::isfinite(state.simpleDuration) && state.simpleDuration > 0.0) {
          progress = simpleTime / state.simpleDuration;
        }
        progress = std::clamp(progress, 0.0, 1.0);
      } else {
        progress = 1.0;
      }

      std::string interpolated = interpolateTransformValue(progress, transformComp);
      if (interpolated.empty()) {
        continue;
      }

      auto& animValues = registry.get_or_emplace<AnimatedValuesComponent>(state.targetEntity);

      // For additive="sum", append to existing transform.
      if (transformComp.additive) {
        auto it = animValues.overrides.find("transform");
        if (it != animValues.overrides.end()) {
          it->second += " " + interpolated;
          continue;
        }
      }

      animValues.overrides["transform"] = std::move(interpolated);
    }
  }

  // Process all <animateMotion> animation entities in document order.
  {
    auto view = registry.view<AnimateMotionComponent, AnimationTimingComponent>();
    std::vector<Entity> motionEntities;
    for (auto entity : view) {
      motionEntities.push_back(entity);
    }
    std::sort(motionEntities.begin(), motionEntities.end());

    for (auto entity : motionEntities) {
      auto& motionComp = registry.get<AnimateMotionComponent>(entity);
      auto& timing = registry.get<AnimationTimingComponent>(entity);
      auto& state = registry.get<AnimationStateComponent>(entity);

      if (state.targetEntity == entt::null) {
        continue;
      }

      if (!shouldApplyValue(state.phase, timing.fill) ||
          !registry.valid(state.targetEntity)) {
        continue;
      }

      double progress = 0.0;
      if (state.phase == AnimationPhase::Active) {
        double elapsed = documentTime - state.beginTime;
        if (std::isfinite(state.activeDuration)) {
          elapsed = std::min(elapsed, state.activeDuration);
        }
        double simpleTime = elapsed;
        if (std::isfinite(state.simpleDuration) && state.simpleDuration > 0.0) {
          simpleTime = std::fmod(elapsed, state.simpleDuration);
          if (simpleTime == 0.0 && elapsed > 0.0) {
            simpleTime = state.simpleDuration;
          }
        }
        if (std::isfinite(state.simpleDuration) && state.simpleDuration > 0.0) {
          progress = simpleTime / state.simpleDuration;
        }
        progress = std::clamp(progress, 0.0, 1.0);
      } else {
        progress = 1.0;
      }

      std::string motionTransform = computeMotionTransform(registry, entity, progress, motionComp, timing);
      if (motionTransform.empty()) {
        continue;
      }

      auto& animValues = registry.get_or_emplace<AnimatedValuesComponent>(state.targetEntity);

      // animateMotion creates a supplemental transform that is appended.
      auto it = animValues.overrides.find("transform");
      if (it != animValues.overrides.end()) {
        it->second += " " + motionTransform;
      } else {
        animValues.overrides["transform"] = std::move(motionTransform);
      }
    }
  }

  // Clean up empty AnimatedValuesComponent instances.
  for (auto [entity, animValues] : registry.view<AnimatedValuesComponent>().each()) {
    if (animValues.overrides.empty()) {
      registry.remove<AnimatedValuesComponent>(entity);
    }
  }
}

}  // namespace donner::svg::components
