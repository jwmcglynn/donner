#include "donner/svg/SVGSVGElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/StylesheetComponent.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"

namespace donner::svg {

SVGSVGElement SVGSVGElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::ViewBoxComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>();
  handle.emplace<components::SizedElementComponent>();

  auto& stylesheetComponent = handle.emplace<components::StylesheetComponent>();
  stylesheetComponent.isUserAgentStylesheet = true;

  // From https://www.w3.org/TR/SVG2/styling.html#UAStyleSheet
  stylesheetComponent.parseStylesheet(R"(
    @namespace url(http://www.w3.org/2000/svg);
    @namespace xml url(http://www.w3.org/XML/1998/namespace);

    svg:not(:root), image, marker, pattern, symbol { overflow: hidden; }

    *:not(svg),
    *:not(foreignObject) > svg {
      /* TODO: transform-origin: 0 0; */
    }

    *[xml|space=preserve] {
      text-space-collapse: preserve-spaces;
    }

    :host(use) > symbol {
      display: inline !important;
    }
    :link, :visited {
      cursor: pointer;
    })");
  return SVGSVGElement(handle);
}

std::optional<Boxd> SVGSVGElement::viewBox() const {
  return handle_.get<components::ViewBoxComponent>().viewBox;
}

PreserveAspectRatio SVGSVGElement::preserveAspectRatio() const {
  return handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;
}

Lengthd SVGSVGElement::x() const {
  return handle_.get<components::SizedElementComponent>().properties.x.getRequired();
}

Lengthd SVGSVGElement::y() const {
  return handle_.get<components::SizedElementComponent>().properties.y.getRequired();
}

std::optional<Lengthd> SVGSVGElement::width() const {
  return handle_.get<components::SizedElementComponent>().properties.width.getRequired();
}

std::optional<Lengthd> SVGSVGElement::height() const {
  return handle_.get<components::SizedElementComponent>().properties.height.getRequired();
}

void SVGSVGElement::setViewBox(std::optional<Boxd> viewBox) {
  handle_.get<components::ViewBoxComponent>().viewBox = viewBox;
}

void SVGSVGElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio = preserveAspectRatio;
}

void SVGSVGElement::setX(Lengthd value) {
  handle_.get<components::SizedElementComponent>().properties.x.set(value,
                                                                    css::Specificity::Override());
}

void SVGSVGElement::setY(Lengthd value) {
  handle_.get<components::SizedElementComponent>().properties.y.set(value,
                                                                    css::Specificity::Override());
}

void SVGSVGElement::setWidth(std::optional<Lengthd> value) {
  handle_.get<components::SizedElementComponent>().properties.width.set(
      value, css::Specificity::Override());
}

void SVGSVGElement::setHeight(std::optional<Lengthd> value) {
  handle_.get<components::SizedElementComponent>().properties.height.set(
      value, css::Specificity::Override());
}

}  // namespace donner::svg
