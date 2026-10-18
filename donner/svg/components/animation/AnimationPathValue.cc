#include "donner/svg/components/animation/AnimationPathValue.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#include "donner/base/StringUtils.h"
#include "donner/svg/parser/PathParser.h"

namespace donner::svg::components {
namespace {

using PathValue = parser::PathParser::CommandList;

bool ReserveArray(AnimationResourceBudget& budget, std::size_t count, std::size_t width) {
  return count <= AnimationResourceBudget::kMaximumContextualWork / width &&
         budget.reserveContextualWork(count * width);
}

std::optional<PathValue> ParsePath(std::string_view source, AnimationResourceBudget& budget) {
  if (source.size() > AnimationResourceBudget::kMaximumPathValueBytes ||
      !ReserveArray(budget, source.size(), sizeof(parser::PathParser::Command) + sizeof(double))) {
    return std::nullopt;
  }
  source = StringUtils::TrimWhitespace(source);
  if (StringUtils::EqualsLowercase(source, std::string_view("none"))) {
    return PathValue{};
  }
  auto parsed = parser::PathParser::ParseCommands(source);
  // Static d parsing also retains the completed prefix when a later command is malformed.
  if (!parsed.hasResult()) {
    return std::nullopt;
  }
  return std::move(parsed.result());
}

bool Compatible(const PathValue& first, const PathValue& second) {
  if (first.commands.size() != second.commands.size() ||
      first.parameters.size() != second.parameters.size()) {
    return false;
  }
  for (std::size_t i = 0; i < first.commands.size(); ++i) {
    if (first.commands[i].letter != second.commands[i].letter ||
        first.commands[i].parameterCount != second.commands[i].parameterCount) {
      return false;
    }
  }
  return true;
}

std::optional<PathValue> CopyPath(const PathValue& source, AnimationResourceBudget& budget) {
  if (!ReserveArray(budget, source.commands.size(), sizeof(parser::PathParser::Command)) ||
      !ReserveArray(budget, source.parameters.size(), sizeof(double))) {
    return std::nullopt;
  }
  return source;
}

std::optional<PathValue> CombinePaths(const PathValue& first, const PathValue& second,
                                      double firstWeight, double secondWeight,
                                      AnimationResourceBudget& budget) {
  if (!Compatible(first, second)) {
    return std::nullopt;
  }
  auto result = CopyPath(first, budget);
  if (!result) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < result->parameters.size(); ++i) {
    const double number = first.parameters[i] * firstWeight + second.parameters[i] * secondWeight;
    if (!std::isfinite(number)) {
      return std::nullopt;
    }
    result->parameters[i] = number;
  }
  return result;
}

std::optional<std::string> SerializePath(const PathValue& path, AnimationResourceBudget& budget) {
  constexpr std::size_t kNumberBytes = std::numeric_limits<double>::max_digits10 + 10;
  const std::size_t maximumBytes = path.commands.size() + path.parameters.size() * kNumberBytes;
  if (!budget.reserveContextualWork(maximumBytes)) {
    return std::nullopt;
  }
  std::string result;
  result.reserve(maximumBytes);
  for (const auto& command : path.commands) {
    result.push_back(command.letter);
    for (std::size_t i = 0; i < command.parameterCount; ++i) {
      if (i != 0) {
        result.push_back(' ');
      }
      const double number = path.parameters[command.parameterOffset + i];
      if ((command.letter == 'A' || command.letter == 'a') && (i == 3 || i == 4)) {
        result.push_back(number != 0 ? '1' : '0');
      } else {
        char buffer[kNumberBytes];
        const auto converted =
            std::to_chars(buffer, buffer + sizeof(buffer), number, std::chars_format::general,
                          std::numeric_limits<double>::max_digits10);
        if (converted.ec != std::errc()) {
          return std::nullopt;
        }
        result.append(buffer, converted.ptr);
      }
    }
  }
  return result;
}

struct PathOperands {
  std::vector<PathValue> values;
  std::optional<PathValue> base;
};

bool ReadValues(PathOperands& paths, const AnimateValueComponent& value,
                AnimationResourceBudget& budget) {
  for (const auto& text : value.values) {
    auto parsed = ParsePath(text, budget);
    if (!parsed) {
      return false;
    }
    paths.values.push_back(std::move(*parsed));
  }
  return true;
}

bool ReadFromTo(PathOperands& paths, const AnimateValueComponent& value,
                AnimationResourceBudget& budget) {
  auto from = value.from ? ParsePath(*value.from, budget) : CopyPath(*paths.base, budget);
  auto to = ParsePath(*value.to, budget);
  if (!from || !to) {
    return false;
  }
  paths.values.push_back(std::move(*from));
  paths.values.push_back(std::move(*to));
  return true;
}

bool ReadFromBy(PathOperands& paths, const AnimateValueComponent& value,
                AnimationResourceBudget& budget) {
  auto by = ParsePath(*value.by, budget);
  if (!by) {
    return false;
  }
  auto from = value.from ? ParsePath(*value.from, budget) : CombinePaths(*by, *by, 0, 0, budget);
  if (!from) {
    return false;
  }
  auto to = CombinePaths(*from, *by, 1, 1, budget);
  if (!to) {
    return false;
  }
  paths.values.push_back(std::move(*from));
  paths.values.push_back(std::move(*to));
  return true;
}

std::optional<PathOperands> PreparePaths(const AnimateValueComponent& value,
                                         std::string_view underlying, bool needsBase,
                                         AnimationResourceBudget& budget) {
  const std::size_t count = value.values.empty() ? 2 : value.values.size();
  if (!value.valid || !ReserveArray(budget, count, sizeof(PathValue))) {
    return std::nullopt;
  }
  PathOperands paths;
  paths.values.reserve(count);
  if (needsBase) {
    paths.base = ParsePath(underlying, budget);
    if (!paths.base) {
      return std::nullopt;
    }
  }
  const bool valid = !value.values.empty() ? ReadValues(paths, value, budget)
                     : value.to            ? ReadFromTo(paths, value, budget)
                     : value.by            ? ReadFromBy(paths, value, budget)
                                           : false;
  return valid ? std::optional(std::move(paths)) : std::nullopt;
}

bool AllCompatible(const std::vector<PathValue>& values, const PathValue& base) {
  return std::all_of(values.begin(), values.end(),
                     [&](const auto& value) { return Compatible(value, base); });
}

std::optional<PathValue> InterpolatePath(const PathOperands& paths, double progress,
                                         const AnimateValueComponent& value,
                                         AnimationResourceBudget& budget) {
  if (!ReserveArray(budget, value.keyTimes.size(), sizeof(double)) ||
      !ReserveArray(budget, value.keySplines.size(), sizeof(double))) {
    return std::nullopt;
  }
  AnimationInterpolation options = value;
  if (options.calcMode == CalcMode::Paced) {
    // SVG defines no distance for path data; use its permitted linear fallback.
    options.calcMode = CalcMode::Linear;
    options.keyTimes.clear();
    options.keySplines.clear();
  }
  if (!AllCompatible(paths.values, paths.values.front())) {
    options.calcMode = CalcMode::Discrete;
  }
  const auto segment = SelectAnimationSegment(progress, paths.values.size(), options);
  if (!segment) {
    return std::nullopt;
  }
  const auto& from = paths.values[segment->from];
  const auto& to = paths.values[segment->to];
  if (segment->from == segment->to) {
    return CopyPath(from, budget);
  }
  return CombinePaths(from, to, 1 - segment->progress, segment->progress, budget);
}

std::optional<parser::PathParser::Command> GeometryCommand(const Path::Command& command,
                                                           std::size_t offset) {
  char letter;
  uint8_t count;
  switch (command.verb) {
    case Path::Verb::MoveTo:
      letter = 'M';
      count = 2;
      break;
    case Path::Verb::LineTo:
      letter = 'L';
      count = 2;
      break;
    case Path::Verb::QuadTo:
      letter = 'Q';
      count = 4;
      break;
    case Path::Verb::CurveTo:
      letter = 'C';
      count = 6;
      break;
    case Path::Verb::ClosePath:
      letter = 'Z';
      count = 0;
      break;
    default: return std::nullopt;
  }
  return parser::PathParser::Command{static_cast<uint32_t>(offset), count, letter};
}

}  // namespace

std::optional<std::string> SerializeAnimationPathGeometry(const Path& path,
                                                          AnimationResourceBudget& budget) {
  const std::size_t count = path.commands().size();
  if (count > parser::PathParser::kMaximumCommands ||
      !ReserveArray(budget, count, sizeof(parser::PathParser::Command) + 6 * sizeof(double))) {
    return std::nullopt;
  }
  PathValue value;
  value.commands.reserve(count);
  value.parameters.reserve(count * 6);
  for (const auto& command : path.commands()) {
    const auto converted = GeometryCommand(command, value.parameters.size());
    if (!converted || command.pointIndex > path.points().size() ||
        converted->parameterCount / 2 > path.points().size() - command.pointIndex) {
      return std::nullopt;
    }
    value.commands.push_back(*converted);
    for (std::size_t i = 0; i < converted->parameterCount / 2; ++i) {
      const auto point = path.points()[command.pointIndex + i];
      if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
        return std::nullopt;
      }
      value.parameters.push_back(point.x);
      value.parameters.push_back(point.y);
    }
  }
  auto result = SerializePath(value, budget);
  if (!result || result->size() > AnimationResourceBudget::kMaximumPathValueBytes ||
      !budget.reserveSourceBytes(result->size())) {
    return std::nullopt;
  }
  return result;
}

std::optional<std::string> SampleAnimationPath(double progress, double iteration,
                                               const SetAnimationComponent* set,
                                               const AnimateValueComponent* value,
                                               std::string_view underlying,
                                               AnimationResourceBudget& budget) {
  if (set) {
    if (!set->to) {
      return std::nullopt;
    }
    const auto parsed = ParsePath(*set->to, budget);
    return parsed ? SerializePath(*parsed, budget) : std::nullopt;
  }
  const bool toOnly = value->values.empty() && !value->from && value->to;
  const bool byOnly = value->values.empty() && !value->from && !value->to && value->by;
  const bool additive = !toOnly && (value->additive || byOnly);
  const bool accumulate = !toOnly && value->accumulate;
  const auto paths = PreparePaths(*value, underlying, toOnly || additive, budget);
  if (!paths || (additive && !AllCompatible(paths->values, *paths->base)) ||
      (accumulate && !AllCompatible(paths->values, paths->values.back()))) {
    return std::nullopt;
  }
  auto sampled = InterpolatePath(*paths, progress, *value, budget);
  if (sampled && accumulate && iteration > 0) {
    sampled = CombinePaths(*sampled, paths->values.back(), 1, iteration, budget);
  }
  if (sampled && additive) {
    sampled = CombinePaths(*paths->base, *sampled, 1, 1, budget);
  }
  return sampled ? SerializePath(*sampled, budget) : std::nullopt;
}

}  // namespace donner::svg::components
