#include "donner/svg/renderer/AnimationAttributes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>

#include "donner/base/StringUtils.h"
#include "donner/base/parser/LengthParser.h"
#include "donner/base/parser/NumberParser.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/css/parser/ValueParser.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/PathLengthComponent.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationNumericAttributes.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationValueMath.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SymbolComponent.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/paint/ClipPathComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/paint/StopComponent.h"
#include "donner/svg/components/text/TextComponent.h"
#include "donner/svg/components/text/TextInvalidation.h"
#include "donner/svg/components/text/TextPathComponent.h"
#include "donner/svg/components/text/TextPositioningComponent.h"
#include "donner/svg/parser/AngleParser.h"
#include "donner/svg/parser/Number2dParser.h"
#include "donner/svg/parser/PathParser.h"
#include "donner/svg/parser/PreserveAspectRatioParser.h"
#include "donner/svg/parser/ViewBoxParser.h"

namespace donner::svg::components {
namespace {

struct AttributeRestoreContext {
  AnimationResourceBudget budget;
};

template <typename Component>
Component InitialComponent(const Component& current) {
  if constexpr (std::is_same_v<Component, FEFuncComponent>) {
    return Component(current.channel);
  } else if constexpr (std::is_same_v<Component, LightSourceComponent>) {
    return Component(current.type);
  } else {
    return Component{};
  }
}

struct AttributeRestore {
  std::optional<RcString> source;
  std::function<void(Registry&, Entity)> restore;
  std::function<void(Registry&, Entity)> reset;
};
struct AnimationAttributeBackup {
  std::map<std::string, AttributeRestore> fields;
};

std::optional<RcString> SourceAttribute(Registry& registry, Entity entity, std::string_view name) {
  const auto* attributes = registry.try_get<donner::components::AttributesComponent>(entity);
  return attributes ? attributes->getAttribute(xml::XMLQualifiedNameRef(name)) : std::nullopt;
}

template <typename Value>
std::size_t RetainedValueBytes(const Value& value) {
  if constexpr (std::is_same_v<Value, RcString>) {
    return sizeof(Value) + value.size();
  } else if constexpr (std::is_same_v<Value, FilterInput>) {
    const auto* named = std::get_if<FilterInput::Named>(&value.value);
    return sizeof(Value) + (named ? named->name.size() : 0);
  } else if constexpr (requires { value.retainedBytes(); }) {
    return value.retainedBytes().value_or(AnimationResourceBudget::kMaximumOutputBytes + 1);
  } else if constexpr (requires {
                         value.has_value();
                         *value;
                       }) {
    return sizeof(Value) + (value ? RetainedValueBytes(*value) : 0);
  } else if constexpr (requires {
                         value.capacity();
                         typename Value::value_type;
                       }) {
    return sizeof(Value) + value.capacity() * sizeof(typename Value::value_type);
  } else {
    return sizeof(Value);
  }
}

template <typename Value>
bool PresentationValueMatches(const Value& current, const Value& applied) {
  return current == applied;
}

template <typename Value, std::size_t Size>
bool PresentationValueMatches(const SmallVector<Value, Size>& current,
                              const SmallVector<Value, Size>& applied) {
  return std::equal(current.begin(), current.end(), applied.begin(), applied.end());
}

template <typename Value, PropertyCascade Cascade>
bool PresentationValueMatches(const Property<Value, Cascade>& current,
                              const Property<Value, Cascade>& applied) {
  return current.state == applied.state && current.specificity == applied.specificity &&
         current.get() == applied.get();
}

template <typename Component, typename Value, typename Read, typename Write>
bool OverrideField(Registry& registry, Entity entity, std::string_view name, const Value& value,
                   Read read, Write write, bool create = false) {
  auto* component = registry.try_get<Component>(entity);
  const bool created = component == nullptr;
  if (!component) {
    if constexpr (std::is_default_constructible_v<Component>) {
      if (!create) {
        return false;
      }
      component = &registry.emplace<Component>(entity);
    } else {
      return false;
    }
  }
  if (registry.ctx().contains<AttributeRestoreContext>()) {
    write(*component, value);
    return true;
  }
  auto& backup = registry.get_or_emplace<AnimationAttributeBackup>(entity);
  if (!backup.fields.contains(std::string(name))) {
    auto& budget = registry.ctx().get<AnimationResourceBudget>();
    const auto& original = read(*component);
    if (!budget.reserveOutput(RetainedValueBytes(original) + RetainedValueBytes(value))) {
      if (created) {
        registry.remove<Component>(entity);
      }
      return true;
    }
    backup.fields.emplace(
        name, AttributeRestore{
                  SourceAttribute(registry, entity, name),
                  [original, value, read, write, created](Registry& destination, Entity target) {
                    if (auto* component = destination.try_get<Component>(target);
                        component && PresentationValueMatches(read(*component), value)) {
                      if (created) {
                        destination.remove<Component>(target);
                      } else {
                        write(*component, original);
                      }
                    }
                  },
                  [read, write, created](Registry& destination, Entity target) {
                    if (auto* component = destination.try_get<Component>(target)) {
                      if (created) {
                        destination.remove<Component>(target);
                      } else {
                        write(*component, read(InitialComponent(*component)));
                      }
                    }
                  }});
  }
  write(*component, value);
  return true;
}

template <typename Component, typename Field, typename Value>
bool SimpleField(Registry& registry, Entity entity, std::string_view name, Field Component::* field,
                 const Value& value, bool create = false) {
  return OverrideField<Component>(
      registry, entity, name, Field(value),
      [field](const Component& component) -> const Field& { return component.*field; },
      [field](Component& component, const Field& value) { component.*field = value; }, create);
}

std::optional<double> Number(std::string_view value) {
  const auto result = donner::parser::NumberParser::Parse(value);
  if (result.hasError() || result.result().consumedChars != value.size() ||
      !std::isfinite(result.result().number)) {
    return std::nullopt;
  }
  return result.result().number;
}

template <typename Component, typename Field, typename Value, std::size_t N>
bool ApplyNamedField(Registry& registry, Entity entity, std::string_view name,
                     Field Component::* field, std::string_view value,
                     const std::pair<std::string_view, Value> (&values)[N]) {
  for (const auto& [candidate, parsed] : values) {
    if (candidate == value) {
      return SimpleField(registry, entity, name, field, parsed);
    }
  }
  return true;
}

bool ApplyLinearGradientLength(Registry& registry, Entity entity, std::string_view name,
                               Lengthd length) {
  using Field = std::optional<Lengthd> LinearGradientComponent::*;
  static constexpr std::pair<std::string_view, Field> fields[] = {
      {"x1", &LinearGradientComponent::x1},
      {"y1", &LinearGradientComponent::y1},
      {"x2", &LinearGradientComponent::x2},
      {"y2", &LinearGradientComponent::y2},
  };
  for (const auto& [fieldName, field] : fields) {
    if (fieldName == name && SimpleField(registry, entity, name, field, length)) {
      return true;
    }
  }
  return false;
}

bool ApplyRadialGradientLength(Registry& registry, Entity entity, std::string_view name,
                               Lengthd length) {
  using Field = std::optional<Lengthd> RadialGradientComponent::*;
  static constexpr std::pair<std::string_view, Field> fields[] = {
      {"cx", &RadialGradientComponent::cx}, {"cy", &RadialGradientComponent::cy},
      {"r", &RadialGradientComponent::r},   {"fx", &RadialGradientComponent::fx},
      {"fy", &RadialGradientComponent::fy}, {"fr", &RadialGradientComponent::fr},
  };
  for (const auto& [fieldName, field] : fields) {
    if (fieldName == name && SimpleField(registry, entity, name, field, length)) {
      return true;
    }
  }
  return false;
}

bool ApplyGradientLength(Registry& registry, Entity entity, std::string_view name,
                         std::string_view value) {
  donner::parser::LengthParser::Options options;
  options.unitOptional = true;
  const auto parsed = donner::parser::LengthParser::Parse(value, options);
  if (parsed.hasError() || parsed.result().consumedChars != value.size()) {
    return false;
  }
  const auto length = parsed.result().length;
  return ApplyLinearGradientLength(registry, entity, name, length) ||
         ApplyRadialGradientLength(registry, entity, name, length);
}

bool ApplyFilterNumber(Registry& registry, Entity entity, std::string_view name, double value) {
  return VisitAnimationNumberAttribute(registry, entity, name, [&](auto&, auto field) {
    (void)SimpleField(registry, entity, name, field, value);
  });
}

template <typename Component>
bool ApplyDeviation(Registry& registry, Entity entity, std::string_view name, Vector2d value) {
  return OverrideField<Component>(
      registry, entity, name, value,
      [](const Component& component) {
        return Vector2d(component.stdDeviationX, component.stdDeviationY);
      },
      [](Component& component, Vector2d value) {
        component.stdDeviationX = value.x;
        component.stdDeviationY = value.y;
      });
}

std::optional<std::vector<double>> AnimationNumbers(std::string_view value) {
  std::vector<double> result;
  value = StringUtils::TrimWhitespace(value);
  while (!value.empty()) {
    const auto parsed = donner::parser::NumberParser::Parse(value);
    if (parsed.hasError() || !std::isfinite(parsed.result().number) ||
        result.size() >= AnimationResourceBudget::kMaximumNumbersPerValue) {
      return std::nullopt;
    }
    result.push_back(parsed.result().number);
    value.remove_prefix(parsed.result().consumedChars);
    value = StringUtils::TrimWhitespace(value);
    if (!value.empty() && value.front() == ',') {
      value = StringUtils::TrimWhitespace(value.substr(1));
    }
  }
  return result.empty() ? std::nullopt : std::optional(std::move(result));
}

bool ApplyFilterList(Registry& registry, Entity entity, std::string_view name,
                     std::string_view value) {
  if (name != "values" && name != "kernelMatrix" && name != "tableValues") {
    return false;
  }
  const auto numbers = AnimationNumbers(value);
  if (!numbers) {
    return true;
  }
  if (name == "values") {
    (void)SimpleField(registry, entity, name, &FEColorMatrixComponent::values, *numbers);
  }
  if (name == "kernelMatrix") {
    (void)SimpleField(registry, entity, name, &FEConvolveMatrixComponent::kernelMatrix, *numbers);
  }
  if (name == "tableValues") {
    (void)SimpleField(registry, entity, name, &FEFuncComponent::tableValues, *numbers);
  }
  return true;
}

bool ApplyFilterPair(Registry& registry, Entity entity, std::string_view name,
                     std::string_view value) {
  if (name != "baseFrequency" && name != "radius") {
    return false;
  }
  const auto parsed = parser::Number2dParser::Parse(value);
  if (parsed.hasError() || parsed.result().consumedChars != value.size()) {
    return true;
  }
  const Vector2d pair(parsed.result().numberX, parsed.result().numberY);
  if (!std::isfinite(pair.x) || !std::isfinite(pair.y) || pair.x < 0 || pair.y < 0) {
    return true;
  }
  if (name == "baseFrequency") {
    (void)OverrideField<FETurbulenceComponent>(
        registry, entity, name, pair,
        [](const FETurbulenceComponent& c) { return Vector2d(c.baseFrequencyX, c.baseFrequencyY); },
        [](FETurbulenceComponent& c, Vector2d v) {
          c.baseFrequencyX = v.x;
          c.baseFrequencyY = v.y;
        });
  } else {
    (void)OverrideField<FEMorphologyComponent>(
        registry, entity, name, pair,
        [](const FEMorphologyComponent& c) { return Vector2d(c.radiusX, c.radiusY); },
        [](FEMorphologyComponent& c, Vector2d v) {
          c.radiusX = v.x;
          c.radiusY = v.y;
        });
  }
  return true;
}

/// An feConvolveMatrix order together with its over-limit flag, overridden as one value.
struct ConvolveOrder {
  Vector2i order;             //!< Order on each axis.
  bool exceedsLimit = false;  //!< Whether the authored order exceeded the limit.

  bool operator==(const ConvolveOrder&) const = default;
};

std::optional<int> AnimationInteger(double value, int minimum, int maximum) {
  if (!std::isfinite(value) || value < minimum || value > maximum) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

bool ApplyFilterInteger(Registry& registry, Entity entity, std::string_view name,
                        std::string_view value) {
  if (name != "order" && name != "numOctaves" && name != "targetX" && name != "targetY") {
    return false;
  }
  const auto numbers = AnimationNumbers(value);
  if (!numbers) {
    return true;
  }
  if (name == "order") {
    if (numbers->size() > 2) {
      return true;
    }
    const auto x = AnimationInteger(numbers->front(), 1, kMaximumFilterConvolveOrder);
    const auto y = AnimationInteger(numbers->back(), 1, kMaximumFilterConvolveOrder);
    if (x && y) {
      // The over-limit flag belongs to the order, so it is overridden and restored with it.
      (void)OverrideField<FEConvolveMatrixComponent>(
          registry, entity, name, ConvolveOrder{Vector2i(*x, *y), false},
          [](const auto& component) {
            return ConvolveOrder{Vector2i(component.orderX, component.orderY),
                                 component.orderExceedsLimit};
          },
          [](auto& component, const ConvolveOrder& value) {
            component.orderX = value.order.x;
            component.orderY = value.order.y;
            component.orderExceedsLimit = value.exceedsLimit;
          });
    }
    return true;
  }
  if (numbers->size() != 1) {
    return true;
  }
  const auto number = AnimationInteger(numbers->front(), std::numeric_limits<int>::min(),
                                       std::numeric_limits<int>::max());
  if (!number) {
    return true;
  }
  if (name == "numOctaves") {
    return SimpleField(registry, entity, name, &FETurbulenceComponent::numOctaves, *number);
  }
  const auto field =
      name == "targetX" ? &FEConvolveMatrixComponent::targetX : &FEConvolveMatrixComponent::targetY;
  return SimpleField(registry, entity, name, field, *number);
}

bool ApplyFilterBoolean(Registry& registry, Entity entity, std::string_view name,
                        std::string_view value) {
  if (name == "preserveAlpha" && (value == "true" || value == "false")) {
    return SimpleField(registry, entity, name, &FEConvolveMatrixComponent::preserveAlpha,
                       value == "true");
  }
  if (name == "stitchTiles" && (value == "stitch" || value == "noStitch")) {
    return SimpleField(registry, entity, name, &FETurbulenceComponent::stitchTiles,
                       value == "stitch");
  }
  return false;
}

bool ApplyFilterEdgeMode(Registry& registry, Entity entity, std::string_view name,
                         std::string_view value) {
  if (name != "edgeMode") {
    return false;
  }
  using Convolve = FEConvolveMatrixComponent::EdgeMode;
  using Blur = FEGaussianBlurComponent::EdgeMode;
  if (value == "none") {
    return SimpleField(registry, entity, name, &FEConvolveMatrixComponent::edgeMode,
                       Convolve::None) ||
           SimpleField(registry, entity, name, &FEGaussianBlurComponent::edgeMode, Blur::None);
  }
  if (value == "duplicate") {
    return SimpleField(registry, entity, name, &FEConvolveMatrixComponent::edgeMode,
                       Convolve::Duplicate) ||
           SimpleField(registry, entity, name, &FEGaussianBlurComponent::edgeMode, Blur::Duplicate);
  }
  if (value == "wrap") {
    return SimpleField(registry, entity, name, &FEConvolveMatrixComponent::edgeMode,
                       Convolve::Wrap) ||
           SimpleField(registry, entity, name, &FEGaussianBlurComponent::edgeMode, Blur::Wrap);
  }
  return true;
}

bool ApplyFilterChannel(Registry& registry, Entity entity, std::string_view name,
                        std::string_view value) {
  if (name != "xChannelSelector" && name != "yChannelSelector") {
    return false;
  }
  using Channel = FEDisplacementMapComponent::Channel;
  const auto field = name == "xChannelSelector" ? &FEDisplacementMapComponent::xChannelSelector
                                                : &FEDisplacementMapComponent::yChannelSelector;
  if (value == "R") {
    return SimpleField(registry, entity, name, field, Channel::R);
  }
  if (value == "G") {
    return SimpleField(registry, entity, name, field, Channel::G);
  }
  if (value == "B") {
    return SimpleField(registry, entity, name, field, Channel::B);
  }
  if (value == "A") {
    return SimpleField(registry, entity, name, field, Channel::A);
  }
  return true;
}

bool ApplyFilterReference(Registry& registry, Entity entity, std::string_view name,
                          std::string_view value) {
  if (name != "in" && name != "in2" && name != "result") {
    return false;
  }
  if (!registry.any_of<FilterPrimitiveComponent, FEMergeNodeComponent>(entity)) {
    return false;
  }
  auto* restoration = registry.ctx().find<AttributeRestoreContext>();
  auto& budget = restoration ? restoration->budget : registry.ctx().get<AnimationResourceBudget>();
  if (!budget.reserveOutput(value.size())) {
    return true;
  }
  if (name == "result") {
    return SimpleField(registry, entity, name, &FilterPrimitiveComponent::result, RcString(value));
  }
  const auto input = FilterInput::FromAttribute(value);
  if (name == "in2") {
    return SimpleField(registry, entity, name, &FilterPrimitiveComponent::in2, input);
  }
  return SimpleField(registry, entity, name, &FilterPrimitiveComponent::in, input) ||
         SimpleField(registry, entity, name, &FEMergeNodeComponent::in, input);
}

bool ApplyFilterControl(Registry& registry, Entity entity, std::string_view name,
                        std::string_view value) {
  return ApplyFilterInteger(registry, entity, name, value) ||
         ApplyFilterBoolean(registry, entity, name, value) ||
         ApplyFilterEdgeMode(registry, entity, name, value) ||
         ApplyFilterChannel(registry, entity, name, value) ||
         ApplyFilterReference(registry, entity, name, value);
}

bool ApplyCompositeOperator(Registry& registry, Entity entity, std::string_view name,
                            std::string_view value) {
  if (!registry.all_of<FECompositeComponent>(entity)) {
    return false;
  }
  using Operator = FECompositeComponent::Operator;
  static constexpr std::pair<std::string_view, Operator> values[] = {
      {"over", Operator::Over},
      {"in", Operator::In},
      {"out", Operator::Out},
      {"atop", Operator::Atop},
      {"xor", Operator::Xor},
      {"lighter", Operator::Lighter},
      {"arithmetic", Operator::Arithmetic},
  };
  return ApplyNamedField(registry, entity, name, &FECompositeComponent::op, value, values);
}

bool ApplyMorphologyOperator(Registry& registry, Entity entity, std::string_view name,
                             std::string_view value) {
  if (!registry.all_of<FEMorphologyComponent>(entity)) {
    return false;
  }
  using Operator = FEMorphologyComponent::Operator;
  static constexpr std::pair<std::string_view, Operator> values[] = {
      {"erode", Operator::Erode},
      {"dilate", Operator::Dilate},
  };
  return ApplyNamedField(registry, entity, name, &FEMorphologyComponent::op, value, values);
}

bool ApplyFilterOperator(Registry& registry, Entity entity, std::string_view name,
                         std::string_view value) {
  if (registry.all_of<FECompositeComponent>(entity)) {
    return ApplyCompositeOperator(registry, entity, name, value);
  }
  return ApplyMorphologyOperator(registry, entity, name, value);
}

bool ApplyColorMatrixType(Registry& registry, Entity entity, std::string_view name,
                          std::string_view value) {
  if (!registry.all_of<FEColorMatrixComponent>(entity)) {
    return false;
  }
  using Type = FEColorMatrixComponent::Type;
  static constexpr std::pair<std::string_view, Type> values[] = {
      {"matrix", Type::Matrix},
      {"saturate", Type::Saturate},
      {"hueRotate", Type::HueRotate},
      {"luminanceToAlpha", Type::LuminanceToAlpha},
  };
  return ApplyNamedField(registry, entity, name, &FEColorMatrixComponent::type, value, values);
}

bool ApplyFunctionType(Registry& registry, Entity entity, std::string_view name,
                       std::string_view value) {
  if (!registry.all_of<FEFuncComponent>(entity)) {
    return false;
  }
  using Type = FEFuncComponent::FuncType;
  static constexpr std::pair<std::string_view, Type> values[] = {
      {"identity", Type::Identity}, {"table", Type::Table}, {"discrete", Type::Discrete},
      {"linear", Type::Linear},     {"gamma", Type::Gamma},
  };
  return ApplyNamedField(registry, entity, name, &FEFuncComponent::type, value, values);
}

bool ApplyTurbulenceType(Registry& registry, Entity entity, std::string_view name,
                         std::string_view value) {
  if (!registry.all_of<FETurbulenceComponent>(entity)) {
    return false;
  }
  using Type = FETurbulenceComponent::Type;
  static constexpr std::pair<std::string_view, Type> values[] = {
      {"fractalNoise", Type::FractalNoise},
      {"turbulence", Type::Turbulence},
  };
  return ApplyNamedField(registry, entity, name, &FETurbulenceComponent::type, value, values);
}

bool ApplyFilterType(Registry& registry, Entity entity, std::string_view name,
                     std::string_view value) {
  if (registry.all_of<FEColorMatrixComponent>(entity)) {
    return ApplyColorMatrixType(registry, entity, name, value);
  }
  if (registry.all_of<FEFuncComponent>(entity)) {
    return ApplyFunctionType(registry, entity, name, value);
  }
  return ApplyTurbulenceType(registry, entity, name, value);
}

bool ApplyGradientUnits(Registry& registry, Entity entity, std::string_view name,
                        std::string_view value) {
  if (!registry.all_of<GradientComponent>(entity)) {
    return false;
  }
  static constexpr std::pair<std::string_view, GradientUnits> values[] = {
      {"userSpaceOnUse", GradientUnits::UserSpaceOnUse},
      {"objectBoundingBox", GradientUnits::ObjectBoundingBox},
  };
  return ApplyNamedField(registry, entity, name, &GradientComponent::gradientUnits, value, values);
}

bool ApplyGradientSpreadMethod(Registry& registry, Entity entity, std::string_view name,
                               std::string_view value) {
  if (!registry.all_of<GradientComponent>(entity)) {
    return false;
  }
  static constexpr std::pair<std::string_view, GradientSpreadMethod> values[] = {
      {"pad", GradientSpreadMethod::Pad},
      {"reflect", GradientSpreadMethod::Reflect},
      {"repeat", GradientSpreadMethod::Repeat},
  };
  return ApplyNamedField(registry, entity, name, &GradientComponent::spreadMethod, value, values);
}

bool ApplyBlendMode(Registry& registry, Entity entity, std::string_view name,
                    std::string_view value) {
  if (!registry.all_of<FEBlendComponent>(entity)) {
    return false;
  }
  using Mode = FEBlendComponent::Mode;
  static constexpr std::pair<std::string_view, Mode> values[] = {
      {"normal", Mode::Normal},
      {"multiply", Mode::Multiply},
      {"screen", Mode::Screen},
      {"darken", Mode::Darken},
      {"lighten", Mode::Lighten},
      {"overlay", Mode::Overlay},
      {"color-dodge", Mode::ColorDodge},
      {"color-burn", Mode::ColorBurn},
      {"hard-light", Mode::HardLight},
      {"soft-light", Mode::SoftLight},
      {"difference", Mode::Difference},
      {"exclusion", Mode::Exclusion},
      {"hue", Mode::Hue},
      {"saturation", Mode::Saturation},
      {"color", Mode::Color},
      {"luminosity", Mode::Luminosity},
  };
  return ApplyNamedField(registry, entity, name, &FEBlendComponent::mode, value, values);
}

bool ApplyDiscreteAttribute(Registry& registry, Entity entity, std::string_view name,
                            std::string_view value) {
  if (name == "operator") {
    return ApplyFilterOperator(registry, entity, name, value);
  }
  if (name == "type") {
    return ApplyFilterType(registry, entity, name, value);
  }
  if (name == "gradientUnits") {
    return ApplyGradientUnits(registry, entity, name, value);
  }
  if (name == "spreadMethod") {
    return ApplyGradientSpreadMethod(registry, entity, name, value);
  }
  if (name == "mode") {
    return ApplyBlendMode(registry, entity, name, value);
  }
  return false;
}

template <typename Component>
bool ApplyRegionLength(Registry& registry, Entity entity, std::string_view name, Lengthd value) {
  if (name == "x") {
    return SimpleField(registry, entity, name, &Component::x, value);
  }
  if (name == "y") {
    return SimpleField(registry, entity, name, &Component::y, value);
  }
  if (name == "width") {
    return SimpleField(registry, entity, name, &Component::width, value);
  }
  if (name == "height") {
    return SimpleField(registry, entity, name, &Component::height, value);
  }
  return false;
}

bool ApplyPatternLength(Registry& registry, Entity entity, std::string_view name, Lengthd value) {
  auto* pattern = registry.try_get<PatternComponent>(entity);
  if (!pattern) {
    return false;
  }
  using Field = Property<Lengthd> SizedElementProperties::*;
  static constexpr std::pair<std::string_view, Field> fields[] = {
      {"x", &SizedElementProperties::x},
      {"y", &SizedElementProperties::y},
      {"width", &SizedElementProperties::width},
      {"height", &SizedElementProperties::height}};
  for (const auto& [fieldName, field] : fields) {
    if (fieldName != name) {
      continue;
    }
    auto animated = pattern->sizeProperties.*field;
    animated.substitute(value);
    animated.specificity = registry.ctx().contains<AttributeRestoreContext>()
                               ? css::Specificity()
                               : css::Specificity::Animation();
    return OverrideField<PatternComponent>(
        registry, entity, name, animated,
        [field](const PatternComponent& component) -> const auto& {
          return component.sizeProperties.*field;
        },
        [field](PatternComponent& component, const auto& value) {
          component.sizeProperties.*field = value;
        });
  }
  return false;
}

bool ApplyLayoutLength(Registry& registry, Entity entity, std::string_view name,
                       std::string_view value) {
  static constexpr std::array<std::string_view, 5> attributes = {"x", "y", "width", "height",
                                                                 "textLength"};
  if (std::find(attributes.begin(), attributes.end(), name) == attributes.end()) {
    return false;
  }
  const auto lengths = ParseAnimationLengths(value);
  if (!lengths || lengths->size() != 1) {
    return false;
  }
  const Lengthd length = lengths->front();
  if (name == "textLength") {
    return SimpleField(registry, entity, name, &TextComponent::textLength, length);
  }
  return ApplyRegionLength<MaskComponent>(registry, entity, name, length) ||
         ApplyRegionLength<FilterComponent>(registry, entity, name, length) ||
         ApplyRegionLength<FilterPrimitiveComponent>(registry, entity, name, length) ||
         ApplyPatternLength(registry, entity, name, length);
}

bool ApplyTextPosition(Registry& registry, Entity entity, std::string_view name,
                       std::string_view value) {
  if (!registry.all_of<TextPositioningComponent>(entity)) {
    return false;
  }
  if (name == "rotate") {
    const auto numbers = AnimationNumbers(value);
    if (numbers) {
      SmallVector<double, 1> list;
      for (double number : *numbers) {
        list.push_back(number);
      }
      (void)SimpleField(registry, entity, name, &TextPositioningComponent::rotateDegrees, list);
    }
    return true;
  }
  using Field = SmallVector<Lengthd, 1> TextPositioningComponent::*;
  static constexpr std::pair<std::string_view, Field> fields[] = {
      {"x", &TextPositioningComponent::x},
      {"y", &TextPositioningComponent::y},
      {"dx", &TextPositioningComponent::dx},
      {"dy", &TextPositioningComponent::dy}};
  for (const auto& [fieldName, field] : fields) {
    if (fieldName != name) {
      continue;
    }
    const auto lengths = ParseAnimationLengths(value);
    if (lengths) {
      SmallVector<Lengthd, 1> list;
      for (Lengthd length : *lengths) {
        list.push_back(length);
      }
      (void)SimpleField(registry, entity, name, field, list);
    }
    return true;
  }
  return false;
}

bool ApplyLayoutEnumeration(Registry& registry, Entity entity, std::string_view name,
                            std::string_view value) {
  if (name == "lengthAdjust") {
    if (value == "spacing") {
      return SimpleField(registry, entity, name, &TextComponent::lengthAdjust,
                         LengthAdjust::Spacing);
    }
    if (value == "spacingAndGlyphs") {
      return SimpleField(registry, entity, name, &TextComponent::lengthAdjust,
                         LengthAdjust::SpacingAndGlyphs);
    }
    return true;
  }
  if (value != "userSpaceOnUse" && value != "objectBoundingBox") {
    return false;
  }
  const bool user = value == "userSpaceOnUse";
  if (name == "patternUnits") {
    return SimpleField(registry, entity, name, &PatternComponent::patternUnits,
                       user ? PatternUnits::UserSpaceOnUse : PatternUnits::ObjectBoundingBox);
  }
  if (name == "patternContentUnits") {
    return SimpleField(
        registry, entity, name, &PatternComponent::patternContentUnits,
        user ? PatternContentUnits::UserSpaceOnUse : PatternContentUnits::ObjectBoundingBox);
  }
  if (name == "filterUnits") {
    return SimpleField(registry, entity, name, &FilterComponent::filterUnits,
                       user ? FilterUnits::UserSpaceOnUse : FilterUnits::ObjectBoundingBox);
  }
  if (name == "primitiveUnits") {
    return SimpleField(registry, entity, name, &FilterComponent::primitiveUnits,
                       user ? PrimitiveUnits::UserSpaceOnUse : PrimitiveUnits::ObjectBoundingBox);
  }
  return false;
}

template <typename Component>
bool ApplyReferenceLength(
    Registry& registry, Entity entity, std::string_view name, std::string_view value,
    std::initializer_list<std::pair<std::string_view, Lengthd Component::*>> fields) {
  if (!registry.all_of<Component>(entity)) {
    return false;
  }
  for (const auto& [fieldName, field] : fields) {
    if (fieldName != name) {
      continue;
    }
    if (const auto length = ParseAnimationLength(name, value)) {
      (void)SimpleField(registry, entity, name, field, *length);
    }
    return true;
  }
  return false;
}

bool ApplyViewportReferenceLength(Registry& registry, Entity entity, std::string_view name,
                                  std::string_view value) {
  return ApplyReferenceLength<MarkerComponent>(registry, entity, name, value,
                                               {{"markerWidth", &MarkerComponent::markerWidth},
                                                {"markerHeight", &MarkerComponent::markerHeight},
                                                {"refX", &MarkerComponent::refX},
                                                {"refY", &MarkerComponent::refY}}) ||
         ApplyReferenceLength<SymbolComponent>(
             registry, entity, name, value,
             {{"refX", &SymbolComponent::refX}, {"refY", &SymbolComponent::refY}});
}

bool ApplyMarkerOrientation(Registry& registry, Entity entity, std::string_view name,
                            std::string_view value) {
  if (name == "markerUnits") {
    if (value == "strokeWidth" || value == "userSpaceOnUse") {
      return SimpleField(
          registry, entity, name, &MarkerComponent::markerUnits,
          value == "strokeWidth" ? MarkerUnits::StrokeWidth : MarkerUnits::UserSpaceOnUse);
    }
    return false;
  }
  if (name != "orient" || !registry.all_of<MarkerComponent>(entity)) {
    return false;
  }
  if (value == "auto" || value == "auto-start-reverse") {
    return SimpleField(registry, entity, name, &MarkerComponent::orient,
                       value == "auto" ? MarkerOrient::Auto() : MarkerOrient::AutoStartReverse());
  }
  auto tokens = css::parser::ValueParser::Parse(value);
  if (tokens.size() == 1) {
    auto angle =
        parser::ParseAngle(tokens.front(), parser::AngleParseOptions::AllowNumbersInDegrees);
    if (angle.hasResult() && std::isfinite(angle.result())) {
      return SimpleField(registry, entity, name, &MarkerComponent::orient,
                         MarkerOrient::AngleRadians(angle.result()));
    }
  }
  return true;
}

bool ApplyMaskOrClipUnits(Registry& registry, Entity entity, std::string_view name,
                          std::string_view value) {
  if (value != "userSpaceOnUse" && value != "objectBoundingBox") {
    return false;
  }
  const bool user = value == "userSpaceOnUse";
  if (name == "maskUnits") {
    return SimpleField(registry, entity, name, &MaskComponent::maskUnits,
                       user ? MaskUnits::UserSpaceOnUse : MaskUnits::ObjectBoundingBox);
  }
  if (name == "maskContentUnits") {
    return SimpleField(
        registry, entity, name, &MaskComponent::maskContentUnits,
        user ? MaskContentUnits::UserSpaceOnUse : MaskContentUnits::ObjectBoundingBox);
  }
  if (name == "clipPathUnits") {
    return SimpleField(registry, entity, name, &ClipPathComponent::clipPathUnits,
                       user ? ClipPathUnits::UserSpaceOnUse : ClipPathUnits::ObjectBoundingBox);
  }
  return false;
}

bool ApplyTextPathStartOffset(Registry& registry, Entity entity, std::string_view name,
                              std::string_view value) {
  const auto lengths = ParseAnimationLengths(value);
  if (!lengths || lengths->size() != 1) {
    return false;
  }
  return SimpleField(registry, entity, name, &TextPathComponent::startOffset, lengths->front());
}

bool ApplyTextPathSide(Registry& registry, Entity entity, std::string_view name,
                       std::string_view value) {
  if (value != "left" && value != "right") {
    return false;
  }
  return SimpleField(registry, entity, name, &TextPathComponent::side,
                     value == "left" ? TextPathSide::Left : TextPathSide::Right);
}

bool ApplyTextPathMethod(Registry& registry, Entity entity, std::string_view name,
                         std::string_view value) {
  if (value != "align" && value != "stretch") {
    return false;
  }
  return SimpleField(registry, entity, name, &TextPathComponent::method,
                     value == "align" ? TextPathMethod::Align : TextPathMethod::Stretch);
}

bool ApplyTextPathSpacing(Registry& registry, Entity entity, std::string_view name,
                          std::string_view value) {
  if (value != "auto" && value != "exact") {
    return false;
  }
  return SimpleField(registry, entity, name, &TextPathComponent::spacing,
                     value == "auto" ? TextPathSpacing::Auto : TextPathSpacing::Exact);
}

bool ApplyTextPathAttribute(Registry& registry, Entity entity, std::string_view name,
                            std::string_view value) {
  if (!registry.all_of<TextPathComponent>(entity)) {
    return false;
  }
  if (name == "startOffset") {
    return ApplyTextPathStartOffset(registry, entity, name, value);
  }
  if (name == "side") {
    return ApplyTextPathSide(registry, entity, name, value);
  }
  if (name == "method") {
    return ApplyTextPathMethod(registry, entity, name, value);
  }
  if (name == "spacing") {
    return ApplyTextPathSpacing(registry, entity, name, value);
  }
  return false;
}

bool ApplyTextPathGeometry(Registry& registry, Entity entity, std::string_view name,
                           std::string_view value) {
  if (name != "path" || !registry.all_of<TextPathComponent>(entity)) {
    return false;
  }
  auto* restoration = registry.ctx().find<AttributeRestoreContext>();
  auto& budget = restoration ? restoration->budget : registry.ctx().get<AnimationResourceBudget>();
  if (value.size() > AnimationResourceBudget::kMaximumPathValueBytes ||
      !budget.reserveGeometryWork(value.size())) {
    return true;
  }
  auto parsed = parser::PathParser::Parse(value);
  if (parsed.hasError() || parsed.result().empty()) {
    return true;
  }
  std::optional<Path> path(std::move(parsed.result()));
  return OverrideField<TextPathComponent>(
      registry, entity, name, path,
      [](const TextPathComponent& component) -> const auto& { return component.inlinePath; },
      [](TextPathComponent& component, const auto& value) { component.inlinePath = value; });
}

bool IsViewBoxTarget(ElementType type) {
  switch (type) {
    case ElementType::SVG:
    case ElementType::Symbol:
    case ElementType::Marker:
    case ElementType::Pattern: return true;
    default: return false;
  }
}

bool IsGeometryTarget(ElementType type) {
  switch (type) {
    case ElementType::Circle:
    case ElementType::Ellipse:
    case ElementType::Rect:
    case ElementType::Line:
    case ElementType::Polyline:
    case ElementType::Polygon:
    case ElementType::Path: return true;
    default: return false;
  }
}

bool ApplyViewportGeometry(Registry& registry, Entity entity, std::string_view name,
                           std::string_view value) {
  const auto* type = registry.try_get<ElementTypeComponent>(entity);
  if (!type) {
    return false;
  }
  if (name == "viewBox" && IsViewBoxTarget(type->type())) {
    auto parsed = parser::ViewBoxParser::Parse(value);
    if (parsed.hasResult()) {
      (void)SimpleField(registry, entity, name, &ViewBoxComponent::viewBox, parsed.result(), true);
    }
    return true;
  }
  if (name != "preserveAspectRatio") {
    return false;
  }
  const auto parsed = parser::PreserveAspectRatioParser::Parse(value);
  if (parsed.hasError()) {
    return true;
  }
  if (type->type() == ElementType::FeImage) {
    return SimpleField(registry, entity, name, &FEImageComponent::preserveAspectRatio,
                       parsed.result());
  }
  if (IsViewBoxTarget(type->type()) || type->type() == ElementType::Image) {
    return SimpleField(registry, entity, name, &PreserveAspectRatioComponent::preserveAspectRatio,
                       parsed.result(), true);
  }
  return false;
}

bool ApplyPathCalibration(Registry& registry, Entity entity, std::string_view name,
                          std::string_view value) {
  const auto* type = registry.try_get<ElementTypeComponent>(entity);
  if (name != "pathLength" || !type || !IsGeometryTarget(type->type())) {
    return false;
  }
  const auto number = Number(value);
  if (number && *number >= 0) {
    return SimpleField(registry, entity, name, &PathLengthComponent::value, *number, true);
  }
  return true;
}

bool ApplyLayoutAttribute(Registry& registry, Entity entity, std::string_view name,
                          std::string_view value) {
  if (ApplyTextPosition(registry, entity, name, value) ||
      ApplyLayoutLength(registry, entity, name, value) ||
      ApplyLayoutEnumeration(registry, entity, name, value)) {
    return true;
  }
  if (ApplyViewportReferenceLength(registry, entity, name, value) ||
      ApplyMarkerOrientation(registry, entity, name, value) ||
      ApplyMaskOrClipUnits(registry, entity, name, value)) {
    return true;
  }
  return ApplyTextPathAttribute(registry, entity, name, value) ||
         ApplyTextPathGeometry(registry, entity, name, value) ||
         ApplyViewportGeometry(registry, entity, name, value) ||
         ApplyPathCalibration(registry, entity, name, value);
}

void ApplyAttribute(Registry& registry, Entity entity, std::string_view name,
                    std::string_view value) {
  if (ApplyLayoutAttribute(registry, entity, name, value)) {
    LayoutSystem().invalidate(EntityHandle(registry, entity),
                              /*viewportChanged=*/name == "viewBox");
    InvalidateAnimatedText(registry, entity);
    return;
  }
  if (ApplyFilterControl(registry, entity, name, value) ||
      ApplyDiscreteAttribute(registry, entity, name, value) ||
      ApplyFilterList(registry, entity, name, value) ||
      ApplyFilterPair(registry, entity, name, value)) {
    return;
  }
  if (ApplyGradientLength(registry, entity, name, value)) {
    return;
  }
  if (name == "stdDeviation") {
    const auto parsed = parser::Number2dParser::Parse(value);
    if (parsed.hasResult() && parsed.result().consumedChars == value.size() &&
        std::isfinite(parsed.result().numberX) && std::isfinite(parsed.result().numberY) &&
        parsed.result().numberX >= 0 && parsed.result().numberY >= 0) {
      if (!ApplyDeviation<FEGaussianBlurComponent>(
              registry, entity, name, Vector2d(parsed.result().numberX, parsed.result().numberY))) {
        (void)ApplyDeviation<FEDropShadowComponent>(
            registry, entity, name, Vector2d(parsed.result().numberX, parsed.result().numberY));
      }
    }
    return;
  }
  if (name == "offset" && registry.all_of<StopComponent>(entity)) {
    donner::parser::LengthParser::Options options;
    options.unitOptional = true;
    options.limitUnitToPercentage = true;
    const auto parsed = donner::parser::LengthParser::Parse(value, options);
    if (parsed.hasResult() && parsed.result().consumedChars == value.size()) {
      const auto length = parsed.result().length;
      const auto offset = static_cast<float>(std::clamp(
          length.unit == Lengthd::Unit::Percent ? length.value / 100 : length.value, 0.0, 1.0));
      (void)OverrideField<StopComponent>(
          registry, entity, name, offset,
          [](const StopComponent& component) { return component.properties.offset; },
          [](StopComponent& component, float value) { component.properties.offset = value; });
    }
    return;
  }
  if (auto number = Number(value)) {
    (void)ApplyFilterNumber(registry, entity, name, *number);
  }
}

struct ChangedAttribute {
  Entity entity;
  std::string name;
  std::optional<RcString> value;
};

void RestoreAttributeBackup(Registry& registry, Entity entity,
                            std::vector<ChangedAttribute>& changed) {
  auto* stored = registry.try_get<AnimationAttributeBackup>(entity);
  if (!stored) {
    return;
  }
  auto backup = std::move(*stored);
  registry.remove<AnimationAttributeBackup>(entity);
  for (const auto& [name, field] : backup.fields) {
    const auto source = SourceAttribute(registry, entity, name);
    if (!source && source != field.source) {
      field.reset(registry, entity);
    } else {
      field.restore(registry, entity);
      if (source != field.source) {
        changed.push_back({entity, name, source});
      }
    }
  }
  LayoutSystem().invalidate(EntityHandle(registry, entity),
                            /*viewportChanged=*/backup.fields.contains("viewBox"));
  InvalidateAnimatedText(registry, entity);
}

}  // namespace

void RestoreAnimationAttributes(Registry& registry, Entity target) {
  std::vector<ChangedAttribute> changed;
  if (target != entt::null) {
    RestoreAttributeBackup(registry, target, changed);
  } else {
    for (auto entity : registry.view<AnimationAttributeBackup>()) {
      RestoreAttributeBackup(registry, entity, changed);
    }
  }
  if (changed.empty()) {
    return;
  }
  registry.ctx().emplace<AttributeRestoreContext>();
  for (const auto& change : changed) {
    if (change.value && change.value->size() > AnimationResourceBudget::kMaximumSourceBytes) {
      continue;
    }
    const std::string_view value = change.value ? std::string_view(*change.value)
                                                : (change.name == "viewBox" ? "0 0 1 1" : "0");
    ApplyAttribute(registry, change.entity, change.name, value);
  }
  registry.ctx().erase<AttributeRestoreContext>();
}

bool ApplyAnimationAttributes(Registry& registry) {
  for (auto [entity, values] : registry.view<AnimatedValuesComponent>().each()) {
    for (const auto& [name, value] : values.overrides) {
      ApplyAttribute(registry, entity, name, value);
      if (registry.ctx().get<AnimationResourceBudget>().rejected()) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace donner::svg::components
