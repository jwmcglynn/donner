#include "donner/svg/components/animation/AnimationSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/StringUtils.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/xml/components/XMLNamespaceContext.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationMotion.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationStateComponent.h"
#include "donner/svg/components/animation/AnimationTimeline.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/parser/PathParser.h"

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

bool AnimatesContextAttribute(Registry& registry, Entity entity, bool reference) {
  const auto matches = [reference](std::string_view name) {
    return reference ? name == "href" || name == "xlink:href"
                     : name == "gradientUnits" || name == "maskUnits";
  };
  if (const auto* value = registry.try_get<AnimateValueComponent>(entity)) {
    return matches(value->attributeName);
  }
  if (const auto* value = registry.try_get<SetAnimationComponent>(entity)) {
    return matches(value->attributeName);
  }
  return false;
}

/// Returns true if the animation value should be applied (active or frozen).
bool shouldApplyValue(AnimationPhase phase, AnimationFill fill) {
  return phase == AnimationPhase::Active ||
         (phase == AnimationPhase::After && fill == AnimationFill::Freeze);
}

/// Compute the interpolation progress [0,1] within the active interval.
/// Handles repeat iterations and keyTimes.
double computeProgress(double documentTime, double beginTime, double simpleDuration,
                       double activeDuration, bool frozen) {
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
    if (frozen && simpleTime == 0.0 && elapsed > 0.0) {
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
std::vector<double> parseNumbers(std::string_view str) {
  std::vector<double> result;
  while (!str.empty()) {
    if (str.front() == ' ' || str.front() == '\t' || str.front() == ',') {
      str.remove_prefix(1);
      continue;
    }

    const auto maybeNumber = donner::parser::NumberParser::Parse(str);
    if (maybeNumber.hasError()) {
      return {};
    }
    if (result.size() >= AnimationResourceBudget::kMaximumNumbersPerValue) {
      return {};
    }
    result.push_back(maybeNumber.result().number);
    str.remove_prefix(maybeNumber.result().consumedChars);
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
      case Path::Verb::ClosePath: oss << "Z"; break;
    }
  }

  return oss.str();
}

/// Try to interpolate SVG path data strings.
/// Returns empty string if paths are not structurally compatible.
std::string tryInterpolatePaths(const std::string& fromStr, const std::string& toStr, double t) {
  if (fromStr.size() > AnimationResourceBudget::kMaximumPathValueBytes ||
      toStr.size() > AnimationResourceBudget::kMaximumPathValueBytes) {
    return {};
  }
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
std::string interpolateAnimateValue(double progress, const AnimateValueComponent& valueComp,
                                    const std::string& underlying,
                                    const AnimationValueContext& context) {
  std::vector<std::string> values = valueComp.values;
  if (values.empty()) {
    if (valueComp.to) {
      values = {valueComp.from.value_or(underlying), *valueComp.to};
    } else if (valueComp.by && !context.referenceValue) {
      const auto from = valueComp.from
                            ? valueComp.from
                            : CombineAnimationValues(*valueComp.by, *valueComp.by, 0, 0, context);
      if (!from) {
        return {};
      }
      const auto to = CombineAnimationValues(*from, *valueComp.by, 1, 1, context);
      if (!to) {
        return {};
      }
      values = {*from, *to};
    }
  }
  std::vector<double> distances;
  AnimationInterpolation options = valueComp;
  if (context.referenceValue) {
    options.calcMode = CalcMode::Discrete;
  }
  if (options.calcMode == CalcMode::Paced) {
    for (std::size_t i = 1; i < values.size(); ++i) {
      const auto distance = AnimationValueDistance(values[i - 1], values[i], context);
      if (!distance) {
        options.calcMode = CalcMode::Discrete;
        options.keyTimes.clear();
        break;
      }
      distances.push_back(*distance);
    }
  }
  const auto segment = SelectAnimationSegment(progress, values.size(), options, distances);
  if (!segment) {
    return {};
  }
  if (segment->from == segment->to) {
    return values[segment->from];
  }
  const auto& from = values[segment->from];
  const auto& to = values[segment->to];
  if (auto result =
          CombineAnimationValues(from, to, 1 - segment->progress, segment->progress, context)) {
    return *result;
  }
  const auto path = tryInterpolatePaths(from, to, segment->progress);
  if (!path.empty()) {
    return path;
  }
  options.calcMode = CalcMode::Discrete;
  const auto discrete = SelectAnimationSegment(progress, values.size(), options);
  return discrete ? values[discrete->from] : std::string{};
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
    case TransformAnimationType::SkewX: oss << "skewX(" << values[0] << ")"; break;
    case TransformAnimationType::SkewY: oss << "skewY(" << values[0] << ")"; break;
  }
  return oss.str();
}

/// The additive zero tuple, including scale(0) for by-only scale animation.
std::vector<double> zeroTransformValues(TransformAnimationType type) {
  switch (type) {
    case TransformAnimationType::Translate: return {0.0, 0.0};
    case TransformAnimationType::Scale: return {0.0, 0.0};
    case TransformAnimationType::Rotate: return {0.0, 0.0, 0.0};
    case TransformAnimationType::SkewX:
    case TransformAnimationType::SkewY: return {0.0};
  }
  return {0.0};
}

/// Interpolate transform values for <animateTransform>.
std::string interpolateTransformValue(double progress,
                                      const AnimateTransformComponent& transformComp,
                                      double iteration = 0) {
  const TransformAnimationType type = transformComp.type;

  // Build the effective keyframe list as parsed and padded number tuples. Precedence per SMIL:
  // values, from/to, from/by, by, then to.
  std::vector<std::vector<double>> keyframes;

  const auto parsePadded = [type](const std::string& str) {
    std::vector<double> nums = parseNumbers(str);
    if (nums.empty() || nums.size() > static_cast<std::size_t>(expectedNumberCount(type)) ||
        !std::all_of(nums.begin(), nums.end(), [](double value) { return std::isfinite(value); })) {
      return std::vector<double>{};
    }
    padToExpected(nums, type);
    return nums;
  };

  if (!transformComp.values.empty()) {
    keyframes.reserve(transformComp.values.size());
    for (const auto& v : transformComp.values) {
      keyframes.push_back(parsePadded(v));
    }
  } else if (transformComp.from.has_value() && transformComp.to.has_value()) {
    keyframes.push_back(parsePadded(*transformComp.from));
    keyframes.push_back(parsePadded(*transformComp.to));
  } else if (transformComp.by.has_value()) {
    // A by-only transform uses additive zero, including scale(0).
    std::vector<double> fromNums = transformComp.from.has_value() ? parsePadded(*transformComp.from)
                                                                  : zeroTransformValues(type);
    const std::vector<double> byNums = parsePadded(*transformComp.by);
    if (fromNums.empty() || byNums.empty()) {
      return {};
    }
    std::vector<double> toNums = fromNums;
    for (size_t i = 0; i < toNums.size() && i < byNums.size(); ++i) {
      toNums[i] += byNums[i];
    }
    keyframes.push_back(std::move(fromNums));
    keyframes.push_back(std::move(toNums));
  } else if (transformComp.to.has_value()) {
    keyframes.push_back(parsePadded(*transformComp.to));
  } else {
    return {};
  }

  if (std::any_of(keyframes.begin(), keyframes.end(),
                  [](const auto& frame) { return frame.empty(); })) {
    return {};
  }
  std::vector<double> distances;
  if (transformComp.calcMode == CalcMode::Paced) {
    for (std::size_t i = 1; i < keyframes.size(); ++i) {
      double distance = 0;
      const std::size_t count = type == TransformAnimationType::Rotate ? 1 : keyframes[i].size();
      for (std::size_t j = 0; j < count; ++j) {
        distance = std::hypot(distance, keyframes[i][j] - keyframes[i - 1][j]);
      }
      distances.push_back(distance);
    }
  }
  const auto segment = SelectAnimationSegment(progress, keyframes.size(), transformComp, distances);
  if (!segment) {
    return {};
  }
  if (segment->from == segment->to) {
    auto result = keyframes[segment->from];
    if (transformComp.accumulate) {
      for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] += iteration * keyframes.back()[i];
      }
    }
    return formatTransform(type, result);
  }
  const std::size_t interval = segment->from;
  const double localT = segment->progress;

  // Interpolate the number tuples.
  const std::vector<double>& fromNums = keyframes[interval];
  const std::vector<double>& toNums = keyframes[interval + 1];

  const size_t count = std::min(fromNums.size(), toNums.size());
  std::vector<double> result(count);
  for (size_t i = 0; i < count; ++i) {
    result[i] = fromNums[i] + (toNums[i] - fromNums[i]) * localT;
    if (transformComp.accumulate) {
      result[i] += iteration * keyframes.back()[i];
    }
  }

  return formatTransform(type, result);
}

void AddAnimationSourceBytes(std::string_view value, std::size_t& sourceBytes) {
  if (sourceBytes <= AnimationResourceBudget::kMaximumSourceBytes) {
    sourceBytes += std::min(value.size(), AnimationResourceBudget::kMaximumSourceBytes + 1);
  }
}

void AddOptionalAnimationSourceBytes(const std::optional<std::string>& value,
                                     std::size_t& sourceBytes) {
  if (value.has_value()) {
    AddAnimationSourceBytes(*value, sourceBytes);
  }
}

std::size_t AnimationSourceBytes(const SetAnimationComponent* setComp,
                                 const AnimateValueComponent* valueComp,
                                 const AnimateTransformComponent* transformComp,
                                 const AnimateMotionComponent* motionComp) {
  std::size_t sourceBytes = 0;
  const auto addValues = [&](const auto& component) {
    sourceBytes += (component.keyTimes.size() + component.keySplines.size()) * sizeof(double);
    AddOptionalAnimationSourceBytes(component.from, sourceBytes);
    AddOptionalAnimationSourceBytes(component.to, sourceBytes);
    AddOptionalAnimationSourceBytes(component.by, sourceBytes);
    for (const std::string& value : component.values) {
      AddAnimationSourceBytes(value, sourceBytes);
      if (sourceBytes > AnimationResourceBudget::kMaximumSourceBytes) {
        break;
      }
    }
  };
  if (setComp) {
    AddAnimationSourceBytes(setComp->attributeName, sourceBytes);
    AddAnimationSourceBytes(setComp->to, sourceBytes);
    AddOptionalAnimationSourceBytes(setComp->href, sourceBytes);
  } else if (valueComp) {
    addValues(*valueComp);
  } else if (transformComp) {
    addValues(*transformComp);
    AddAnimationSourceBytes(transformComp->attributeName, sourceBytes);
  } else if (motionComp) {
    addValues(*motionComp);
    AddOptionalAnimationSourceBytes(motionComp->path, sourceBytes);
    AddAnimationSourceBytes(motionComp->rotate, sourceBytes);
    sourceBytes += motionComp->keyPoints.size() * sizeof(double);
  }
  return sourceBytes;
}

std::string RoundAnimationIntegers(const std::string& input) {
  const auto numbers = ParseAnimationLengths(input);
  if (!numbers) {
    return {};
  }
  std::ostringstream output;
  output.precision(12);
  for (std::size_t i = 0; i < numbers->size(); ++i) {
    if ((*numbers)[i].unit != Lengthd::Unit::None) {
      return {};
    }
    if (i != 0) {
      output << ' ';
    }
    output << std::floor((*numbers)[i].value + 0.5);
  }
  return output.str();
}

std::optional<std::pair<std::string_view, std::string>> SampleAnimationValue(
    double progress, double iteration, const SetAnimationComponent* setComp,
    const AnimateValueComponent* valueComp, const AnimateTransformComponent* transformComp,
    const std::string& underlying, const AnimationValueContext& context) {
  if (setComp) {
    if (setComp->attributeName.empty()) {
      return std::nullopt;
    }
    return std::pair<std::string_view, std::string>{setComp->attributeName, setComp->to};
  }
  if (valueComp) {
    if (valueComp->attributeName.empty()) {
      return std::nullopt;
    }
    std::string sampled = interpolateAnimateValue(progress, *valueComp, underlying, context);
    const bool toOnly = valueComp->values.empty() && !valueComp->from && valueComp->to;
    if (!context.referenceValue && valueComp->accumulate && !toOnly && iteration > 0) {
      const auto last = interpolateAnimateValue(1, *valueComp, underlying, context);
      sampled = CombineAnimationValues(sampled, last, 1, iteration, context).value_or(sampled);
    }
    const bool byOnly =
        valueComp->values.empty() && !valueComp->from && !valueComp->to && valueComp->by;
    if (!context.referenceValue && !sampled.empty() && !toOnly && (valueComp->additive || byOnly)) {
      sampled = CombineAnimationValues(underlying, sampled, 1, 1, context).value_or(sampled);
    }
    if (context.integerValue) {
      sampled = RoundAnimationIntegers(sampled);
    }
    return std::pair<std::string_view, std::string>{valueComp->attributeName, std::move(sampled)};
  }
  std::string sampled = interpolateTransformValue(progress, *transformComp, iteration);
  const bool byOnly = transformComp->values.empty() && !transformComp->from && !transformComp->to &&
                      transformComp->by;
  if (!sampled.empty() && !underlying.empty() && (transformComp->additive || byOnly)) {
    sampled = underlying + " " + sampled;
  }
  return std::pair<std::string_view, std::string>{transformComp->attributeName, std::move(sampled)};
}

/// The `href` of an animation element, taken from whichever animation component carries it.
const std::optional<std::string>& AnimationHref(Registry& registry, Entity entity) {
  if (const auto* setComp = registry.try_get<SetAnimationComponent>(entity)) {
    return setComp->href;
  }
  if (const auto* valueComp = registry.try_get<AnimateValueComponent>(entity)) {
    return valueComp->href;
  }
  if (const auto* motion = registry.try_get<AnimateMotionComponent>(entity)) {
    return motion->href;
  }
  return registry.get<AnimateTransformComponent>(entity).href;
}

bool ValidTransformAttribute(Registry& registry, Entity target, std::string_view name) {
  const auto* type = registry.try_get<ElementTypeComponent>(target);
  if (!type) {
    return false;
  }
  if (name == "transform") {
    return true;
  }
  if (name == "gradientTransform") {
    return type->type() == ElementType::LinearGradient ||
           type->type() == ElementType::RadialGradient;
  }
  return name == "patternTransform" && type->type() == ElementType::Pattern;
}

bool ProcessAnimation(Registry& registry, Entity entity, double documentTime,
                      AnimationResourceBudget& resourceBudget,
                      std::unordered_map<Entity, std::string>& motionBases) {
  const auto& timing = registry.get<AnimationTimingComponent>(entity);
  auto& state = registry.get_or_emplace<AnimationStateComponent>(entity);
  const auto* setComp = registry.try_get<SetAnimationComponent>(entity);
  const auto* valueComp = registry.try_get<AnimateValueComponent>(entity);
  const auto* transformComp = registry.try_get<AnimateTransformComponent>(entity);
  const auto* motionComp = registry.try_get<AnimateMotionComponent>(entity);

  if (!resourceBudget.reserveAnimation(
          AnimationSourceBytes(setComp, valueComp, transformComp, motionComp))) {
    return false;
  }
  state.targetEntity =
      resolveTargetByHrefOrParent(registry, entity, AnimationHref(registry, entity));
  if (!registry.valid(state.targetEntity) || !shouldApplyValue(state.phase, timing.fill)) {
    return true;
  }
  if (!IsAnimationInSvgNamespace(registry, state.targetEntity)) {
    return true;
  }

  if (transformComp &&
      !ValidTransformAttribute(registry, state.targetEntity, transformComp->attributeName)) {
    return true;
  }

  if (timing.fill == AnimationFill::Remove &&
      documentTime >= state.beginTime + state.repeatDuration) {
    return true;
  }
  const double sampleTime =
      (state.phase == AnimationPhase::Active || !std::isfinite(state.activeDuration))
          ? documentTime
          : state.beginTime + state.activeDuration;
  const double progress = computeProgress(
      std::min(sampleTime, state.beginTime + state.repeatDuration), state.beginTime,
      state.simpleDuration, state.activeDuration,
      state.phase == AnimationPhase::After || sampleTime >= state.beginTime + state.repeatDuration);
  double iteration = 0;
  if (std::isfinite(state.simpleDuration) && state.simpleDuration > 0) {
    const double elapsed = std::min(sampleTime - state.beginTime, state.repeatDuration);
    iteration = std::floor(elapsed / state.simpleDuration);
    if (progress == 1 && iteration > 0) {
      --iteration;
    }
  }
  const std::string_view attribute =
      setComp
          ? setComp->attributeName
          : (valueComp
                 ? std::string_view(valueComp->attributeName)
                 : (transformComp ? std::string_view(transformComp->attributeName) : "transform"));
  const auto context =
      setComp ? AnimationValueContext{}
              : AnimationContextFor(registry, state.targetEntity, attribute, resourceBudget);
  const bool needsUnderlying =
      motionComp || (valueComp && (valueComp->additive || !valueComp->from)) ||
      (transformComp && (transformComp->additive || (!transformComp->from && transformComp->by)));
  const std::string underlying = needsUnderlying
                                     ? UnderlyingAnimationValue(registry, state.targetEntity,
                                                                attribute, context, resourceBudget)
                                     : std::string{};
  if (resourceBudget.rejected()) {
    return false;
  }
  std::optional<std::pair<std::string_view, std::string>> sampled;
  if (motionComp) {
    std::string motion =
        SampleAnimationMotion(registry, entity, *motionComp, progress, iteration, resourceBudget);
    const auto [base, inserted] = motionBases.try_emplace(state.targetEntity, underlying);
    const std::string& prefix = motionComp->additive ? underlying : base->second;
    if (!motion.empty() && !prefix.empty()) {
      motion = prefix + " " + motion;
    }
    sampled = std::pair<std::string_view, std::string>{"transform", std::move(motion)};
  } else {
    sampled = SampleAnimationValue(progress, iteration, setComp, valueComp, transformComp,
                                   underlying, context);
  }
  if (resourceBudget.rejected()) {
    return false;
  }
  if (!sampled || sampled->second.empty()) {
    return true;
  }
  if (!resourceBudget.reserveOutput(sampled->second.size())) {
    return false;
  }
  auto& values = registry.get_or_emplace<AnimatedValuesComponent>(state.targetEntity);
  values.overrides[std::string(sampled->first)] = std::move(sampled->second);
  return true;
}

std::string_view MouseEventName(EventType type) {
  switch (type) {
    case EventType::Click: return "click";
    case EventType::DblClick: return "dblclick";
    case EventType::MouseDown: return "mousedown";
    case EventType::MouseUp: return "mouseup";
    case EventType::MouseMove: return "mousemove";
    case EventType::MouseEnter: return "mouseenter";
    case EventType::MouseLeave: return "mouseleave";
    case EventType::MouseOver: return "mouseover";
    case EventType::MouseOut: return "mouseout";
    default: return {};
  }
}

std::unordered_set<Entity> MouseEventPath(Registry& registry, Entity target, bool bubbles) {
  std::unordered_set<Entity> path;
  constexpr std::size_t kMaximumEventDepth = 4096;
  while (registry.valid(target) && path.size() < kMaximumEventDepth && path.insert(target).second) {
    const auto* tree = registry.try_get<donner::components::TreeComponent>(target);
    if (!bubbles || tree == nullptr) {
      break;
    }
    target = tree->parent();
  }
  return path;
}

bool RecordMatchingEvents(Registry& registry, const std::optional<std::string>& value,
                          Entity defaultTarget, std::string_view event,
                          const std::unordered_set<Entity>& path, double time,
                          std::vector<double>& instances, bool futureOnly = false) {
  if (!value) {
    return false;
  }
  const auto specs = ParseAnimationTimeList(*value);
  if (!specs) {
    return false;
  }
  bool changed = false;
  for (const auto& spec : *specs) {
    if (spec.kind != AnimationTimeSpecifier::Kind::Event || spec.event != event) {
      continue;
    }
    const Entity source =
        spec.sourceId.empty()
            ? defaultTarget
            : registry.ctx().get<SVGDocumentContext>().getEntityById(RcString(spec.sourceId));
    if (path.contains(source) && (!futureOnly || spec.offset > 0)) {
      changed |= InsertAnimationInstance(instances, time + spec.offset);
    }
  }
  return changed;
}

bool DispatchAnimationMouseEvent(Registry& registry, Entity entity,
                                 const AnimationTimingComponent& timing, std::string_view eventName,
                                 const std::unordered_set<Entity>& path, double time) {
  auto& state = registry.get_or_emplace<AnimationStateComponent>(entity);
  const Entity target =
      resolveTargetByHrefOrParent(registry, entity, AnimationHref(registry, entity));
  bool changed = false;
  changed |= RecordMatchingEvents(registry, timing.endValue, target, eventName, path, time,
                                  state.eventEndTimes, state.phase != AnimationPhase::Active);
  changed |= RecordMatchingEvents(registry, timing.beginValue, target, eventName, path, time,
                                  state.eventBeginTimes);
  return changed;
}

}  // namespace

bool IsAnimationInSvgNamespace(Registry& registry, Entity entity) {
  const auto& namespaces = registry.ctx().get<xml::components::XMLNamespaceContext>();
  for (std::size_t depth = 0; depth < 4096; ++depth) {
    const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
    if (!tree) {
      return false;
    }
    const auto uri = namespaces.getNamespaceUri(registry, entity, tree->tagName().namespacePrefix);
    if ((uri && *uri != "http://www.w3.org/2000/svg") ||
        (!uri && !tree->tagName().namespacePrefix.empty())) {
      return false;
    }
    if (tree->parent() == entt::null) {
      return true;
    }
    entity = tree->parent();
  }
  return false;
}

std::vector<Entity> CollectAnimationEntities(Registry& registry) {
  std::vector<Entity> result;
  const Entity root = registry.ctx().get<SVGDocumentContext>().rootEntity;
  Entity current = root;
  std::size_t visited = 0;
  std::size_t declarations = 0;
  while (current != entt::null && ++visited <= 1024 * 1024) {
    if (registry.any_of<SetAnimationComponent, AnimateValueComponent, AnimateTransformComponent,
                        AnimateMotionComponent>(current)) {
      if (++declarations > AnimationResourceBudget::kMaximumAnimations) {
        result.resize(AnimationResourceBudget::kMaximumAnimations + 1, entt::null);
        break;
      }
      if (!IsAnimationInSvgNamespace(registry, current)) {
        registry.remove<AnimationStateComponent>(current);
      } else {
        result.push_back(current);
      }
      if (result.size() > AnimationResourceBudget::kMaximumAnimations) {
        break;
      }
    }
    const auto* tree = registry.try_get<donner::components::TreeComponent>(current);
    if (!tree) {
      break;
    }
    if (tree->firstChild() != entt::null) {
      current = tree->firstChild();
      continue;
    }
    while (tree && tree->nextSibling() == entt::null) {
      if (current == root) {
        return result;
      }
      current = tree->parent();
      tree = registry.try_get<donner::components::TreeComponent>(current);
    }
    current = tree ? tree->nextSibling() : entt::null;
  }
  if (current != entt::null && visited > 1024 * 1024) {
    result.resize(AnimationResourceBudget::kMaximumAnimations + 1, entt::null);
  }
  return result;
}

bool AnimationSystem::isAnimationTarget(Registry& registry, Entity targetEntity) const {
  if (targetEntity == entt::null) {
    return false;
  }

  const auto animations = CollectAnimationEntities(registry);
  if (animations.size() > AnimationResourceBudget::kMaximumAnimations) {
    return false;
  }
  for (const Entity entity : animations) {
    const auto* state = registry.try_get<AnimationStateComponent>(entity);
    const Entity resolved =
        (state != nullptr && state->targetEntity != entt::null)
            ? state->targetEntity
            : resolveTargetByHrefOrParent(registry, entity, AnimationHref(registry, entity));
    if (resolved == targetEntity) {
      return true;
    }
  }

  return false;
}

bool AnimationSystem::dispatchMouseEvent(Registry& registry, Entity targetEntity, EventType type,
                                         double documentTime) {
  return dispatchEvent(registry, targetEntity, MouseEventName(type), eventBubbles(type),
                       documentTime);
}

bool AnimationSystem::dispatchEvent(Registry& registry, Entity targetEntity, std::string_view name,
                                    bool bubbles, double documentTime) {
  if (name.empty() || name.size() > 128 || !registry.valid(targetEntity) ||
      !std::isfinite(documentTime)) {
    return false;
  }
  const auto animations = CollectAnimationEntities(registry);
  if (!ResolveAnimationTimeline(registry, animations, documentTime)) {
    return false;
  }
  const auto path = MouseEventPath(registry, targetEntity, bubbles);
  AnimationResourceBudget budget;
  bool changed = false;
  for (Entity entity : animations) {
    const auto& timing = registry.get<AnimationTimingComponent>(entity);
    std::size_t sourceBytes = 0;
    AddOptionalAnimationSourceBytes(timing.beginValue, sourceBytes);
    AddOptionalAnimationSourceBytes(timing.endValue, sourceBytes);
    if (!budget.reserveAnimation(sourceBytes)) {
      break;
    }
    if (registry.any_of<SetAnimationComponent, AnimateValueComponent, AnimateTransformComponent,
                        AnimateMotionComponent>(entity)) {
      changed |= DispatchAnimationMouseEvent(registry, entity, timing, name, path, documentTime);
    }
  }
  return changed;
}

void AnimationSystem::advance(Registry& registry, double documentTime,
                              std::vector<ParseDiagnostic>* /*outWarnings*/) {
  if (registry.ctx().contains<AnimationResourceBudget>()) {
    registry.ctx().erase<AnimationResourceBudget>();
  }
  auto& resourceBudget = registry.ctx().emplace<AnimationResourceBudget>();
  // Clear all animated overrides from previous frame.
  for (auto [entity, animValues] : registry.view<AnimatedValuesComponent>().each()) {
    animValues.overrides.clear();
  }

  if (registry.ctx().get<ResourceManagerContext>().processingMode() ==
      ProcessingMode::SecureStatic) {
    registry.clear<AnimatedValuesComponent>();
    return;
  }
  auto animations = CollectAnimationEntities(registry);
  if (!ResolveAnimationTimeline(registry, animations, documentTime)) {
    return;
  }
  std::stable_sort(animations.begin(), animations.end(), [&](Entity first, Entity second) {
    return registry.get<AnimationStateComponent>(first).beginTime <
           registry.get<AnimationStateComponent>(second).beginTime;
  });
  std::stable_partition(animations.begin(), animations.end(), [&](Entity entity) {
    return !registry.all_of<AnimateMotionComponent>(entity);
  });
  // Coordinate interpolation needs the final sampled unit selection, including inherited units.
  std::stable_partition(animations.begin(), animations.end(), [&](Entity entity) {
    return AnimatesContextAttribute(registry, entity, false);
  });
  std::stable_partition(animations.begin(), animations.end(), [&](Entity entity) {
    return AnimatesContextAttribute(registry, entity, true);
  });
  std::unordered_map<Entity, std::string> motionBases;
  for (const Entity entity : animations) {
    if (!ProcessAnimation(registry, entity, documentTime, resourceBudget, motionBases)) {
      for (auto [target, values] : registry.view<AnimatedValuesComponent>().each()) {
        values.overrides.clear();
      }
      break;
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
