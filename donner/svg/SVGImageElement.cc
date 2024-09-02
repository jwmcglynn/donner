#include "donner/svg/SVGImageElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/AttributesComponent.h"
#include "donner/svg/components/style/StyleComponent.h"

namespace donner::svg {

SVGImageElement SVGImageElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGImageElement(handle);
}

void SVGImageElement::setHref(std::string_view value) {
  handle_.get_or_emplace<components::AttributesComponent>().setAttribute(XMLQualifiedName("href"),
                                                                         RcString(value));
}

std::string_view SVGImageElement::href() const {
  return handle_.get<components::AttributesComponent>().getAttribute(XMLQualifiedName("href")).value_or("");
}

void SVGImageElement::setX(Lengthd value) {
  handle_.get_or_emplace<components::StyleComponent>().setPresentationAttribute("x", value.toString());
}

Lengthd SVGImageElement::x() const {
  return handle_.get<components::StyleComponent>().getPresentationAttribute<Lengthd>("x").value_or(Lengthd());
}

void SVGImageElement::setY(Lengthd value) {
  handle_.get_or_emplace<components::StyleComponent>().setPresentationAttribute("y", value.toString());
}

Lengthd SVGImageElement::y() const {
  return handle_.get<components::StyleComponent>().getPresentationAttribute<Lengthd>("y").value_or(Lengthd());
}

void SVGImageElement::setWidth(Lengthd value) {
  handle_.get_or_emplace<components::StyleComponent>().setPresentationAttribute("width", value.toString());
}

Lengthd SVGImageElement::width() const {
  return handle_.get<components::StyleComponent>().getPresentationAttribute<Lengthd>("width").value_or(Lengthd());
}

void SVGImageElement::setHeight(Lengthd value) {
  handle_.get_or_emplace<components::StyleComponent>().setPresentationAttribute("height", value.toString());
}

Lengthd SVGImageElement::height() const {
  return handle_.get<components::StyleComponent>().getPresentationAttribute<Lengthd>("height").value_or(Lengthd());
}

Lengthd SVGImageElement::computedX() const {
  return getComputedStyle().get<Lengthd>("x").value_or(Lengthd());
}

Lengthd SVGImageElement::computedY() const {
  return getComputedStyle().get<Lengthd>("y").value_or(Lengthd());
}

Lengthd SVGImageElement::computedWidth() const {
  return getComputedStyle().get<Lengthd>("width").value_or(Lengthd());
}

Lengthd SVGImageElement::computedHeight() const {
  return getComputedStyle().get<Lengthd>("height").value_or(Lengthd());
}

void SVGImageElement::invalidate() const {
  handle_.remove<components::ComputedStyleComponent>();
}

void SVGImageElement::compute() const {
  components::StyleSystem().computeStyle(handle_, nullptr);
}

}  // namespace donner::svg
