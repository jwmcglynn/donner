#include "src/svg/svg_line_element.h"

#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/line_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGLineElement SVGLineElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), RcString(Tag), Type);
  handle.emplace<RenderingBehaviorComponent>(RenderingBehavior::NoTraverseChildren);
  return SVGLineElement(handle);
}

void SVGLineElement::setX1(Lengthd value) {
  invalidate();
  handle_.get_or_emplace<LineComponent>().x1 = value;
}

void SVGLineElement::setY1(Lengthd value) {
  invalidate();
  handle_.get_or_emplace<LineComponent>().y1 = value;
}

void SVGLineElement::setX2(Lengthd value) {
  invalidate();
  handle_.get_or_emplace<LineComponent>().x2 = value;
}

void SVGLineElement::setY2(Lengthd value) {
  invalidate();
  handle_.get_or_emplace<LineComponent>().y2 = value;
}

Lengthd SVGLineElement::x1() const {
  return handle_.get_or_emplace<LineComponent>().x1;
}

Lengthd SVGLineElement::y1() const {
  return handle_.get_or_emplace<LineComponent>().y1;
}

Lengthd SVGLineElement::x2() const {
  return handle_.get_or_emplace<LineComponent>().x2;
}

Lengthd SVGLineElement::y2() const {
  return handle_.get_or_emplace<LineComponent>().y2;
}

void SVGLineElement::invalidate() const {
  handle_.remove<ComputedPathComponent>();
}

}  // namespace donner::svg
