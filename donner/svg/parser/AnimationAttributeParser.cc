#include "donner/svg/parser/AnimationAttributeParser.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "donner/base/parser/NumberParser.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/svg/components/animation/AnimateMotionComponent.h"
#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationTimingComponent.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"
#include "donner/svg/parser/ClockValueParser.h"
#include "donner/svg/parser/ListParser.h"

namespace donner::svg::parser {
namespace {
using xml::XMLQualifiedNameRef;

std::optional<double> ParseNumberNoSuffix(std::string_view value) {
  const auto parsed = donner::parser::NumberParser::Parse(value);
  if (parsed.hasError() || parsed.result().consumedChars != value.size() ||
      !std::isfinite(parsed.result().number)) {
    return std::nullopt;
  }
  return parsed.result().number;
}

struct AnimationParseContext {
  std::optional<ParseDiagnostic> diagnostic;
};

template <typename Href>
bool ParseAnimationHref(EntityHandle handle, Href& href, const XMLQualifiedNameRef& name,
                        std::string_view value) {
  if (name == XMLQualifiedNameRef("href")) {
    href = std::string(value);
    return true;
  }
  if (name != XMLQualifiedNameRef("xlink", "href")) {
    return false;
  }
  const auto* attributes = handle.try_get<donner::components::AttributesComponent>();
  if (!attributes || !attributes->hasAttribute("href")) {
    href = std::string(value);
  }
  return true;
}

/// Parse a semicolon-separated list of values, trimming whitespace.
ParseResult<std::vector<std::string>> parseSemicolonList(std::string_view str) {
  std::vector<std::string> result;
  size_t consumed = 0;
  while (!str.empty()) {
    const auto pos = str.find(';');
    std::string_view item = (pos != std::string_view::npos) ? str.substr(0, pos) : str;
    // Trim whitespace.
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
      item.remove_prefix(1);
    }
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
      item.remove_suffix(1);
    }
    if (!item.empty()) {
      if (result.size() >= ListParser::kMaximumItems) {
        return ParseDiagnostic::Error("Animation list item limit exceeded",
                                      FileOffset::Offset(consumed));
      }
      result.emplace_back(item);
    }
    if (pos == std::string_view::npos) {
      break;
    }
    str.remove_prefix(pos + 1);
    consumed += pos + 1;
  }
  return result;
}

/// Parse a semicolon-separated list of doubles. Each semicolon group may contain multiple
/// whitespace/comma-separated numbers (e.g., keyTimes "0; 0.5; 1").
ParseResult<std::vector<double>> parseDoubleList(std::string_view str) {
  auto itemResult = parseSemicolonList(str);
  if (itemResult.hasError()) {
    return std::move(itemResult.error());
  }

  std::vector<double> result;
  size_t searchOffset = 0;
  for (const std::string& item : itemResult.result()) {
    size_t itemOffset = str.find(item, searchOffset);
    if (itemOffset == std::string_view::npos) {
      itemOffset = searchOffset;
    }
    searchOffset = std::min(str.size(), itemOffset + item.size());
    std::string_view remaining = item;
    while (!remaining.empty()) {
      if (remaining.front() == ' ' || remaining.front() == '\t' || remaining.front() == ',') {
        remaining.remove_prefix(1);
        continue;
      }

      const auto maybeNumber = donner::parser::NumberParser::Parse(remaining);
      if (maybeNumber.hasError()) {
        return ParseDiagnostic::Error("Invalid animation numeric list",
                                      FileOffset::Offset(itemOffset));
      }
      if (result.size() >= ListParser::kMaximumItems) {
        return ParseDiagnostic::Error(
            "Animation numeric list item limit exceeded",
            FileOffset::Offset(itemOffset + item.size() - remaining.size()));
      }
      result.push_back(maybeNumber.result().number);
      remaining.remove_prefix(maybeNumber.result().consumedChars);
    }
  }
  return result;
}

void ParseCalculationMode(components::AnimationInterpolation& options, std::string_view value) {
  if (value == "linear") {
    options.calcMode = components::CalcMode::Linear;
  } else if (value == "discrete") {
    options.calcMode = components::CalcMode::Discrete;
  } else if (value == "paced") {
    options.calcMode = components::CalcMode::Paced;
  } else if (value == "spline") {
    options.calcMode = components::CalcMode::Spline;
  } else {
    options.valid = false;
  }
}

void ParseInterpolationList(AnimationParseContext& context,
                            components::AnimationInterpolation& options,
                            const XMLQualifiedNameRef& name, std::string_view value) {
  auto result = parseDoubleList(value);
  auto& destination =
      name == XMLQualifiedNameRef("keyTimes") ? options.keyTimes : options.keySplines;
  if (result.hasResult() && !result.result().empty()) {
    destination = std::move(result.result());
  } else {
    destination.clear();
    options.valid = false;
    if (result.hasError()) {
      context.diagnostic = std::move(result.error());
    }
  }
}

void ParseAnimationComposition(components::AnimationInterpolation& options,
                               const XMLQualifiedNameRef& name, std::string_view value) {
  bool& destination =
      name == XMLQualifiedNameRef("additive") ? options.additive : options.accumulate;
  destination = value == "sum";
  const auto defaultValue = name == XMLQualifiedNameRef("additive") ? "replace" : "none";
  options.valid &= destination || value == defaultValue;
}

/// Parse attributes shared by value, transform, and motion interpolation.
bool ParseAnimationInterpolation(AnimationParseContext& context,
                                 components::AnimationInterpolation& options,
                                 const XMLQualifiedNameRef& name, std::string_view value) {
  if (name == XMLQualifiedNameRef("calcMode")) {
    ParseCalculationMode(options, value);
  } else if (name == XMLQualifiedNameRef("keyTimes") || name == XMLQualifiedNameRef("keySplines")) {
    ParseInterpolationList(context, options, name, value);
  } else if (name == XMLQualifiedNameRef("additive") || name == XMLQualifiedNameRef("accumulate")) {
    ParseAnimationComposition(options, name, value);
  } else {
    return false;
  }
  return true;
}

ParseResult<std::optional<components::ClockValue>> ParseEarliestOffset(std::string_view listValue) {
  std::optional<components::ClockValue> bestOffset;
  auto itemResult = parseSemicolonList(listValue);
  if (itemResult.hasError()) {
    return std::move(itemResult.error());
  }
  for (const std::string& item : itemResult.result()) {
    if (item == "indefinite") {
      continue;
    }
    auto result = ClockValueParser::Parse(item);
    if (result.hasResult()) {
      if (!bestOffset.has_value() || result.result().seconds() < bestOffset->seconds()) {
        bestOffset = result.result();
      }
    }
  }
  return bestOffset;
}

void ParseTimingOffset(AnimationParseContext& context, std::optional<std::string>& rawValue,
                       std::optional<components::ClockValue>& destination, std::string_view value) {
  rawValue = std::string(value);
  auto offset = ParseEarliestOffset(value);
  destination.reset();
  if (offset.hasResult()) {
    destination = std::move(offset.result());
  } else {
    context.diagnostic = std::move(offset.error());
  }
}

bool ParseTimingDuration(components::AnimationTimingComponent& timing,
                         const XMLQualifiedNameRef& name, std::string_view value) {
  std::optional<components::ClockValue>* destination;
  if (name == XMLQualifiedNameRef("dur")) {
    destination = &timing.dur;
  } else if (name == XMLQualifiedNameRef("repeatDur")) {
    destination = &timing.repeatDur;
  } else if (name == XMLQualifiedNameRef("min")) {
    destination = &timing.min;
  } else if (name == XMLQualifiedNameRef("max")) {
    destination = &timing.max;
  } else {
    return false;
  }
  auto result = ClockValueParser::Parse(value);
  if (result.hasResult() && (name != XMLQualifiedNameRef("max") || result.result().seconds() > 0)) {
    *destination = result.result();
  }
  return true;
}

void ParseRepeatCount(components::AnimationTimingComponent& timing, std::string_view value) {
  if (value == "indefinite") {
    timing.repeatCount = std::numeric_limits<double>::infinity();
  } else if (auto maybeNumber = ParseNumberNoSuffix(value); maybeNumber && *maybeNumber > 0) {
    timing.repeatCount = maybeNumber;
  }
}

void ParseRestartMode(components::AnimationTimingComponent& timing, std::string_view value) {
  if (value == "whenNotActive") {
    timing.restart = components::AnimationRestart::WhenNotActive;
  } else if (value == "never") {
    timing.restart = components::AnimationRestart::Never;
  } else {
    timing.restart = components::AnimationRestart::Always;
  }
}

/// Retain complete timing lists for interval resolution; earliest offsets support inspection.
void parseAnimationTimingAttribute(AnimationParseContext& context,
                                   components::AnimationTimingComponent& timing,
                                   const XMLQualifiedNameRef& name, std::string_view value) {
  if (ParseTimingDuration(timing, name, value)) {
    return;
  }
  if (name == XMLQualifiedNameRef("begin")) {
    ParseTimingOffset(context, timing.beginValue, timing.beginOffset, value);
  } else if (name == XMLQualifiedNameRef("end")) {
    ParseTimingOffset(context, timing.endValue, timing.endOffset, value);
  } else if (name == XMLQualifiedNameRef("fill")) {
    timing.fill =
        value == "freeze" ? components::AnimationFill::Freeze : components::AnimationFill::Remove;
  } else if (name == XMLQualifiedNameRef("repeatCount")) {
    ParseRepeatCount(timing, value);
  } else if (name == XMLQualifiedNameRef("restart")) {
    ParseRestartMode(timing, value);
  }
}

/// Returns true if the given name is one of the shared SMIL timing attributes.
bool IsAnimationTimingAttribute(const XMLQualifiedNameRef& name) {
  return name == XMLQualifiedNameRef("begin") || name == XMLQualifiedNameRef("dur") ||
         name == XMLQualifiedNameRef("end") || name == XMLQualifiedNameRef("fill") ||
         name == XMLQualifiedNameRef("repeatCount") || name == XMLQualifiedNameRef("repeatDur") ||
         name == XMLQualifiedNameRef("restart") || name == XMLQualifiedNameRef("min") ||
         name == XMLQualifiedNameRef("max");
}

template <typename Component>
bool ParseAnimationValues(EntityHandle handle, AnimationParseContext& context, Component& component,
                          const XMLQualifiedNameRef& name, std::string_view value) {
  if (name == XMLQualifiedNameRef("from")) {
    component.from = std::string(value);
  } else if (name == XMLQualifiedNameRef("to")) {
    component.to = std::string(value);
  } else if (name == XMLQualifiedNameRef("by")) {
    component.by = std::string(value);
  } else if (ParseAnimationHref(handle, component.href, name, value)) {
    return true;
  } else if (name == XMLQualifiedNameRef("values")) {
    auto parsed = parseSemicolonList(value);
    if (parsed.hasResult()) {
      component.values = std::move(parsed.result());
    } else {
      component.values.clear();
      component.valid = false;
      context.diagnostic = std::move(parsed.error());
    }
  } else {
    return ParseAnimationInterpolation(context, component, name, value);
  }
  return true;
}

bool ParseTransformType(components::AnimateTransformComponent& component, std::string_view value) {
  using Type = components::TransformAnimationType;
  if (value == "translate") {
    component.type = Type::Translate;
  } else if (value == "scale") {
    component.type = Type::Scale;
  } else if (value == "rotate") {
    component.type = Type::Rotate;
  } else if (value == "skewX") {
    component.type = Type::SkewX;
  } else if (value == "skewY") {
    component.type = Type::SkewY;
  } else {
    component.valid = false;
  }
  return true;
}

bool ParseMotionAttribute(EntityHandle handle, AnimationParseContext& context,
                          components::AnimateMotionComponent& motion,
                          const XMLQualifiedNameRef& name, std::string_view value) {
  if (name == XMLQualifiedNameRef("path")) {
    motion.path = std::string(value);
  } else if (name == XMLQualifiedNameRef("rotate")) {
    motion.rotate = std::string(value);
  } else if (name == XMLQualifiedNameRef("keyPoints")) {
    auto parsed = parseDoubleList(value);
    if (parsed.hasResult() && !parsed.result().empty()) {
      motion.keyPoints = std::move(parsed.result());
    } else {
      motion.valid = false;
      if (parsed.hasError()) {
        context.diagnostic = std::move(parsed.error());
      }
    }
  } else {
    return ParseAnimationValues(handle, context, motion, name, value);
  }
  return true;
}

bool ParseSpecificAnimationAttribute(EntityHandle handle, AnimationParseContext& context,
                                     const XMLQualifiedNameRef& name, std::string_view value) {
  if (auto* component = handle.try_get<components::AnimateValueComponent>()) {
    if (name == XMLQualifiedNameRef("attributeName")) {
      component->attributeName = std::string(value);
      return true;
    }
    return ParseAnimationValues(handle, context, *component, name, value);
  }
  if (auto* component = handle.try_get<components::AnimateTransformComponent>()) {
    if (name == XMLQualifiedNameRef("attributeName")) {
      component->attributeName = std::string(value);
      return true;
    }
    return name == XMLQualifiedNameRef("type")
               ? ParseTransformType(*component, value)
               : ParseAnimationValues(handle, context, *component, name, value);
  }
  if (auto* component = handle.try_get<components::AnimateMotionComponent>()) {
    return ParseMotionAttribute(handle, context, *component, name, value);
  }
  if (auto* component = handle.try_get<components::SetAnimationComponent>()) {
    if (name == XMLQualifiedNameRef("attributeName")) {
      component->attributeName = std::string(value);
    } else if (name == XMLQualifiedNameRef("to")) {
      component->to = std::string(value);
    } else if (ParseAnimationHref(handle, component->href, name, value)) {
      return true;
    } else {
      return false;
    }
    return true;
  }
  if (auto* component = handle.try_get<components::MPathComponent>()) {
    if (!ParseAnimationHref(handle, component->href, name, value)) {
      return false;
    }
    return true;
  }
  return false;
}

struct AnimationAttributeRevision {
  uint64_t hash = 0;
  bool initialized = false;
};

template <typename Component>
void ResetAnimationComponent(EntityHandle handle) {
  if (auto* component = handle.try_get<Component>()) {
    *component = Component{};
  }
}

}  // namespace

ParseResult<bool> ParseAnimationAttribute(EntityHandle handle, const XMLQualifiedNameRef& name,
                                          std::string_view value) {
  AnimationParseContext context;
  bool recognized = false;
  if (auto* timing = handle.try_get<components::AnimationTimingComponent>();
      timing && IsAnimationTimingAttribute(name)) {
    parseAnimationTimingAttribute(context, *timing, name, value);
    recognized = true;
  } else {
    recognized = ParseSpecificAnimationAttribute(handle, context, name, value);
  }
  if (context.diagnostic) {
    return std::move(*context.diagnostic);
  }
  return recognized;
}

bool RefreshAnimationAttributes(Registry& registry, std::span<const Entity> animations) {
  std::size_t total = 0;
  for (Entity entity : animations) {
    const auto* attributes = registry.try_get<donner::components::AttributesComponent>(entity);
    if (!attributes) {
      continue;
    }
    uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&](std::string_view value) {
      for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
      }
      hash ^= 255;
      hash *= 1099511628211ULL;
    };
    const auto names = attributes->attributes();
    for (const auto& name : names) {
      const auto value = attributes->getAttribute(name);
      const std::size_t bytes =
          name.name.size() + name.namespacePrefix.size() + (value ? value->size() : 0);
      if (bytes > components::AnimationResourceBudget::kMaximumSourceBytes - total) {
        return false;
      }
      total += bytes;
      mix(name.namespacePrefix);
      mix(name.name);
      if (value) {
        mix(std::string_view(*value));
      }
    }
    auto& revision = registry.get_or_emplace<AnimationAttributeRevision>(entity);
    if (revision.initialized && revision.hash == hash) {
      continue;
    }
    revision = {hash, true};
    EntityHandle handle(registry, entity);
    ResetAnimationComponent<components::AnimationTimingComponent>(handle);
    ResetAnimationComponent<components::AnimateValueComponent>(handle);
    ResetAnimationComponent<components::AnimateTransformComponent>(handle);
    ResetAnimationComponent<components::AnimateMotionComponent>(handle);
    ResetAnimationComponent<components::SetAnimationComponent>(handle);
    for (const auto& name : names) {
      if (const auto value = attributes->getAttribute(name)) {
        (void)ParseAnimationAttribute(handle, name, std::string_view(*value));
      }
    }
  }
  return true;
}

}  // namespace donner::svg::parser
