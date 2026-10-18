#include "donner/svg/components/animation/AnimationTimeline.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>

#include "donner/base/StringUtils.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimationReference.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationStateComponent.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"
#include "donner/svg/parser/AnimationAttributeParser.h"
#include "donner/svg/parser/ClockValueParser.h"

namespace donner::svg::components {
namespace {
constexpr std::size_t kMaximumInstances = 1024;
constexpr std::size_t kMaximumTimelineIntervals = 16384;
constexpr std::size_t kMaximumDependencyUpdates = 65536;
constexpr std::size_t kMaximumDependencySteps = 262144;

std::size_t TimeOffsetSign(std::string_view item) {
  std::size_t sign = item.find_last_of("+-");
  while (sign != std::string_view::npos && sign > 1 &&
         (item[sign - 1] == 'e' || item[sign - 1] == 'E') && item[sign - 2] >= '0' &&
         item[sign - 2] <= '9') {
    sign = item.find_last_of("+-", sign - 1);
  }
  return sign;
}

bool ParseRepeatSpecifier(std::string_view item, AnimationTimeSpecifier& result) {
  if (!item.starts_with("repeat(") || !item.ends_with(')')) {
    return false;
  }
  const auto number = item.substr(7, item.size() - 8);
  const auto [end, error] =
      std::from_chars(number.data(), number.data() + number.size(), result.repeat);
  if (error != std::errc{} || end != number.data() + number.size() || result.repeat == 0) {
    return false;
  }
  result.kind = AnimationTimeSpecifier::Kind::Repeat;
  return true;
}

bool ExtractTimeSource(std::string_view& item, AnimationTimeSpecifier& result) {
  const std::size_t dot = item.rfind('.');
  if (dot == std::string_view::npos) {
    return true;
  }
  result.sourceId = item.substr(0, dot);
  item.remove_prefix(dot + 1);
  return !result.sourceId.empty();
}

bool AssignTimeSpecifierKind(std::string_view item, AnimationTimeSpecifier& result) {
  result.kind = AnimationTimeSpecifier::Kind::Event;
  if (!result.sourceId.empty() && item == "begin") {
    result.kind = AnimationTimeSpecifier::Kind::Begin;
  } else if (!result.sourceId.empty() && item == "end") {
    result.kind = AnimationTimeSpecifier::Kind::End;
  } else if (!result.sourceId.empty() && item.starts_with("repeat(")) {
    return ParseRepeatSpecifier(item, result);
  } else {
    if (item.empty() || item.find_first_of(" \t\r\n()+") != std::string_view::npos) {
      return false;
    }
    result.event = item;
  }
  return true;
}

std::optional<AnimationTimeSpecifier> ParseTimeSpecifier(std::string_view item) {
  auto clock = parser::ClockValueParser::Parse(item);
  if (clock.hasResult()) {
    if (std::isfinite(clock.result().seconds())) {
      return AnimationTimeSpecifier{.offset = clock.result().seconds()};
    }
    return std::nullopt;
  }
  AnimationTimeSpecifier result;
  // Only one clock parse: repeated suffix parsing makes long hostile names quadratic.
  const std::size_t sign = TimeOffsetSign(item);
  if (sign != std::string_view::npos) {
    auto offset = parser::ClockValueParser::Parse(item.substr(sign));
    if (offset.hasResult() && std::isfinite(offset.result().seconds())) {
      result.offset = offset.result().seconds();
      item = StringUtils::TrimWhitespace(item.substr(0, sign));
    }
  }
  if (!ExtractTimeSource(item, result) || !AssignTimeSpecifierKind(item, result)) {
    return std::nullopt;
  }
  return result;
}

struct TimelineNode {
  Entity entity;
  std::vector<AnimationTimeSpecifier> begins;
  std::vector<AnimationTimeSpecifier> ends;
  std::vector<AnimationInterval> intervals;
  std::vector<std::size_t> dependents;
  double simpleDuration = 0;
  bool queued = true;
};

using NodeIndices = std::unordered_map<Entity, std::size_t>;

std::optional<std::size_t> SourceIndex(Registry& registry, const NodeIndices& indices,
                                       const AnimationTimeSpecifier& specifier,
                                       AnimationResourceBudget& budget) {
  if (specifier.kind == AnimationTimeSpecifier::Kind::Event || specifier.sourceId.empty()) {
    return std::nullopt;
  }
  const Entity entity = FindAnimationReference(registry, specifier.sourceId, budget);
  const auto it = indices.find(entity);
  return it == indices.end() ? std::nullopt : std::optional(it->second);
}

bool AppendInstance(std::vector<double>& times, double time) {
  if (!std::isfinite(time)) {
    return true;
  }
  const auto pos = std::lower_bound(times.begin(), times.end(), time);
  if (pos != times.end() && *pos == time) {
    return true;
  }
  if (times.size() >= kMaximumInstances) {
    return false;
  }
  times.insert(pos, time);
  return true;
}

bool ResolveInstances(Registry& registry, const NodeIndices& indices,
                      const std::vector<TimelineNode>& nodes,
                      const std::vector<AnimationTimeSpecifier>& specs, std::vector<double>& times,
                      std::size_t& steps, AnimationResourceBudget& budget) {
  for (const auto& spec : specs) {
    if (++steps > kMaximumDependencySteps) {
      return false;
    }
    if (spec.kind == AnimationTimeSpecifier::Kind::Offset) {
      if (!AppendInstance(times, spec.offset)) {
        return false;
      }
      continue;
    }
    const auto sourceIndex = SourceIndex(registry, indices, spec, budget);
    if (budget.rejected()) {
      return false;
    }
    if (!sourceIndex) {
      continue;
    }
    const auto& source = nodes[*sourceIndex];
    for (const auto& interval : source.intervals) {
      if (++steps > kMaximumDependencySteps) {
        return false;
      }
      double value = interval.begin;
      if (spec.kind == AnimationTimeSpecifier::Kind::End) {
        value = interval.end;
      } else if (spec.kind == AnimationTimeSpecifier::Kind::Repeat) {
        value += source.simpleDuration * spec.repeat;
        const double repeatEnd = std::min(interval.end, interval.begin + interval.repeatDuration);
        if (!(value < repeatEnd) || source.simpleDuration <= 0) {
          continue;
        }
      }
      if (!AppendInstance(times, value + spec.offset)) {
        return false;
      }
    }
  }
  std::sort(times.begin(), times.end());
  return true;
}

double NaturalDuration(const AnimationTimingComponent& timing, double simple) {
  double duration = simple;
  if (timing.repeatCount || timing.repeatDur) {
    duration =
        timing.repeatCount ? simple * *timing.repeatCount : std::numeric_limits<double>::infinity();
    if (timing.repeatDur) {
      duration = std::min(duration, timing.repeatDur->seconds());
    }
  }
  return std::max(0.0, duration);
}

std::vector<AnimationInterval> BuildIntervals(const AnimationTimingComponent& timing, double simple,
                                              const std::vector<double>& begins,
                                              const std::vector<double>& ends) {
  std::vector<AnimationInterval> result;
  const double natural = NaturalDuration(timing, simple);
  for (double begin : begins) {
    if (!result.empty()) {
      if (timing.restart == AnimationRestart::Never) {
        break;
      }
      if (begin < result.back().end) {
        if (timing.restart == AnimationRestart::WhenNotActive) {
          continue;
        }
        result.back().end = begin;
      }
    }
    double duration = natural;
    const auto end = std::lower_bound(ends.begin(), ends.end(), begin);
    if (end != ends.end()) {
      duration = std::min(duration, *end - begin);
    }
    const double minimum = timing.min ? std::max(0.0, timing.min->seconds()) : 0.0;
    const double maximum =
        timing.max ? std::max(0.0, timing.max->seconds()) : std::numeric_limits<double>::infinity();
    if (minimum <= maximum) {
      duration = std::clamp(duration, minimum, maximum);
    }
    result.push_back({begin, begin + duration, natural});
  }
  return result;
}

bool InitializeNodes(Registry& registry, std::span<const Entity> animations,
                     std::vector<TimelineNode>& nodes, NodeIndices& indices,
                     AnimationResourceBudget& budget) {
  std::size_t bytes = 0;
  for (Entity entity : animations) {
    const auto& timing = registry.get<AnimationTimingComponent>(entity);
    bytes += timing.beginValue ? timing.beginValue->size() : 0;
    bytes += timing.endValue ? timing.endValue->size() : 0;
    if (bytes > AnimationResourceBudget::kMaximumSourceBytes) {
      return false;
    }
    auto begins = ParseAnimationTimeList(timing.beginValue.value_or("0s"));
    auto ends = ParseAnimationTimeList(timing.endValue.value_or(""));
    if (!begins || !ends) {
      return false;
    }
    const double simple =
        timing.dur ? timing.dur->seconds() : std::numeric_limits<double>::infinity();
    indices.emplace(entity, nodes.size());
    nodes.push_back({.entity = entity,
                     .begins = std::move(*begins),
                     .ends = std::move(*ends),
                     .simpleDuration = std::max(0.0, simple)});
  }
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    for (const auto* specs : {&nodes[i].begins, &nodes[i].ends}) {
      for (const auto& spec : *specs) {
        if (const auto source = SourceIndex(registry, indices, spec, budget)) {
          auto& dependents = nodes[*source].dependents;
          if (std::find(dependents.begin(), dependents.end(), i) == dependents.end()) {
            dependents.push_back(i);
          }
        }
        if (budget.rejected()) {
          return false;
        }
      }
    }
  }
  return true;
}

void SelectInterval(AnimationStateComponent& state, const TimelineNode& node, double time) {
  state.phase = AnimationPhase::Before;
  state.simpleDuration = node.simpleDuration;
  state.intervals = node.intervals;
  for (const auto& interval : node.intervals) {
    if (interval.begin > time) {
      break;
    }
    state.beginTime = interval.begin;
    state.activeDuration = interval.end - interval.begin;
    state.repeatDuration = interval.repeatDuration;
    state.phase = time < interval.end ? AnimationPhase::Active : AnimationPhase::After;
  }
}

}  // namespace

std::optional<std::vector<AnimationTimeSpecifier>> ParseAnimationTimeList(std::string_view value) {
  if (value.size() > AnimationResourceBudget::kMaximumSourceBytes) {
    return std::nullopt;
  }
  std::vector<AnimationTimeSpecifier> result;
  std::size_t count = 0;
  while (!value.empty()) {
    if (++count > kMaximumInstances) {
      return std::nullopt;
    }
    const auto separator = value.find(';');
    if (auto spec = ParseTimeSpecifier(StringUtils::TrimWhitespace(value.substr(0, separator)))) {
      result.push_back(std::move(*spec));
    }
    if (separator == std::string_view::npos) {
      break;
    }
    value.remove_prefix(separator + 1);
  }
  return result;
}

bool InsertAnimationInstance(std::vector<double>& times, double time) {
  if (!std::isfinite(time) || times.size() >= kMaximumInstances) {
    return false;
  }
  const auto pos = std::lower_bound(times.begin(), times.end(), time);
  if (pos != times.end() && *pos == time) {
    return false;
  }
  times.insert(pos, time);
  return true;
}

bool ResolveDependencyQueue(Registry& registry, const NodeIndices& indices,
                            std::vector<TimelineNode>& nodes, std::deque<std::size_t>& queue,
                            AnimationResourceBudget& budget) {
  std::size_t updates = 0;
  std::size_t steps = 0;
  std::size_t totalIntervals = 0;
  while (!queue.empty()) {
    if (++updates > kMaximumDependencyUpdates) {
      return false;
    }
    const auto index = queue.front();
    queue.pop_front();
    auto& node = nodes[index];
    node.queued = false;
    const auto& state = registry.get<AnimationStateComponent>(node.entity);
    auto begins = state.eventBeginTimes;
    auto ends = state.eventEndTimes;
    if (!ResolveInstances(registry, indices, nodes, node.begins, begins, steps, budget) ||
        !ResolveInstances(registry, indices, nodes, node.ends, ends, steps, budget)) {
      return false;
    }
    auto intervals = BuildIntervals(registry.get<AnimationTimingComponent>(node.entity),
                                    node.simpleDuration, begins, ends);
    if (intervals == node.intervals) {
      continue;
    }
    totalIntervals = totalIntervals - node.intervals.size() + intervals.size();
    if (totalIntervals > kMaximumTimelineIntervals) {
      return false;
    }
    node.intervals = std::move(intervals);
    for (std::size_t dependent : node.dependents) {
      if (!nodes[dependent].queued) {
        nodes[dependent].queued = true;
        queue.push_back(dependent);
      }
    }
  }
  return true;
}

bool ResolveAnimationTimeline(Registry& registry, std::span<const Entity> animations, double time) {
  AnimationResourceBudget budget;
  return ResolveAnimationTimeline(registry, animations, time, budget);
}

bool ResolveAnimationTimeline(Registry& registry, std::span<const Entity> animations, double time,
                              AnimationResourceBudget& budget) {
  if (animations.size() > AnimationResourceBudget::kMaximumAnimations ||
      !parser::RefreshAnimationAttributes(registry, animations)) {
    return false;
  }
  for (Entity entity : animations) {
    auto& state = registry.get_or_emplace<AnimationStateComponent>(entity);
    state.phase = AnimationPhase::Before;
    state.intervals.clear();
  }
  if (!std::isfinite(time) || animations.size() > AnimationResourceBudget::kMaximumAnimations) {
    return false;
  }
  std::vector<TimelineNode> nodes;
  NodeIndices indices;
  if (!InitializeNodes(registry, animations, nodes, indices, budget)) {
    return false;
  }
  std::deque<std::size_t> queue;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    queue.push_back(i);
  }
  if (!ResolveDependencyQueue(registry, indices, nodes, queue, budget)) {
    return false;
  }
  for (const auto& node : nodes) {
    SelectInterval(registry.get<AnimationStateComponent>(node.entity), node, time);
  }
  return true;
}

}  // namespace donner::svg::components
