#include "src/svg/svg_filter_primitive_standard_attributes.h"

#include "src/svg/components/filter/filter_primitive_component.h"
#include "src/svg/components/rendering_behavior_component.h"

namespace donner::svg {

SVGFilterPrimitiveStandardAttributes::SVGFilterPrimitiveStandardAttributes(EntityHandle handle)
    : SVGElement(handle) {
  handle_.emplace<components::FilterPrimitiveComponent>();
  handle_.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
}

Lengthd SVGFilterPrimitiveStandardAttributes::x() const {
  return handle_.get<components::FilterPrimitiveComponent>().x.value_or(
      Lengthd(-10.0, Lengthd::Unit::Percent));
}

Lengthd SVGFilterPrimitiveStandardAttributes::y() const {
  return handle_.get<components::FilterPrimitiveComponent>().y.value_or(
      Lengthd(-10.0, Lengthd::Unit::Percent));
}

Lengthd SVGFilterPrimitiveStandardAttributes::width() const {
  return handle_.get<components::FilterPrimitiveComponent>().width.value_or(
      Lengthd(120.0, Lengthd::Unit::Percent));
}

Lengthd SVGFilterPrimitiveStandardAttributes::height() const {
  return handle_.get<components::FilterPrimitiveComponent>().height.value_or(
      Lengthd(120.0, Lengthd::Unit::Percent));
}

void SVGFilterPrimitiveStandardAttributes::setX(const Lengthd& value) {
  handle_.get<components::FilterPrimitiveComponent>().x = value;
}

void SVGFilterPrimitiveStandardAttributes::setY(const Lengthd& value) {
  handle_.get<components::FilterPrimitiveComponent>().y = value;
}

void SVGFilterPrimitiveStandardAttributes::setWidth(const Lengthd& value) {
  handle_.get<components::FilterPrimitiveComponent>().width = value;
}

void SVGFilterPrimitiveStandardAttributes::setHeight(const Lengthd& value) {
  handle_.get<components::FilterPrimitiveComponent>().height = value;
}

std::optional<RcString> SVGFilterPrimitiveStandardAttributes::result() const {
  return handle_.get<components::FilterPrimitiveComponent>().result;
}

void SVGFilterPrimitiveStandardAttributes::setResult(const RcStringOrRef& value) {
  handle_.get<components::FilterPrimitiveComponent>().result = value;
}

}  // namespace donner::svg
