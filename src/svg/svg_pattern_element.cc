#include "src/svg/svg_pattern_element.h"

#include "src/svg/components/layout/sized_element_component.h"
#include "src/svg/components/paint/pattern_component.h"
#include "src/svg/components/preserve_aspect_ratio_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/components/shadow/computed_shadow_tree_component.h"
#include "src/svg/components/style/style_component.h"
#include "src/svg/components/style/style_system.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGPatternElement SVGPatternElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle
      .emplace<components::RenderingBehaviorComponent>(
          components::RenderingBehavior::ShadowOnlyChildren)
      .appliesTransform = false;
  handle.emplace<components::PatternComponent>();
  handle.emplace<components::SizedElementComponent>();
  handle.emplace<components::DoNotInheritFillOrStrokeTag>();
  handle.emplace<components::ViewboxComponent>();
  return SVGPatternElement(handle);
}

std::optional<Boxd> SVGPatternElement::viewbox() const {
  return handle_.get<components::ViewboxComponent>().viewbox;
}

PreserveAspectRatio SVGPatternElement::preserveAspectRatio() const {
  return handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;
}

Lengthd SVGPatternElement::x() const {
  return handle_.get<components::SizedElementComponent>().properties.x.getRequired();
}

Lengthd SVGPatternElement::y() const {
  return handle_.get<components::SizedElementComponent>().properties.y.getRequired();
}

std::optional<Lengthd> SVGPatternElement::width() const {
  return handle_.get<components::SizedElementComponent>().properties.width.getRequired();
}

std::optional<Lengthd> SVGPatternElement::height() const {
  return handle_.get<components::SizedElementComponent>().properties.height.getRequired();
}

PatternUnits SVGPatternElement::patternUnits() const {
  return handle_.get_or_emplace<components::PatternComponent>().patternUnits.value_or(
      PatternUnits::Default);
}

PatternContentUnits SVGPatternElement::patternContentUnits() const {
  return handle_.get_or_emplace<components::PatternComponent>().patternContentUnits.value_or(
      PatternContentUnits::Default);
}

Transformd SVGPatternElement::patternTransform() const {
  computeTransform();
  return handle_.get<components::ComputedTransformComponent>().transform;
}

std::optional<RcString> SVGPatternElement::href() const {
  auto maybeHref = handle_.get_or_emplace<components::PatternComponent>().href;
  if (maybeHref) {
    return maybeHref.value().href;
  } else {
    return std::nullopt;
  }
}

void SVGPatternElement::setViewbox(std::optional<Boxd> viewbox) {
  handle_.get<components::ViewboxComponent>().viewbox = viewbox;
}

void SVGPatternElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>().preserveAspectRatio =
      preserveAspectRatio;
}

void SVGPatternElement::setX(Lengthd value) {
  handle_.get<components::SizedElementComponent>().properties.x.set(value,
                                                                    css::Specificity::Override());
}

void SVGPatternElement::setY(Lengthd value) {
  handle_.get<components::SizedElementComponent>().properties.y.set(value,
                                                                    css::Specificity::Override());
}

void SVGPatternElement::setWidth(std::optional<Lengthd> value) {
  handle_.get<components::SizedElementComponent>().properties.width.set(
      value, css::Specificity::Override());
}

void SVGPatternElement::setHeight(std::optional<Lengthd> value) {
  handle_.get<components::SizedElementComponent>().properties.height.set(
      value, css::Specificity::Override());
}

void SVGPatternElement::setPatternUnits(PatternUnits value) {
  handle_.get_or_emplace<components::PatternComponent>().patternUnits = value;
}

void SVGPatternElement::setPatternContentUnits(PatternContentUnits value) {
  handle_.get_or_emplace<components::PatternComponent>().patternContentUnits = value;
}

void SVGPatternElement::setPatternTransform(Transformd transform) {
  auto& component = handle_.get_or_emplace<components::TransformComponent>();
  component.transform.set(CssTransform(transform), css::Specificity::Override());
}

void SVGPatternElement::setHref(const std::optional<RcString>& value) {
  handle_.get_or_emplace<components::PatternComponent>().href = value;
  // Force the shadow tree to be regenerated.
  handle_.remove<components::ComputedShadowTreeComponent>();
}

void SVGPatternElement::invalidateTransform() {
  handle_.remove<components::ComputedTransformComponent>();
}

void SVGPatternElement::computeTransform() const {
  auto& transform = handle_.get_or_emplace<components::TransformComponent>();
  transform.computeWithPrecomputedStyle(handle_, components::StyleSystem().computeStyle(handle_),
                                        FontMetrics(), nullptr);
}

}  // namespace donner::svg
