#include "donner/svg/components/animation/AnimationValueSampler.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <sstream>
#include <type_traits>
#include <vector>

#include "donner/base/parser/NumberParser.h"
#include "donner/svg/components/animation/AnimationPathValue.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"

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

bool ValidNumericDeclaration(const AnimateValueComponent& value, const std::string& underlying,
                             const AnimationValueContext& context) {
  const auto valid = [&](std::string_view operand) {
    return ValidAnimationNumericOperand(operand, context);
  };
  if (!value.values.empty()) {
    return std::all_of(value.values.begin(), value.values.end(), valid);
  }
  if (value.to) {
    return valid(value.from.value_or(underlying)) && valid(*value.to);
  }
  return value.by && valid(*value.by) && (!value.from || valid(*value.from));
}

std::optional<std::vector<std::string>> BuildAnimationValues(const AnimateValueComponent& value,
                                                             const std::string& underlying,
                                                             const AnimationValueContext& context) {
  if (!value.values.empty()) {
    return value.values;
  }
  if (value.to) {
    return std::vector<std::string>{value.from.value_or(underlying), *value.to};
  }
  if (!value.by || context.referenceValue) {
    return std::vector<std::string>{};
  }
  const auto from =
      value.from ? value.from : CombineAnimationValues(*value.by, *value.by, 0, 0, context);
  if (!from) {
    return std::nullopt;
  }
  const auto to = CombineAnimationValues(*from, *value.by, 1, 1, context, true);
  return to ? std::optional(std::vector<std::string>{*from, *to}) : std::nullopt;
}

std::vector<double> AnimationDistances(std::span<const std::string> values,
                                       const AnimationValueContext& context,
                                       AnimationInterpolation& options) {
  std::vector<double> distances;
  if (options.calcMode != CalcMode::Paced) {
    return distances;
  }
  for (std::size_t i = 1; i < values.size(); ++i) {
    const auto distance = AnimationValueDistance(values[i - 1], values[i], context);
    if (!distance) {
      options.calcMode = CalcMode::Discrete;
      options.keyTimes.clear();
      return {};
    }
    distances.push_back(*distance);
  }
  return distances;
}

/// Interpolate a value for `<animate>` at the given progress.
/// Returns a value (which may be empty), or nullopt if interpolation failed.
std::optional<std::string> interpolateAnimateValue(double progress,
                                                   const AnimateValueComponent& valueComp,
                                                   const std::string& underlying,
                                                   const AnimationValueContext& context) {
  if (!ValidNumericDeclaration(valueComp, underlying, context)) {
    return std::nullopt;
  }
  const auto values = BuildAnimationValues(valueComp, underlying, context);
  if (!values) {
    return {};
  }
  AnimationInterpolation options = valueComp;
  if (context.referenceValue) {
    options.calcMode = CalcMode::Discrete;
  }
  const std::vector<double> distances = AnimationDistances(*values, context, options);
  const auto segment = SelectAnimationSegment(progress, values->size(), options, distances);
  if (!segment) {
    return {};
  }
  if (segment->from == segment->to) {
    return (*values)[segment->from];
  }
  const auto& from = (*values)[segment->from];
  const auto& to = (*values)[segment->to];
  if (auto result =
          CombineAnimationValues(from, to, 1 - segment->progress, segment->progress, context)) {
    return *result;
  }
  options.calcMode = CalcMode::Discrete;
  const auto discrete = SelectAnimationSegment(progress, values->size(), options);
  return discrete ? std::optional((*values)[discrete->from]) : std::nullopt;
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
  oss.precision(std::numeric_limits<double>::max_digits10);

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

std::vector<double> ParseTransformTuple(std::string_view value, TransformAnimationType type) {
  std::vector<double> numbers = parseNumbers(value);
  if (numbers.empty() || numbers.size() > static_cast<std::size_t>(expectedNumberCount(type)) ||
      !std::all_of(numbers.begin(), numbers.end(),
                   [](double number) { return std::isfinite(number); })) {
    return {};
  }
  padToExpected(numbers, type);
  return numbers;
}

std::vector<std::vector<double>> BuildByTransformKeyframes(
    const AnimateTransformComponent& transform) {
  std::vector<double> from = transform.from ? ParseTransformTuple(*transform.from, transform.type)
                                            : zeroTransformValues(transform.type);
  const std::vector<double> by = ParseTransformTuple(*transform.by, transform.type);
  if (from.empty() || by.empty()) {
    return {};
  }
  std::vector<double> to = from;
  for (size_t i = 0; i < to.size() && i < by.size(); ++i) {
    to[i] += by[i];
  }
  return {std::move(from), std::move(to)};
}

std::vector<std::vector<double>> BuildTransformKeyframes(
    const AnimateTransformComponent& transform) {
  std::vector<std::vector<double>> keyframes;
  const auto parse = [&](const std::string& value) {
    return ParseTransformTuple(value, transform.type);
  };
  if (!transform.values.empty()) {
    keyframes.reserve(transform.values.size());
    std::transform(transform.values.begin(), transform.values.end(), std::back_inserter(keyframes),
                   parse);
  } else if (transform.from && transform.to) {
    keyframes = {parse(*transform.from), parse(*transform.to)};
  } else if (transform.by) {
    keyframes = BuildByTransformKeyframes(transform);
  } else if (transform.to) {
    keyframes.push_back(parse(*transform.to));
  }
  return keyframes;
}

std::vector<double> TransformDistances(std::span<const std::vector<double>> keyframes,
                                       TransformAnimationType type, CalcMode mode) {
  std::vector<double> distances;
  if (mode != CalcMode::Paced) {
    return distances;
  }
  for (std::size_t i = 1; i < keyframes.size(); ++i) {
    double distance = 0;
    const std::size_t count = type == TransformAnimationType::Rotate ? 1 : keyframes[i].size();
    for (std::size_t j = 0; j < count; ++j) {
      distance = std::hypot(distance, keyframes[i][j] - keyframes[i - 1][j]);
    }
    distances.push_back(distance);
  }
  return distances;
}

void AccumulateTransform(std::vector<double>& values, std::span<const double> last, bool accumulate,
                         double iteration) {
  if (!accumulate) {
    return;
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    values[i] += iteration * last[i];
  }
}

/// Interpolate transform values for <animateTransform>.
std::string interpolateTransformValue(double progress,
                                      const AnimateTransformComponent& transformComp,
                                      double iteration = 0) {
  const TransformAnimationType type = transformComp.type;
  std::vector<std::vector<double>> keyframes = BuildTransformKeyframes(transformComp);

  if (std::any_of(keyframes.begin(), keyframes.end(),
                  [](const auto& frame) { return frame.empty(); })) {
    return {};
  }
  const std::vector<double> distances = TransformDistances(keyframes, type, transformComp.calcMode);
  const auto segment = SelectAnimationSegment(progress, keyframes.size(), transformComp, distances);
  if (!segment) {
    return {};
  }
  if (segment->from == segment->to) {
    auto result = keyframes[segment->from];
    AccumulateTransform(result, keyframes.back(), transformComp.accumulate, iteration);
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
  }

  AccumulateTransform(result, keyframes.back(), transformComp.accumulate, iteration);

  return formatTransform(type, result);
}

std::string RoundAnimationIntegers(const std::string& input) {
  const auto numbers = ParseAnimationLengths(input);
  if (!numbers) {
    return {};
  }
  std::ostringstream output;
  output.precision(std::numeric_limits<double>::max_digits10);
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

bool AddRecipeSourceBytes(std::size_t& total, std::size_t bytes) {
  if (bytes > AnimationResourceBudget::kMaximumSourceBytes - total) {
    return false;
  }
  total += bytes;
  return true;
}

std::optional<std::size_t> RecipeSampleSourceBytes(const ContextualLengthSample& sample) {
  return std::visit(
      [](const auto& declaration) -> std::optional<std::size_t> {
        std::size_t bytes = 0;
        const auto add = [&](const std::optional<std::string>& value) {
          return !value || AddRecipeSourceBytes(bytes, value->size());
        };
        if (!AddRecipeSourceBytes(bytes, declaration.attributeName.size()) ||
            !add(declaration.href) || !add(declaration.to)) {
          return std::nullopt;
        }
        if constexpr (std::is_same_v<std::decay_t<decltype(declaration)>, AnimateValueComponent>) {
          if (declaration.keyTimes.size() >
                  AnimationResourceBudget::kMaximumSourceBytes / sizeof(double) ||
              declaration.keySplines.size() >
                  AnimationResourceBudget::kMaximumSourceBytes / sizeof(double) ||
              declaration.values.size() >
                  AnimationResourceBudget::kMaximumOutputBytes / (2 * sizeof(std::string)) ||
              !AddRecipeSourceBytes(bytes, declaration.keyTimes.size() * sizeof(double)) ||
              !AddRecipeSourceBytes(bytes, declaration.keySplines.size() * sizeof(double)) ||
              !add(declaration.from) || !add(declaration.by)) {
            return std::nullopt;
          }
          for (const auto& value : declaration.values) {
            if (!AddRecipeSourceBytes(bytes, value.size())) {
              return std::nullopt;
            }
          }
        }
        return bytes;
      },
      sample.declaration);
}

bool RecipeStorageFits(const std::vector<ContextualLengthSample>& samples, std::size_t limit) {
  std::size_t retained = sizeof(ContextualLengthRecipe) + 64;
  const auto add = [&](std::size_t count, std::size_t size = 1) {
    if (retained > limit || count > (limit - retained) / size) {
      return false;
    }
    retained += count * size;
    return true;
  };
  if (!add(samples.capacity(), sizeof(ContextualLengthSample))) {
    return false;
  }
  for (const auto& sample : samples) {
    const bool fits = std::visit(
        [&](const auto& declaration) {
          const auto addString = [&](const std::optional<std::string>& value) {
            return !value || add(value->capacity() + 1);
          };
          if (!add(declaration.attributeName.capacity() + 1) || !addString(declaration.href) ||
              !addString(declaration.to)) {
            return false;
          }
          if constexpr (std::is_same_v<std::decay_t<decltype(declaration)>,
                                       AnimateValueComponent>) {
            if (!add(declaration.values.capacity(), sizeof(std::string)) ||
                !add(declaration.keyTimes.capacity(), sizeof(double)) ||
                !add(declaration.keySplines.capacity(), sizeof(double)) ||
                !addString(declaration.from) || !addString(declaration.by)) {
              return false;
            }
            for (const auto& value : declaration.values) {
              if (!add(value.capacity() + 1)) {
                return false;
              }
            }
          }
          return true;
        },
        sample.declaration);
    if (!fits) {
      return false;
    }
  }
  return true;
}

struct RecipeMetrics {
  std::size_t source = 0;
  std::size_t retained = 0;
  std::size_t work = 0;
  std::size_t depth = 1;
};

bool AddFrozenUnderlyingMetrics(const ContextualLengthSample& sample, RecipeMetrics& metrics) {
  constexpr std::size_t kMaximumFrozenUnderlyingDepth = 256;
  if (!sample.frozenUnderlying) {
    return true;
  }
  if (!AddRecipeSourceBytes(metrics.source, sample.frozenUnderlying->sourceBytes) ||
      sample.frozenUnderlying->retainedBytes >
          AnimationResourceBudget::kMaximumOutputBytes - metrics.retained ||
      sample.frozenUnderlying->evaluationWork >
          AnimationResourceBudget::kMaximumContextualWork - metrics.work ||
      sample.frozenUnderlying->nestingDepth >= kMaximumFrozenUnderlyingDepth) {
    return false;
  }
  metrics.retained += sample.frozenUnderlying->retainedBytes;
  metrics.work += sample.frozenUnderlying->evaluationWork;
  metrics.depth = std::max(metrics.depth, sample.frozenUnderlying->nestingDepth + 1);
  return true;
}

bool AddRecipeSampleMetrics(const ContextualLengthSample& sample, RecipeMetrics& metrics) {
  const auto bytes = RecipeSampleSourceBytes(sample);
  if (!bytes || !AddRecipeSourceBytes(metrics.source, *bytes)) {
    return false;
  }
  const auto* value = std::get_if<AnimateValueComponent>(&sample.declaration);
  const std::size_t keyframes = value ? value->values.size() : 0;
  const std::size_t storage =
      2 * (sizeof(ContextualLengthSample) + *bytes + keyframes * sizeof(std::string)) + 256;
  if (*bytes > AnimationResourceBudget::kMaximumContextualWork / 16 ||
      keyframes > AnimationResourceBudget::kMaximumContextualWork / 64) {
    return false;
  }
  const std::size_t sampleWork = *bytes * 16 + keyframes * 64 + 256;
  if (storage > AnimationResourceBudget::kMaximumOutputBytes - metrics.retained ||
      sampleWork > AnimationResourceBudget::kMaximumContextualWork - metrics.work) {
    return false;
  }
  metrics.retained += storage;
  metrics.work += sampleWork;
  return AddFrozenUnderlyingMetrics(sample, metrics);
}

}  // namespace

std::optional<std::pair<std::string, std::string>> SampleAnimationValue(
    double progress, double iteration, const SetAnimationComponent* setComp,
    const AnimateValueComponent* valueComp, const AnimateTransformComponent* transformComp,
    const std::string& underlying, const AnimationValueContext& context,
    AnimationResourceBudget* budget) {
  const std::string_view attribute = setComp     ? std::string_view(setComp->attributeName)
                                     : valueComp ? std::string_view(valueComp->attributeName)
                                                 : std::string_view{};
  if (attribute == "d" || attribute == "path") {
    AnimationResourceBudget localBudget;
    auto path = SampleAnimationPath(progress, iteration, setComp, valueComp, underlying,
                                    budget ? *budget : localBudget);
    return path ? std::optional(
                      std::pair<std::string, std::string>{std::string(attribute), std::move(*path)})
                : std::nullopt;
  }
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
    const bool toOnly = valueComp->values.empty() && !valueComp->from && valueComp->to;
    const bool byOnly =
        valueComp->values.empty() && !valueComp->from && !valueComp->to && valueComp->by;
    AnimationValueContext valueContext = context;
    valueContext.lengthValuesAreOffsets = !toOnly && (valueComp->additive || byOnly);
    auto value = interpolateAnimateValue(progress, *valueComp, underlying, valueContext);
    if (!value) {
      return std::nullopt;
    }
    std::string sampled = std::move(*value);
    if (!context.referenceValue && valueComp->accumulate && !toOnly && iteration > 0) {
      AnimationValueContext accumulatedContext = valueContext;
      accumulatedContext.lengthValuesAreOffsets = true;
      const auto last = interpolateAnimateValue(1, *valueComp, underlying, accumulatedContext);
      if (last) {
        sampled = CombineAnimationValues(sampled, *last, 1, iteration, valueContext, true)
                      .value_or(sampled);
      }
    }
    if (!context.referenceValue && !sampled.empty() && !toOnly && (valueComp->additive || byOnly)) {
      sampled = CombineAnimationValues(underlying, sampled, 1, 1, context, true).value_or(sampled);
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

std::shared_ptr<const ContextualLengthRecipe> ContextualLengthRecipe::Create(
    std::vector<ContextualLengthSample>&& samples) {
  if (samples.size() > AnimationResourceBudget::kMaximumAnimations) {
    return nullptr;
  }
  RecipeMetrics metrics;
  for (const auto& sample : samples) {
    if (!AddRecipeSampleMetrics(sample, metrics)) {
      return nullptr;
    }
  }
  if (!RecipeStorageFits(samples, metrics.retained)) {
    return nullptr;
  }
  return std::shared_ptr<const ContextualLengthRecipe>(new ContextualLengthRecipe(
      std::move(samples), metrics.source, metrics.retained, metrics.work, metrics.depth));
}

std::optional<Lengthd> SampleContextualLengthRecipe(const ContextualLengthRecipe& recipe,
                                                    Lengthd underlying,
                                                    const AnimationValueContext& context) {
  return SampleContextualAnimationRecipe(
      recipe, underlying, [&](const ContextualLengthSample& sample, Lengthd lower) {
        const auto value =
            SampleAnimationValue(sample.progress, sample.iteration,
                                 std::get_if<SetAnimationComponent>(&sample.declaration),
                                 std::get_if<AnimateValueComponent>(&sample.declaration), nullptr,
                                 lower.toRcString().str(), context);
        if (!value) {
          return std::optional<Lengthd>{};
        }
        const auto parsed = ParseAnimationLength(value->first, value->second);
        return parsed ? std::optional(*parsed) : std::nullopt;
      });
}

}  // namespace donner::svg::components
