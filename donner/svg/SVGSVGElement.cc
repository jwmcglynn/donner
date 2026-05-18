#include "donner/svg/SVGSVGElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/core/UserAgentStylesheet.h"

namespace donner::svg {

SVGSVGElement SVGSVGElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::ViewBoxComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>();
  handle.emplace<components::SizedElementComponent>();

  auto& stylesheetComponent = handle.emplace<components::StylesheetComponent>();
  stylesheetComponent.isUserAgentStylesheet = true;

  // From https://www.w3.org/TR/SVG2/styling.html#UAStyleSheet
  stylesheetComponent.parseStylesheet(kUserAgentStylesheet);
  return SVGSVGElement(handle);
}

std::optional<Box2d> SVGSVGElement::viewBox() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::ViewBoxComponent>().viewBox;
}

PreserveAspectRatio SVGSVGElement::preserveAspectRatio() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;
}

Lengthd SVGSVGElement::x() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::SizedElementComponent>().properties.x.getRequired();
}

Lengthd SVGSVGElement::y() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::SizedElementComponent>().properties.y.getRequired();
}

std::optional<Lengthd> SVGSVGElement::width() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::SizedElementComponent>().properties.width.getRequired();
}

std::optional<Lengthd> SVGSVGElement::height() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  return handle_.get<components::SizedElementComponent>().properties.height.getRequired();
}

void SVGSVGElement::setViewBox(std::optional<Box2d> viewBox) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get<components::ViewBoxComponent>().viewBox = viewBox;
  access.bumpMutationRevision();
}

void SVGSVGElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio = preserveAspectRatio;
  access.bumpMutationRevision();
}

void SVGSVGElement::setX(Lengthd value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get<components::SizedElementComponent>().properties.x.set(value,
                                                                    css::Specificity::Override());
  access.bumpMutationRevision();
}

void SVGSVGElement::setY(Lengthd value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get<components::SizedElementComponent>().properties.y.set(value,
                                                                    css::Specificity::Override());
  access.bumpMutationRevision();
}

void SVGSVGElement::setWidth(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get<components::SizedElementComponent>().properties.width.set(
      value, css::Specificity::Override());
  access.bumpMutationRevision();
}

void SVGSVGElement::setHeight(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get<components::SizedElementComponent>().properties.height.set(
      value, css::Specificity::Override());
  access.bumpMutationRevision();
}

}  // namespace donner::svg
