#include "src/svg/svg_filter_element.h"

#include "src/svg/components/filter/filter_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGFilterElement SVGFilterElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  EntityHandle handle = CreateEntity(registry, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::FilterComponent>();
  return SVGFilterElement(handle);
}

Lengthd SVGFilterElement::x() const {
  return handle_.get<components::FilterComponent>().x.value_or(
      Lengthd(-10.0, Lengthd::Unit::Percent));
}

Lengthd SVGFilterElement::y() const {
  return handle_.get<components::FilterComponent>().y.value_or(
      Lengthd(-10.0, Lengthd::Unit::Percent));
}

Lengthd SVGFilterElement::width() const {
  return handle_.get<components::FilterComponent>().width.value_or(
      Lengthd(120.0, Lengthd::Unit::Percent));
}

Lengthd SVGFilterElement::height() const {
  return handle_.get<components::FilterComponent>().height.value_or(
      Lengthd(120.0, Lengthd::Unit::Percent));
}

void SVGFilterElement::setX(const Lengthd& value) {
  handle_.get<components::FilterComponent>().x = value;
}

void SVGFilterElement::setY(const Lengthd& value) {
  handle_.get<components::FilterComponent>().y = value;
}

void SVGFilterElement::setWidth(const Lengthd& value) {
  handle_.get<components::FilterComponent>().width = value;
}

void SVGFilterElement::setHeight(const Lengthd& value) {
  handle_.get<components::FilterComponent>().height = value;
}

FilterUnits SVGFilterElement::filterUnits() const {
  return handle_.get<components::FilterComponent>().filterUnits;
}

void SVGFilterElement::setFilterUnits(FilterUnits value) {
  handle_.get<components::FilterComponent>().filterUnits = value;
}

PrimitiveUnits SVGFilterElement::primitiveUnits() const {
  return handle_.get<components::FilterComponent>().primitiveUnits;
}

void SVGFilterElement::setPrimitiveUnits(PrimitiveUnits value) {
  handle_.get<components::FilterComponent>().primitiveUnits = value;
}

}  // namespace donner::svg
