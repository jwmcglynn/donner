#include "donner/svg/SVGTextPositioningElement.h"

#include "donner/svg/components/text/TextPositioningComponent.h"

namespace donner::svg {

SVGTextPositioningElement::SVGTextPositioningElement(EntityHandle handle)
    : SVGTextContentElement(handle) {
  handle_.emplace<components::TextPositioningComponent>();
}

void SVGTextPositioningElement::setX(std::optional<Lengthd> value) {
  SmallVector<Lengthd, 1>& x = handle_.get<components::TextPositioningComponent>().x;
  if (value) {
    x = {*value};
  } else {
    x.clear();
  }
}

void SVGTextPositioningElement::setXList(SmallVector<Lengthd, 1>&& value) {
  handle_.get<components::TextPositioningComponent>().x = std::move(value);
}

std::optional<Lengthd> SVGTextPositioningElement::x() const {
  SmallVector<Lengthd, 1>& x = handle_.get<components::TextPositioningComponent>().x;
  if (x.empty()) {
    return std::nullopt;
  }

  return x[0];
}

const SmallVector<Lengthd, 1>& SVGTextPositioningElement::xList() const {
  return handle_.get<components::TextPositioningComponent>().x;
}

void SVGTextPositioningElement::setY(std::optional<Lengthd> value) {
  SmallVector<Lengthd, 1>& y = handle_.get<components::TextPositioningComponent>().y;
  if (value) {
    y = {*value};
  } else {
    y.clear();
  }
}

void SVGTextPositioningElement::setYList(SmallVector<Lengthd, 1>&& value) {
  handle_.get<components::TextPositioningComponent>().y = std::move(value);
}

std::optional<Lengthd> SVGTextPositioningElement::y() const {
  SmallVector<Lengthd, 1>& y = handle_.get<components::TextPositioningComponent>().y;
  if (y.empty()) {
    return std::nullopt;
  }

  return y[0];
}

const SmallVector<Lengthd, 1>& SVGTextPositioningElement::yList() const {
  return handle_.get<components::TextPositioningComponent>().y;
}

void SVGTextPositioningElement::setDx(std::optional<Lengthd> value) {
  SmallVector<Lengthd, 1>& dx = handle_.get<components::TextPositioningComponent>().dx;
  if (value) {
    dx = {*value};
  } else {
    dx.clear();
  }
}

void SVGTextPositioningElement::setDxList(SmallVector<Lengthd, 1>&& value) {
  handle_.get_or_emplace<components::TextPositioningComponent>().dx = std::move(value);
}

std::optional<Lengthd> SVGTextPositioningElement::dx() const {
  const SmallVector<Lengthd, 1>& dx = handle_.get<components::TextPositioningComponent>().dx;
  if (dx.empty()) {
    return std::nullopt;
  }

  return dx[0];
}

const SmallVector<Lengthd, 1>& SVGTextPositioningElement::dxList() const {
  return handle_.get<components::TextPositioningComponent>().dx;
}

void SVGTextPositioningElement::setDy(std::optional<Lengthd> value) {
  SmallVector<Lengthd, 1>& dy = handle_.get<components::TextPositioningComponent>().dy;
  if (value) {
    dy = {*value};
  } else {
    dy.clear();
  }
}

void SVGTextPositioningElement::setDyList(SmallVector<Lengthd, 1>&& value) {
  handle_.get_or_emplace<components::TextPositioningComponent>().dy = std::move(value);
}

std::optional<Lengthd> SVGTextPositioningElement::dy() const {
  const SmallVector<Lengthd, 1>& dy = handle_.get<components::TextPositioningComponent>().dy;
  if (dy.empty()) {
    return std::nullopt;
  }

  return dy[0];
}

const SmallVector<Lengthd, 1>& SVGTextPositioningElement::dyList() const {
  return handle_.get<components::TextPositioningComponent>().dy;
}

void SVGTextPositioningElement::setRotate(std::optional<double> degrees) {
  SmallVector<double, 1>& rotateDegrees =
      handle_.get<components::TextPositioningComponent>().rotateDegrees;
  if (degrees) {
    rotateDegrees = {*degrees};
  } else {
    rotateDegrees.clear();
  }
}

void SVGTextPositioningElement::setRotateList(SmallVector<double, 1>&& value) {
  handle_.get_or_emplace<components::TextPositioningComponent>().rotateDegrees = std::move(value);
}

std::optional<double> SVGTextPositioningElement::rotate() const {
  const auto& rotateDegrees = handle_.get<components::TextPositioningComponent>().rotateDegrees;
  if (rotateDegrees.empty()) {
    return std::nullopt;
  }

  return rotateDegrees[0];
}

const SmallVector<double, 1>& SVGTextPositioningElement::rotateList() const {
  return handle_.get<components::TextPositioningComponent>().rotateDegrees;
}

}  // namespace donner::svg
