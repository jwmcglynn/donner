#include "donner/svg/SVGTextContentElement.h"

#include "donner/base/Box.h"
#include "donner/base/Vector2.h"
#include "donner/svg/components/text/TextComponent.h"

namespace donner::svg {

SVGTextContentElement::SVGTextContentElement(EntityHandle handle) : SVGGraphicsElement(handle) {
  handle_.emplace<components::TextComponent>();
}

std::optional<Lengthd> SVGTextContentElement::textLength() const {
  return handle_.get<components::TextComponent>().textLength;
}

void SVGTextContentElement::setTextLength(std::optional<Lengthd> value) {
  handle_.get<components::TextComponent>().textLength = value;
}

LengthAdjust SVGTextContentElement::lengthAdjust() const {
  return handle_.get<components::TextComponent>().lengthAdjust;
}

void SVGTextContentElement::setLengthAdjust(LengthAdjust value) {
  handle_.get_or_emplace<components::TextComponent>().lengthAdjust = value;
}

long SVGTextContentElement::getNumberOfChars() const {
  return handle_.get<components::TextComponent>().text.size();
}

double SVGTextContentElement::getComputedTextLength() const {
  // TODO: implement proper text length computation
  return 0.0;
}

double SVGTextContentElement::getSubStringLength(std::size_t charnum, std::size_t nchars) const {
  // TODO: implement proper text length computation
  return 0.0;
}

Vector2d SVGTextContentElement::getStartPositionOfChar(std::size_t /*charnum*/) const {
  // TODO: implement proper start position
  return Vector2d();
}

Vector2d SVGTextContentElement::getEndPositionOfChar(std::size_t /*charnum*/) const {
  // TODO: implement proper end position
  return Vector2d();
}

Boxd SVGTextContentElement::getExtentOfChar(std::size_t /*charnum*/) const {
  // TODO: implement proper extent
  return Boxd(Vector2d(), Vector2d());
}

double SVGTextContentElement::getRotationOfChar(std::size_t /*charnum*/) const {
  // TODO: implement proper rotation
  return 0.0;
}

long SVGTextContentElement::getCharNumAtPosition(const Vector2d& /*point*/) const {
  // TODO: implement proper hit testing
  return -1;
}

void SVGTextContentElement::selectSubString(std::size_t /*charnum*/, std::size_t /*nchars*/) {
  // TODO: implement selection highlighting
}

void SVGTextContentElement::appendText(std::string_view text) {
  auto& textComponent = handle_.get<components::TextComponent>();

  if (textComponent.text.empty()) {
    textComponent.text = text;
  } else {
    textComponent.text = textComponent.text + text;
  }
}

RcString SVGTextContentElement::textContent() const {
  return handle_.get<components::TextComponent>().text;
}

}  // namespace donner::svg
