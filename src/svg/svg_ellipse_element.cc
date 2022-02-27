#include "src/svg/svg_ellipse_element.h"

#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/ellipse_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGEllipseElement SVGEllipseElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), RcString(Tag), Type);
  handle.emplace<RenderingBehaviorComponent>(RenderingBehavior::NoTraverseChildren);
  return SVGEllipseElement(handle);
}

void SVGEllipseElement::setCx(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<EllipseComponent>().properties;
  properties.cx.set(value, css::Specificity::Override());
}

void SVGEllipseElement::setCy(Lengthd value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<EllipseComponent>().properties;
  properties.cy.set(value, css::Specificity::Override());
}

void SVGEllipseElement::setRx(std::optional<Lengthd> value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<EllipseComponent>().properties;
  properties.rx.set(value, css::Specificity::Override());
}

void SVGEllipseElement::setRy(std::optional<Lengthd> value) {
  invalidate();

  auto& properties = handle_.get_or_emplace<EllipseComponent>().properties;
  properties.ry.set(value, css::Specificity::Override());
}

Lengthd SVGEllipseElement::cx() const {
  const auto* component = handle_.try_get<EllipseComponent>();
  return component ? component->properties.cx.getRequired() : Lengthd();
}

Lengthd SVGEllipseElement::cy() const {
  const auto* component = handle_.try_get<EllipseComponent>();
  return component ? component->properties.cy.getRequired() : Lengthd();
}

std::optional<Lengthd> SVGEllipseElement::rx() const {
  const auto* component = handle_.try_get<EllipseComponent>();
  return component ? component->properties.rx.get() : std::nullopt;
}

std::optional<Lengthd> SVGEllipseElement::ry() const {
  const auto* component = handle_.try_get<EllipseComponent>();
  return component ? component->properties.ry.get() : std::nullopt;
}

Lengthd SVGEllipseElement::computedCx() const {
  compute();
  return handle_.get<ComputedEllipseComponent>().properties.cx.getRequired();
}

Lengthd SVGEllipseElement::computedCy() const {
  compute();
  return handle_.get<ComputedEllipseComponent>().properties.cy.getRequired();
}

Lengthd SVGEllipseElement::computedRx() const {
  compute();

  const ComputedStyleComponent& style = handle_.get<ComputedStyleComponent>();
  return std::get<0>(handle_.get<ComputedEllipseComponent>().properties.calculateRx(style.viewbox(),
                                                                                    FontMetrics()));
}

Lengthd SVGEllipseElement::computedRy() const {
  compute();

  const ComputedStyleComponent& style = handle_.get_or_emplace<ComputedStyleComponent>();
  return std::get<0>(handle_.get<ComputedEllipseComponent>().properties.calculateRy(style.viewbox(),
                                                                                    FontMetrics()));
}

void SVGEllipseElement::invalidate() const {
  handle_.remove<ComputedEllipseComponent>();
  handle_.remove<ComputedPathComponent>();
}

void SVGEllipseElement::compute() const {
  auto& ellipse = handle_.get_or_emplace<EllipseComponent>();
  ellipse.computePath(handle_, FontMetrics());
}

}  // namespace donner::svg
