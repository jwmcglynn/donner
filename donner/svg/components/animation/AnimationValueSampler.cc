#include "donner/svg/components/animation/AnimationValueSampler.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

#include "donner/base/Path.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/parser/PathParser.h"

namespace donner::svg::components {
namespace {

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
/// Returns a value (which may be empty), or nullopt if interpolation failed.
std::optional<std::string> interpolateAnimateValue(double progress,
                                                   const AnimateValueComponent& valueComp,
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
  return discrete ? std::optional(values[discrete->from]) : std::nullopt;
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

}  // namespace

std::optional<std::pair<std::string, std::string>> SampleAnimationValue(
    double progress, double iteration, const SetAnimationComponent* setComp,
    const AnimateValueComponent* valueComp, const AnimateTransformComponent* transformComp,
    const std::string& underlying, const AnimationValueContext& context) {
  if (setComp) {
    if (setComp->attributeName.empty() || !setComp->to) {
      return std::nullopt;
    }
    return std::pair<std::string, std::string>{setComp->attributeName, *setComp->to};
  }
  if (valueComp) {
    if (valueComp->attributeName.empty()) {
      return std::nullopt;
    }
    auto value = interpolateAnimateValue(progress, *valueComp, underlying, context);
    if (!value) {
      return std::nullopt;
    }
    std::string sampled = std::move(*value);
    const bool toOnly = valueComp->values.empty() && !valueComp->from && valueComp->to;
    if (!context.referenceValue && valueComp->accumulate && !toOnly && iteration > 0) {
      const auto last = interpolateAnimateValue(1, *valueComp, underlying, context);
      if (last) {
        sampled = CombineAnimationValues(sampled, *last, 1, iteration, context).value_or(sampled);
      }
    }
    const bool byOnly =
        valueComp->values.empty() && !valueComp->from && !valueComp->to && valueComp->by;
    if (!context.referenceValue && !sampled.empty() && !toOnly && (valueComp->additive || byOnly)) {
      sampled = CombineAnimationValues(underlying, sampled, 1, 1, context).value_or(sampled);
    }
    if (context.integerValue) {
      sampled = RoundAnimationIntegers(sampled);
    }
    return std::pair<std::string, std::string>{valueComp->attributeName, std::move(sampled)};
  }
  std::string sampled = interpolateTransformValue(progress, *transformComp, iteration);
  const bool byOnly = transformComp->values.empty() && !transformComp->from && !transformComp->to &&
                      transformComp->by;
  if (!sampled.empty() && !underlying.empty() && (transformComp->additive || byOnly)) {
    sampled = underlying + " " + sampled;
  }
  return std::pair<std::string, std::string>{transformComp->attributeName, std::move(sampled)};
}

std::optional<Lengthd> SampleContextualLengthRecipe(const ContextualLengthRecipe& recipe,
                                                    Lengthd underlying,
                                                    const AnimationValueContext& context) {
  std::optional<Lengthd> result;
  for (const auto& sample : recipe.samples) {
    const auto value = SampleAnimationValue(sample.progress, sample.iteration,
                                            std::get_if<SetAnimationComponent>(&sample.declaration),
                                            std::get_if<AnimateValueComponent>(&sample.declaration),
                                            nullptr, underlying.toRcString().str(), context);
    if (!value) {
      continue;
    }
    const auto parsed = ParseAnimationLength(value->first, value->second);
    if (parsed) {
      result = *parsed;
      underlying = *result;
    }
  }
  return result;
}

}  // namespace donner::svg::components
