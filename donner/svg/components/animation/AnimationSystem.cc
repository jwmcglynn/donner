#include "donner/svg/components/animation/AnimationSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationStateComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"
#include "donner/svg/parser/PathParser.h"

// Slice 1 SMIL animation system. Deferred features (paced/spline calcMode, syncbase timing,
// additive/accumulate, <animateMotion>/<mpath>) are intentionally absent; see
// docs/design_docs/animation.md.

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
  // A `begin` attribute that yielded no resolvable offset (e.g. begin="indefinite", or a list
  // containing only unsupported syncbase/event values) means the begin time is unresolved: the
  // animation never starts. This is distinct from an absent `begin` attribute, which defaults to
  // an offset of 0.
  if (timing.beginValue.has_value() && !timing.beginOffset.has_value()) {
    state.phase = AnimationPhase::Before;
    state.wasActive = false;
    return;
  }

  state.beginTime = timing.beginOffset.has_value() ? timing.beginOffset->seconds() : 0.0;
  state.simpleDuration = computeSimpleDuration(timing, isSetElement);
  state.activeDuration = computeActiveDuration(timing, state.simpleDuration);
  AnimationPhase newPhase = computePhase(documentTime, state.beginTime, state.activeDuration);

  // Enforce restart attribute.
  // restart="never": once the animation has completed, it cannot restart.
  if (timing.restart == AnimationRestart::Never && state.hasCompleted) {
    // Keep the After phase; animation stays frozen or removed.
    newPhase = AnimationPhase::After;
  }
  // restart="whenNotActive": can only restart when not currently active.
  if (timing.restart == AnimationRestart::WhenNotActive && state.wasActive &&
      newPhase == AnimationPhase::Active && state.phase == AnimationPhase::After) {
    // Was active, ended, now a new begin wants to make it active again; suppress.
    newPhase = AnimationPhase::After;
  }

  // Track completion for restart="never".
  if (newPhase == AnimationPhase::After && state.phase == AnimationPhase::Active) {
    state.hasCompleted = true;
  }

  state.wasActive = (newPhase == AnimationPhase::Active);
  state.phase = newPhase;
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

/// Format a double as string, avoiding unnecessary trailing zeros.
std::string formatDouble(double value) {
  std::ostringstream oss;
  oss.precision(6);
  oss << value;
  return oss.str();
}

/// Compute the interpolation progress [0,1] within the active interval.
/// Handles repeat iterations and keyTimes.
double computeProgress(double documentTime, double beginTime, double simpleDuration,
                       double activeDuration) {
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

/// Parse a space/comma-separated list of doubles from a string.
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

/// Check if two Paths have structurally compatible command sequences for interpolation.
bool arePathsCompatible(const Path& a, const Path& b) {
  const auto aCmds = a.commands();
  const auto bCmds = b.commands();
  if (aCmds.size() != bCmds.size()) {
    return false;
  }
  for (size_t i = 0; i < aCmds.size(); ++i) {
    if (aCmds[i].verb != bCmds[i].verb) {
      return false;
    }
  }
  return a.points().size() == b.points().size();
}

/// Interpolate two structurally compatible Paths at parameter t.
/// Returns the interpolated path as an SVG path data string.
std::string interpolatePaths(const Path& from, const Path& to, double t) {
  std::ostringstream oss;
  oss.precision(6);

  const auto cmds = from.commands();
  const auto fromPts = from.points();
  const auto toPts = to.points();

  auto lerpPoint = [&](size_t pi) {
    double x = fromPts[pi].x + (toPts[pi].x - fromPts[pi].x) * t;
    double y = fromPts[pi].y + (toPts[pi].y - fromPts[pi].y) * t;
    return std::pair<double, double>(x, y);
  };

  for (size_t i = 0; i < cmds.size(); ++i) {
    if (i > 0) {
      oss << ' ';
    }

    const size_t pi = cmds[i].pointIndex;
    switch (cmds[i].verb) {
      case Path::Verb::MoveTo: {
        auto [x, y] = lerpPoint(pi);
        oss << "M" << x << "," << y;
        break;
      }
      case Path::Verb::LineTo: {
        auto [x, y] = lerpPoint(pi);
        oss << "L" << x << "," << y;
        break;
      }
      case Path::Verb::QuadTo: {
        auto [cx, cy] = lerpPoint(pi);
        auto [ex, ey] = lerpPoint(pi + 1);
        oss << "Q" << cx << "," << cy << " " << ex << "," << ey;
        break;
      }
      case Path::Verb::CurveTo: {
        auto [c1x, c1y] = lerpPoint(pi);
        auto [c2x, c2y] = lerpPoint(pi + 1);
        auto [ex, ey] = lerpPoint(pi + 2);
        oss << "C" << c1x << "," << c1y << " " << c2x << "," << c2y << " " << ex << "," << ey;
        break;
      }
      case Path::Verb::ClosePath:
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
    // to-only animation: use "to" as a discrete value.
    return valueComp.to.value();
  } else if (valueComp.from.has_value() && valueComp.by.has_value()) {
    // from/by: numeric addition.
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

  // Linear mode: interpolate between adjacent values.
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

  // Try number-list interpolation (for dasharray, points, etc.).
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

}  // namespace

void AnimationSystem::advance(Registry& registry, double documentTime,
                              std::vector<ParseDiagnostic>* /*outWarnings*/) {
  // Clear all animated overrides from previous frame.
  for (auto [entity, animValues] : registry.view<AnimatedValuesComponent>().each()) {
    animValues.overrides.clear();
  }

  // Collect all animation entities and process them in document order (ascending entity ID).
  // Replace-mode sandwich priority: the last animation in document order wins, regardless of
  // element type, so all animation element types share a single ordered pass.
  std::vector<Entity> animationEntities;
  for (auto [entity, timing] : registry.view<AnimationTimingComponent>().each()) {
    if (registry.any_of<SetAnimationComponent, AnimateValueComponent, AnimateTransformComponent>(
            entity)) {
      animationEntities.push_back(entity);
    }
  }
  std::sort(animationEntities.begin(), animationEntities.end());

  for (const Entity entity : animationEntities) {
    const auto& timing = registry.get<AnimationTimingComponent>(entity);
    auto& state = registry.get_or_emplace<AnimationStateComponent>(entity);

    const auto* setComp = registry.try_get<SetAnimationComponent>(entity);
    const auto* valueComp = registry.try_get<AnimateValueComponent>(entity);
    const auto* transformComp = registry.try_get<AnimateTransformComponent>(entity);

    if (state.targetEntity == entt::null) {
      const std::optional<std::string>& href =
          setComp ? setComp->href : (valueComp ? valueComp->href : transformComp->href);
      state.targetEntity = resolveTargetByHrefOrParent(registry, entity, href);
    }

    computeTimingState(state, timing, documentTime, /*isSetElement=*/setComp != nullptr);

    if (!registry.valid(state.targetEntity) || !shouldApplyValue(state.phase, timing.fill)) {
      continue;
    }

    // Compute the interpolation progress: current time while active, or the final value
    // (progress = 1.0) when frozen.
    const double progress =
        (state.phase == AnimationPhase::Active)
            ? computeProgress(documentTime, state.beginTime, state.simpleDuration,
                              state.activeDuration)
            : 1.0;

    std::string attributeName;
    std::string newValue;
    if (setComp) {
      if (setComp->attributeName.empty()) {
        continue;
      }
      attributeName = setComp->attributeName;
      newValue = setComp->to;
    } else if (valueComp) {
      if (valueComp->attributeName.empty()) {
        continue;
      }
      attributeName = valueComp->attributeName;
      newValue = interpolateAnimateValue(progress, *valueComp);
    } else {
      attributeName = "transform";
      newValue = interpolateTransformValue(progress, *transformComp);
    }

    if (newValue.empty()) {
      continue;
    }

    auto& animValues = registry.get_or_emplace<AnimatedValuesComponent>(state.targetEntity);
    animValues.overrides[attributeName] = std::move(newValue);
  }

  // Clean up empty AnimatedValuesComponent instances.
  for (auto [entity, animValues] : registry.view<AnimatedValuesComponent>().each()) {
    if (animValues.overrides.empty()) {
      registry.remove<AnimatedValuesComponent>(entity);
    }
  }
}

}  // namespace donner::svg::components
