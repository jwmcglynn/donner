#include "src/svg/svg_pattern_element.h"

#include "src/svg/components/computed_shadow_tree_component.h"
#include "src/svg/components/pattern_component.h"
#include "src/svg/components/preserve_aspect_ratio_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/style_component.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGPatternElement SVGPatternElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, RcString(Tag), Type);
  handle.emplace<RenderingBehaviorComponent>(RenderingBehavior::ShadowOnlyChildren)
      .appliesTransform = false;
  handle.emplace<PatternComponent>();
  handle.emplace<SizedElementComponent>();
  handle.emplace<DoNotInheritFillOrStrokeTag>();
  handle.emplace<ViewboxComponent>();
  return SVGPatternElement(handle);
}

std::optional<Boxd> SVGPatternElement::viewbox() const {
  return handle_.get<ViewboxComponent>().viewbox;
}

PreserveAspectRatio SVGPatternElement::preserveAspectRatio() const {
  return handle_.get<PreserveAspectRatioComponent>().preserveAspectRatio;
}

Lengthd SVGPatternElement::x() const {
  return handle_.get<SizedElementComponent>().properties.x.getRequired();
}

Lengthd SVGPatternElement::y() const {
  return handle_.get<SizedElementComponent>().properties.y.getRequired();
}

std::optional<Lengthd> SVGPatternElement::width() const {
  return handle_.get<SizedElementComponent>().properties.width.getRequired();
}

std::optional<Lengthd> SVGPatternElement::height() const {
  return handle_.get<SizedElementComponent>().properties.height.getRequired();
}

PatternUnits SVGPatternElement::patternUnits() const {
  return handle_.get_or_emplace<PatternComponent>().patternUnits.value_or(PatternUnits::Default);
}

PatternContentUnits SVGPatternElement::patternContentUnits() const {
  return handle_.get_or_emplace<PatternComponent>().patternContentUnits.value_or(
      PatternContentUnits::Default);
}

Transformd SVGPatternElement::patternTransform() const {
  computeTransform();
  return handle_.get<ComputedTransformComponent>().transform;
}

std::optional<RcString> SVGPatternElement::href() const {
  auto maybeHref = handle_.get_or_emplace<PatternComponent>().href;
  if (maybeHref) {
    return maybeHref.value().href;
  } else {
    return std::nullopt;
  }
}

void SVGPatternElement::setViewbox(std::optional<Boxd> viewbox) {
  handle_.get<ViewboxComponent>().viewbox = viewbox;
}

void SVGPatternElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  handle_.get_or_emplace<PreserveAspectRatioComponent>().preserveAspectRatio = preserveAspectRatio;
}

void SVGPatternElement::setX(Lengthd value) {
  handle_.get<SizedElementComponent>().properties.x.set(value, css::Specificity::Override());
}

void SVGPatternElement::setY(Lengthd value) {
  handle_.get<SizedElementComponent>().properties.y.set(value, css::Specificity::Override());
}

void SVGPatternElement::setWidth(std::optional<Lengthd> value) {
  handle_.get<SizedElementComponent>().properties.width.set(value, css::Specificity::Override());
}

void SVGPatternElement::setHeight(std::optional<Lengthd> value) {
  handle_.get<SizedElementComponent>().properties.height.set(value, css::Specificity::Override());
}

void SVGPatternElement::setPatternUnits(PatternUnits value) {
  handle_.get_or_emplace<PatternComponent>().patternUnits = value;
}

void SVGPatternElement::setPatternContentUnits(PatternContentUnits value) {
  handle_.get_or_emplace<PatternComponent>().patternContentUnits = value;
}

void SVGPatternElement::setPatternTransform(Transformd transform) {
  auto& component = handle_.get_or_emplace<TransformComponent>();
  component.transform.set(CssTransform(transform), css::Specificity::Override());
}

void SVGPatternElement::setHref(const std::optional<RcString>& value) {
  handle_.get_or_emplace<PatternComponent>().href = value;
  // Force the shadow tree to be regenerated.
  handle_.remove<ComputedShadowTreeComponent>();
}

void SVGPatternElement::invalidateTransform() {
  handle_.remove<ComputedTransformComponent>();
}

void SVGPatternElement::computeTransform() const {
  auto& transform = handle_.get_or_emplace<TransformComponent>();
  transform.compute(handle_, FontMetrics());
}

}  // namespace donner::svg
