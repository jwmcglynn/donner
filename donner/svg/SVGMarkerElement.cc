#include "donner/svg/SVGMarkerElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"

namespace donner::svg {

SVGMarkerElement SVGMarkerElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGMarkerElement(handle);
}

void SVGMarkerElement::setMarkerWidth(double value) {
  handle_.get_or_emplace<components::MarkerComponent>().markerWidth = value;
}

double SVGMarkerElement::markerWidth() const {
  return handle_.get<components::MarkerComponent>().markerWidth;
}

void SVGMarkerElement::setMarkerHeight(double value) {
  handle_.get_or_emplace<components::MarkerComponent>().markerHeight = value;
}

double SVGMarkerElement::markerHeight() const {
  return handle_.get<components::MarkerComponent>().markerHeight;
}

void SVGMarkerElement::setRefX(double value) {
  handle_.get_or_emplace<components::MarkerComponent>().refX = value;
}

double SVGMarkerElement::refX() const {
  return handle_.get<components::MarkerComponent>().refX;
}

void SVGMarkerElement::setRefY(double value) {
  handle_.get_or_emplace<components::MarkerComponent>().refY = value;
}

double SVGMarkerElement::refY() const {
  return handle_.get<components::MarkerComponent>().refY;
}

void SVGMarkerElement::setOrient(std::string_view value) {
  // handle_.get_or_emplace<components::MarkerComponent>().orient = std::string(value);
}

std::string_view SVGMarkerElement::orient() const {
  // return handle_.get<components::MarkerComponent>().orient;
  return "";
}

}  // namespace donner::svg
