#include "donner/svg/SVGSymbolElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/SymbolComponent.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"

namespace donner::svg {

SVGSymbolElement SVGSymbolElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  // Set the rendering behavior for a symbol, which is not rendered directly.
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::ShadowOnlyChildren);
  handle.emplace<components::ViewBoxComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>();
  handle.emplace<components::SymbolComponent>();
  handle.emplace<components::SizedElementComponent>().canOverrideWidthHeightForSymbol = true;
  return SVGSymbolElement(handle);
}

void SVGSymbolElement::setViewBox(OptionalRef<Boxd> viewBox) {
  handle_.get<components::ViewBoxComponent>().viewBox = viewBox;
}

std::optional<Boxd> SVGSymbolElement::viewBox() const {
  return handle_.get<components::ViewBoxComponent>().viewBox;
}

void SVGSymbolElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>().preserveAspectRatio =
      preserveAspectRatio;
}

PreserveAspectRatio SVGSymbolElement::preserveAspectRatio() const {
  return handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;
}

void SVGSymbolElement::setX(Lengthd value) {
  handle_.get<components::SizedElementComponent>().properties.x.set(value,
                                                                    css::Specificity::Override());
}

Lengthd SVGSymbolElement::x() const {
  return handle_.get<components::SizedElementComponent>().properties.x.getRequired();
}

void SVGSymbolElement::setY(Lengthd value) {
  handle_.get<components::SizedElementComponent>().properties.y.set(value,
                                                                    css::Specificity::Override());
}

Lengthd SVGSymbolElement::y() const {
  return handle_.get<components::SizedElementComponent>().properties.y.getRequired();
}

void SVGSymbolElement::setWidth(std::optional<Lengthd> value) {
  handle_.get<components::SizedElementComponent>().properties.width.set(
      value, css::Specificity::Override());
}

std::optional<Lengthd> SVGSymbolElement::width() const {
  return handle_.get<components::SizedElementComponent>().properties.width.get();
}

void SVGSymbolElement::setHeight(std::optional<Lengthd> value) {
  handle_.get<components::SizedElementComponent>().properties.height.set(
      value, css::Specificity::Override());
}

std::optional<Lengthd> SVGSymbolElement::height() const {
  return handle_.get<components::SizedElementComponent>().properties.height.get();
}

void SVGSymbolElement::setRefX(double value) {
  handle_.get_or_emplace<components::SymbolComponent>().refX = value;
}

double SVGSymbolElement::refX() const {
  return handle_.get<components::SymbolComponent>().refX;
}

void SVGSymbolElement::setRefY(double value) {
  handle_.get_or_emplace<components::SymbolComponent>().refY = value;
}

double SVGSymbolElement::refY() const {
  return handle_.get<components::SymbolComponent>().refY;
}

}  // namespace donner::svg
