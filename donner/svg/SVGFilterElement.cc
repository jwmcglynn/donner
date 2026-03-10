#include "donner/svg/SVGFilterElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"

namespace donner::svg {

SVGFilterElement SVGFilterElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
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

std::optional<RcString> SVGFilterElement::href() const {
  auto maybeHref = handle_.get<components::FilterComponent>().href;
  if (maybeHref.has_value()) {
    return maybeHref->href;
  } else {
    return std::nullopt;
  }
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

void SVGFilterElement::setHref(OptionalRef<RcStringOrRef> value) {
  if (value) {
    handle_.get<components::FilterComponent>().href = Reference(RcString(value.value()));
  } else {
    handle_.get<components::FilterComponent>().href = std::nullopt;
  }

  handle_.remove<components::ComputedFilterComponent>();
}

FilterUnits SVGFilterElement::filterUnits() const {
  return handle_.get<components::FilterComponent>().filterUnits.value_or(FilterUnits::Default);
}

void SVGFilterElement::setFilterUnits(FilterUnits value) {
  handle_.get<components::FilterComponent>().filterUnits = value;
}

PrimitiveUnits SVGFilterElement::primitiveUnits() const {
  return handle_.get<components::FilterComponent>().primitiveUnits.value_or(
      PrimitiveUnits::Default);
}

void SVGFilterElement::setPrimitiveUnits(PrimitiveUnits value) {
  handle_.get<components::FilterComponent>().primitiveUnits = value;
}

}  // namespace donner::svg
