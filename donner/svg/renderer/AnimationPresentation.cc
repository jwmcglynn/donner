#include "donner/svg/renderer/AnimationPresentation.h"

#include <array>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

#include "donner/base/RcString.h"
#include "donner/base/parser/LengthParser.h"
#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/shape/CircleComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/EllipseComponent.h"
#include "donner/svg/components/shape/LineComponent.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/components/shape/PolyComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/core/CssTransform.h"
#include "donner/svg/parser/PointsListParser.h"
#include "donner/svg/parser/TransformParser.h"
#include "donner/svg/renderer/AnimationAttributes.h"
#include "donner/svg/renderer/AnimationReferences.h"

namespace donner::svg::components {
namespace {

/// Set of animatable geometry length attribute names.
bool isGeometryLengthAttribute(std::string_view attrName) {
  return attrName == "cx" || attrName == "cy" || attrName == "r" || attrName == "rx" ||
         attrName == "ry" || attrName == "x" || attrName == "y" || attrName == "width" ||
         attrName == "height" || attrName == "x1" || attrName == "y1" || attrName == "x2" ||
         attrName == "y2";
}

/**
 * Backs up base component values that the animation override applier mutates in place, so the
 * mutation can be reverted on a later frame when the animation is no longer active (or targets a
 * different value). Without this, base-component mutations would "stick" across time samples of the
 * same live document. Captured lazily the first time an entity is animated.
 */
struct AnimationBaseBackup {
  std::optional<CircleComponent> circle;
  std::optional<EllipseComponent> ellipse;
  std::optional<RectComponent> rect;
  std::optional<LineComponent> line;
  std::array<std::optional<Lengthd>, 4> appliedLine;
  std::optional<PathComponent> path;
  std::optional<PolyComponent> poly;
  std::optional<RcString> polySource;
  std::vector<Vector2d> appliedPoints;
  std::optional<SizedElementComponent> sized;
  std::array<std::optional<RcString>, 4> lineSources;
  std::map<std::string, std::optional<RcString>> authored;
  std::string transformAttribute;
  bool transformCaptured = false;
  bool transformExisted = false;
  std::optional<TransformComponent> transform;
};

/// Remove the computed shape components for an entity so they are recomputed from the (restored or
/// overridden) base shape components on the next shape pass.
void invalidateComputedShape(Registry& registry, Entity entity) {
  registry.remove<ComputedPathComponent>(entity);
  registry.remove<ComputedCircleComponent>(entity);
  registry.remove<ComputedEllipseComponent>(entity);
  registry.remove<ComputedRectComponent>(entity);
}

std::optional<RcString> AuthoredAnimationAttribute(Registry& registry, Entity entity,
                                                   std::string_view name) {
  const auto* attributes = registry.try_get<donner::components::AttributesComponent>(entity);
  return attributes ? attributes->getAttribute(xml::XMLQualifiedNameRef(name)) : std::nullopt;
}

AnimationBaseBackup& AnimationBackupFor(Registry& registry, Entity entity) {
  auto& backup = registry.get_or_emplace<AnimationBaseBackup>(entity);
  if (backup.authored.empty()) {
    if (const auto* values = registry.try_get<AnimatedValuesComponent>(entity)) {
      for (const auto& [name, value] : values->overrides) {
        backup.authored.emplace(name, AuthoredAnimationAttribute(registry, entity, name));
      }
    }
  }
  return backup;
}

template <typename Property, typename Value>
void SetAnimatedProperty(Property& property, Value&& value) {
  property.substitute(std::forward<Value>(value));
  property.specificity = css::Specificity::Animation();
}

template <typename Properties, std::size_t... Indices>
void RestoreAnimatedProperties(Registry& registry, Entity entity, AnimationBaseBackup& backup,
                               Properties& current, Properties& saved,
                               std::index_sequence<Indices...>) {
  auto currentProperties = current.allProperties();
  auto savedProperties = saved.allProperties();
  const auto restore = [&](auto& property, const auto& base) {
    if (property.specificity != css::Specificity::Animation()) {
      return;
    }
    property = base;
    const auto savedSource = backup.authored.find(std::string(property.name));
    const auto currentSource = AuthoredAnimationAttribute(registry, entity, property.name);
    if (savedSource != backup.authored.end() && savedSource->second != currentSource) {
      property.clear();
      if (currentSource) {
        donner::parser::LengthParser::Options options;
        options.unitOptional = true;
        const auto parsed =
            donner::parser::LengthParser::Parse(std::string_view(*currentSource), options);
        if (parsed.hasResult() && parsed.result().consumedChars == currentSource->size()) {
          property.set(parsed.result().length, css::Specificity());
        }
      }
    }
  };
  (restore(std::get<Indices>(currentProperties), std::get<Indices>(savedProperties)), ...);
}

template <typename Component>
void RestoreAnimatedComponent(Registry& registry, Entity entity, AnimationBaseBackup& backup,
                              std::optional<Component>& saved) {
  if (!saved) {
    return;
  }
  if (auto* current = registry.try_get<Component>(entity)) {
    using Properties = decltype(current->properties);
    constexpr std::size_t count = std::tuple_size_v<decltype(current->properties.allProperties())>;
    RestoreAnimatedProperties<Properties>(registry, entity, backup, current->properties,
                                          saved->properties, std::make_index_sequence<count>());
  }
}

Lengthd ParseAuthoredLength(const std::optional<RcString>& source) {
  if (source) {
    donner::parser::LengthParser::Options options;
    options.unitOptional = true;
    const auto parsed = donner::parser::LengthParser::Parse(*source, options);
    if (parsed.hasResult() && parsed.result().consumedChars == source->size()) {
      return parsed.result().length;
    }
  }
  return Lengthd();
}

void RestoreAnimatedLine(Registry& registry, Entity entity, AnimationBaseBackup& backup) {
  auto* current = registry.try_get<LineComponent>(entity);
  if (!backup.line || !current) {
    return;
  }
  static constexpr std::array<std::string_view, 4> names = {"x1", "y1", "x2", "y2"};
  static constexpr std::array<Lengthd LineComponent::*, 4> fields = {
      &LineComponent::x1, &LineComponent::y1, &LineComponent::x2, &LineComponent::y2};
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (!backup.appliedLine[i]) {
      continue;
    }
    const auto source = AuthoredAnimationAttribute(registry, entity, names[i]);
    if (source != backup.lineSources[i]) {
      current->*fields[i] = ParseAuthoredLength(source);
    } else if (current->*fields[i] == *backup.appliedLine[i]) {
      current->*fields[i] = backup.line.value().*fields[i];
    }
  }
}

void RestoreAnimatedPath(Registry& registry, Entity entity, AnimationBaseBackup& backup) {
  auto* path = registry.try_get<PathComponent>(entity);
  if (!backup.path || !path || path->d.specificity != css::Specificity::Animation()) {
    return;
  }
  const auto source = AuthoredAnimationAttribute(registry, entity, "d");
  if (source != backup.authored["d"]) {
    path->d.clear();
    if (source) {
      path->d.set(*source, css::Specificity());
    }
    path->splineOverride.reset();
  } else {
    path->d = backup.path->d;
    if (!path->splineOverride) {
      path->splineOverride = std::move(backup.path->splineOverride);
    }
  }
}

void RestoreAnimatedPoly(Registry& registry, Entity entity, AnimationBaseBackup& backup) {
  auto* poly = registry.try_get<PolyComponent>(entity);
  if (!backup.poly || !poly) {
    return;
  }
  const auto source = AuthoredAnimationAttribute(registry, entity, "points");
  if (source != backup.polySource) {
    poly->points.clear();
    if (source) {
      auto parsed = parser::PointsListParser::Parse(*source);
      if (parsed.hasResult()) {
        poly->points = std::move(parsed.result());
      }
    }
  } else if (poly->points == backup.appliedPoints) {
    poly->points = std::move(backup.poly->points);
  }
}

void RestoreAnimatedTransform(Registry& registry, Entity entity, AnimationBaseBackup& backup) {
  auto* transform = registry.try_get<TransformComponent>(entity);
  if (!backup.transformCaptured || !transform ||
      transform->transform.specificity != css::Specificity::Animation()) {
    return;
  }
  const auto source = AuthoredAnimationAttribute(registry, entity, backup.transformAttribute);
  if (source != backup.authored[backup.transformAttribute]) {
    transform->transform.clear();
    if (source) {
      const auto parsed = parser::TransformParser::Parse(*source);
      if (parsed.hasResult()) {
        transform->transform.set(CssTransform(parsed.result()), css::Specificity());
      }
    }
  } else if (backup.transformExisted) {
    *transform = *backup.transform;
  } else {
    registry.remove<TransformComponent>(entity);
  }
  LayoutSystem().invalidate(EntityHandle(registry, entity));
}

void restoreAnimationBaseBackup(Registry& registry, Entity entity, AnimationBaseBackup& backup) {
  RestoreAnimatedComponent(registry, entity, backup, backup.circle);
  RestoreAnimatedComponent(registry, entity, backup, backup.ellipse);
  RestoreAnimatedComponent(registry, entity, backup, backup.rect);
  RestoreAnimatedComponent(registry, entity, backup, backup.sized);
  if (backup.sized) {
    // Sized-element lengths feed cached layout, such as a `<use>` instance's translation.
    LayoutSystem().invalidate(EntityHandle(registry, entity));
  }
  RestoreAnimatedLine(registry, entity, backup);
  RestoreAnimatedPath(registry, entity, backup);
  RestoreAnimatedPoly(registry, entity, backup);
  RestoreAnimatedTransform(registry, entity, backup);
  invalidateComputedShape(registry, entity);
}

/**
 * Advance the animation system to the current document time and apply the resulting animated value
 * overrides on top of the just-computed styles/geometry.
 *
 * This runs immediately after StyleSystem::computeAllStyles() and before layout/shape computation,
 * so that animated transforms participate in the transform hierarchy and animated geometry feeds
 * the shape pass. Overrides are applied post-style (the tiny-skia model).
 */

}  // namespace

void RestoreAnimationPresentationForMutation(Registry& registry, Entity entity) {
  RestoreAnimationReferences(registry, entity);
  RestoreAnimationAttributes(registry, entity);
  if (auto* saved = registry.try_get<AnimationBaseBackup>(entity)) {
    auto backup = std::move(*saved);
    registry.remove<AnimationBaseBackup>(entity);
    restoreAnimationBaseBackup(registry, entity, backup);
  }
}

bool ReserveAnimationGeometryBackups(Registry& registry, AnimationResourceBudget& budget) {
  for (auto [entity, values] : registry.view<AnimatedValuesComponent>().each()) {
    if (!budget.reserveOutput(sizeof(AnimationBaseBackup))) {
      return false;
    }
    if (values.overrides.contains("points")) {
      if (const auto* poly = registry.try_get<PolyComponent>(entity)) {
        if (!budget.reserveOutput(poly->points.capacity() * sizeof(Vector2d)) ||
            !budget.reserveOutput(values.overrides.at("points").size() * sizeof(Vector2d))) {
          return false;
        }
      }
    }
    if (values.overrides.contains("d")) {
      if (const auto* path = registry.try_get<PathComponent>(entity);
          path && path->splineOverride) {
        const auto bytes = path->splineOverride->retainedBytes();
        if (!budget.reserveOutput(
                bytes.value_or(AnimationResourceBudget::kMaximumOutputBytes + 1))) {
          return false;
        }
      }
    }
  }
  return true;
}

void RestoreAnimationPresentation(Registry& registry) {
  RestoreAnimationReferences(registry);
  RestoreAnimationAttributes(registry);
  for (auto [entity, backup] : registry.view<AnimationBaseBackup>().each()) {
    restoreAnimationBaseBackup(registry, entity, backup);
  }
  registry.clear<AnimationBaseBackup>();
}

namespace {

void RecomputeAnimationStyles(Registry& registry, bool enabled) {
  registry.ctx().get<AnimationStyleContext>().enabled = enabled;
  registry.clear<ComputedStyleComponent>();
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  StyleSystem().computeAllStyles(registry, warnings);
}

void RestoreRejectedAnimationPresentation(Registry& registry) {
  RestoreAnimationReferences(registry);
  RestoreAnimationAttributes(registry);
  registry.clear<AnimatedValuesComponent>();
  RecomputeAnimationStyles(registry, false);
  FinalizeAnimationReferences(registry);
}

bool IsTransformAnimationAttribute(std::string_view name) {
  return name == "transform" || name == "gradientTransform" || name == "patternTransform";
}

void ApplyAnimatedTransform(Registry& registry, Entity entity, std::string_view name,
                            std::string_view value) {
  auto parsed = parser::TransformParser::Parse(value);
  if (!parsed.hasResult()) {
    return;
  }
  auto& backup = AnimationBackupFor(registry, entity);
  if (!backup.transformCaptured) {
    backup.transformCaptured = true;
    backup.transformAttribute = name;
    backup.transformExisted = registry.all_of<TransformComponent>(entity);
    if (backup.transformExisted) {
      backup.transform = registry.get<TransformComponent>(entity);
    }
  }
  auto& transform = registry.get_or_emplace<TransformComponent>(entity);
  SetAnimatedProperty(transform.transform, CssTransform(parsed.result()));
  LayoutSystem().invalidate(EntityHandle(registry, entity));
}

bool ApplyAnimatedPoints(Registry& registry, Entity entity, std::string_view value) {
  auto* poly = registry.try_get<PolyComponent>(entity);
  if (!poly) {
    return false;
  }
  auto parsed = parser::PointsListParser::Parse(value);
  if (parsed.hasError()) {
    return false;
  }
  auto& backup = AnimationBackupFor(registry, entity);
  if (!backup.poly) {
    backup.poly = *poly;
    backup.polySource = AuthoredAnimationAttribute(registry, entity, "points");
  }
  poly->points = std::move(parsed.result());
  backup.appliedPoints = poly->points;
  return true;
}

void ApplyAnimatedPath(Registry& registry, Entity entity, std::string_view value) {
  auto* path = registry.try_get<PathComponent>(entity);
  if (!path) {
    return;
  }
  auto& backup = AnimationBackupFor(registry, entity);
  if (!backup.path) {
    backup.path = *path;
  }
  SetAnimatedProperty(path->d, RcString(value));
  path->splineOverride.reset();
  registry.remove<ComputedPathComponent>(entity);
}

bool ApplyAnimatedCircleLength(Registry& registry, Entity entity, std::string_view name,
                               Lengthd length) {
  auto* circle = registry.try_get<CircleComponent>(entity);
  if (!circle) {
    return false;
  }
  auto& backup = AnimationBackupFor(registry, entity);
  if (!backup.circle) {
    backup.circle = *circle;
  }
  if (name == "cx") {
    SetAnimatedProperty(circle->properties.cx, length);
    return true;
  }
  if (name == "cy") {
    SetAnimatedProperty(circle->properties.cy, length);
    return true;
  }
  if (name == "r") {
    SetAnimatedProperty(circle->properties.r, length);
    return true;
  }
  return false;
}

bool ApplyAnimatedEllipseLength(Registry& registry, Entity entity, std::string_view name,
                                Lengthd length) {
  auto* ellipse = registry.try_get<EllipseComponent>(entity);
  if (!ellipse) {
    return false;
  }
  auto& backup = AnimationBackupFor(registry, entity);
  if (!backup.ellipse) {
    backup.ellipse = *ellipse;
  }
  if (name == "cx") {
    SetAnimatedProperty(ellipse->properties.cx, length);
    return true;
  }
  if (name == "cy") {
    SetAnimatedProperty(ellipse->properties.cy, length);
    return true;
  }
  if (name == "rx") {
    SetAnimatedProperty(ellipse->properties.rx, length);
    return true;
  }
  if (name == "ry") {
    SetAnimatedProperty(ellipse->properties.ry, length);
    return true;
  }
  return false;
}

bool ApplyAnimatedRectLength(Registry& registry, Entity entity, std::string_view name,
                             Lengthd length) {
  auto* rect = registry.try_get<RectComponent>(entity);
  if (!rect) {
    return false;
  }
  auto& backup = AnimationBackupFor(registry, entity);
  if (!backup.rect) {
    backup.rect = *rect;
  }
  if (name == "x") {
    SetAnimatedProperty(rect->properties.x, length);
    return true;
  }
  if (name == "y") {
    SetAnimatedProperty(rect->properties.y, length);
    return true;
  }
  if (name == "width") {
    SetAnimatedProperty(rect->properties.width, length);
    return true;
  }
  if (name == "height") {
    SetAnimatedProperty(rect->properties.height, length);
    return true;
  }
  if (name == "rx") {
    SetAnimatedProperty(rect->properties.rx, length);
    return true;
  }
  if (name == "ry") {
    SetAnimatedProperty(rect->properties.ry, length);
    return true;
  }
  return false;
}

bool ApplyAnimatedSizedLength(Registry& registry, Entity entity, std::string_view name,
                              Lengthd length) {
  auto* sized = registry.try_get<SizedElementComponent>(entity);
  if (!sized) {
    return false;
  }
  auto& backup = AnimationBackupFor(registry, entity);
  if (!backup.sized) {
    backup.sized = *sized;
  }
  std::apply(
      [&](auto&... property) {
        ((property.name == name ? (SetAnimatedProperty(property, length), void()) : void()), ...);
      },
      sized->properties.allProperties());
  LayoutSystem().invalidate(EntityHandle(registry, entity));
  return true;
}

bool ApplyAnimatedLineLength(Registry& registry, Entity entity, std::string_view name,
                             Lengthd length) {
  auto* line = registry.try_get<LineComponent>(entity);
  if (!line) {
    return false;
  }
  auto& backup = AnimationBackupFor(registry, entity);
  if (!backup.line) {
    backup.line = *line;
    static constexpr std::array<std::string_view, 4> names = {"x1", "y1", "x2", "y2"};
    for (std::size_t index = 0; index < names.size(); ++index) {
      backup.lineSources[index] = AuthoredAnimationAttribute(registry, entity, names[index]);
    }
  }
  static constexpr std::array<std::string_view, 4> names = {"x1", "y1", "x2", "y2"};
  static constexpr std::array<Lengthd LineComponent::*, 4> fields = {
      &LineComponent::x1, &LineComponent::y1, &LineComponent::x2, &LineComponent::y2};
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (name == names[index]) {
      line->*fields[index] = length;
      backup.appliedLine[index] = length;
      return true;
    }
  }
  return false;
}

bool ApplyAnimatedGeometryLength(Registry& registry, Entity entity, std::string_view name,
                                 std::string_view value,
                                 const donner::parser::LengthParser::Options& options) {
  const auto parsed = donner::parser::LengthParser::Parse(value, options);
  if (!parsed.hasResult()) {
    return false;
  }
  const Lengthd length = parsed.result().length;
  bool changed = ApplyAnimatedCircleLength(registry, entity, name, length);
  changed |= ApplyAnimatedEllipseLength(registry, entity, name, length);
  changed |= ApplyAnimatedRectLength(registry, entity, name, length);
  changed |= ApplyAnimatedSizedLength(registry, entity, name, length);
  changed |= ApplyAnimatedLineLength(registry, entity, name, length);
  return changed;
}

void ApplyAnimatedOverride(Registry& registry, Entity entity, std::string_view name,
                           std::string_view value,
                           const donner::parser::LengthParser::Options& lengthOptions,
                           bool& geometryChanged) {
  if (IsTransformAnimationAttribute(name)) {
    ApplyAnimatedTransform(registry, entity, name, value);
  } else if (name == "points") {
    geometryChanged |= ApplyAnimatedPoints(registry, entity, value);
  } else if (name == "d") {
    ApplyAnimatedPath(registry, entity, value);
  } else if (isGeometryLengthAttribute(name)) {
    geometryChanged |= ApplyAnimatedGeometryLength(registry, entity, name, value, lengthOptions);
  }
}

void ApplyAnimatedOverrides(Registry& registry) {
  donner::parser::LengthParser::Options lengthOptions;
  lengthOptions.unitOptional = true;
  for (auto [entity, values] : registry.view<AnimatedValuesComponent>().each()) {
    bool geometryChanged = false;
    for (const auto& [name, value] : values.overrides) {
      ApplyAnimatedOverride(registry, entity, name, value, lengthOptions, geometryChanged);
    }
    if (geometryChanged) {
      invalidateComputedShape(registry, entity);
    }
  }
}

}  // namespace

void ApplyAnimationPresentation(Registry& registry) {
  const double documentTime = registry.ctx().get<SVGDocumentContext>().documentTime;
  AnimationSystem().advance(registry, documentTime, nullptr);
  if (!registry.view<AnimatedValuesComponent>().empty()) {
    RecomputeAnimationStyles(registry, true);
  }

  if (!ReserveAnimationGeometryBackups(registry, registry.ctx().get<AnimationResourceBudget>()) ||
      !ApplyAnimationReferences(registry) || !ApplyAnimationAttributes(registry)) {
    RestoreRejectedAnimationPresentation(registry);
    return;
  }

  FinalizeAnimationReferences(registry);
  ApplyAnimatedOverrides(registry);
}

}  // namespace donner::svg::components
