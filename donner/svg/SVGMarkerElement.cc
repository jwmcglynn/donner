#include "donner/svg/SVGMarkerElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"

namespace donner::svg {

SVGMarkerElement SVGMarkerElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGMarkerElement(handle);
}

void SVGMarkerElement::setMarkerWidth(double value) {
  handle_.setAttribute("markerWidth", std::to_string(value));
}

double SVGMarkerElement::markerWidth() const {
  return std::stod(handle_.getAttribute("markerWidth").value_or("0"));
}

void SVGMarkerElement::setMarkerHeight(double value) {
  handle_.setAttribute("markerHeight", std::to_string(value));
}

double SVGMarkerElement::markerHeight() const {
  return std::stod(handle_.getAttribute("markerHeight").value_or("0"));
}

void SVGMarkerElement::setRefX(double value) {
  handle_.setAttribute("refX", std::to_string(value));
}

double SVGMarkerElement::refX() const {
  return std::stod(handle_.getAttribute("refX").value_or("0"));
}

void SVGMarkerElement::setRefY(double value) {
  handle_.setAttribute("refY", std::to_string(value));
}

double SVGMarkerElement::refY() const {
  return std::stod(handle_.getAttribute("refY").value_or("0"));
}

void SVGMarkerElement::setOrient(std::string_view value) {
  handle_.setAttribute("orient", std::string(value));
}

std::string_view SVGMarkerElement::orient() const {
  return handle_.getAttribute("orient").value_or("");
}

}  // namespace donner::svg
