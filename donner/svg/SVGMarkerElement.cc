#include "donner/svg/SVGMarkerElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"

namespace donner::svg {

SVGMarkerElement SVGMarkerElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::MarkerComponent>();
  handle
      .emplace<components::RenderingBehaviorComponent>(
          components::RenderingBehavior::ShadowOnlyChildren)
      .inheritsParentTransform = false;
  handle.emplace<components::ViewBoxComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>();
  return SVGMarkerElement(handle);
}

void SVGMarkerElement::setViewBox(OptionalRef<Boxd> viewBox) {
  handle_.get<components::ViewBoxComponent>().viewBox = viewBox;
}

std::optional<Boxd> SVGMarkerElement::viewBox() const {
  return handle_.get<components::ViewBoxComponent>().viewBox;
}

void SVGMarkerElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>().preserveAspectRatio =
      preserveAspectRatio;
}

PreserveAspectRatio SVGMarkerElement::preserveAspectRatio() const {
  return handle_.get<components::PreserveAspectRatioComponent>().preserveAspectRatio;
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

MarkerUnits SVGMarkerElement::markerUnits() const {
  return handle_.get_or_emplace<components::MarkerComponent>().markerUnits;
}

void SVGMarkerElement::setMarkerUnits(MarkerUnits value) {
  handle_.get_or_emplace<components::MarkerComponent>().markerUnits = value;
}

void SVGMarkerElement::setOrient(MarkerOrient value) {
  handle_.get_or_emplace<components::MarkerComponent>().orient = value;
}

MarkerOrient SVGMarkerElement::orient() const {
  return handle_.get<components::MarkerComponent>().orient;
}

}  // namespace donner::svg
