#include "donner/svg/SVGMarkerElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/ViewBoxComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {
namespace {

void InvalidateMarker(EntityHandle handle) {
  components::LayoutSystem().invalidate(handle);
  components::RenderingContext(*handle.registry()).invalidateRenderTree();
}

}  // namespace

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

void SVGMarkerElement::setViewBox(OptionalRef<Box2d> viewBox) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::ViewBoxComponent>(access).viewBox = viewBox;
  InvalidateMarker(handle_);
}

std::optional<Box2d> SVGMarkerElement::viewBox() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::ViewBoxComponent>();
  return component ? component->viewBox : std::nullopt;
}

void SVGMarkerElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>(access).preserveAspectRatio =
      preserveAspectRatio;
  InvalidateMarker(handle_);
}

PreserveAspectRatio SVGMarkerElement::preserveAspectRatio() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PreserveAspectRatioComponent>();
  return component ? component->preserveAspectRatio : PreserveAspectRatio();
}

void SVGMarkerElement::setMarkerWidth(double value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MarkerComponent>(access).markerWidth = value;
  InvalidateMarker(handle_);
}

double SVGMarkerElement::markerWidth() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MarkerComponent>();
  return component ? component->markerWidth : components::MarkerComponent().markerWidth;
}

void SVGMarkerElement::setMarkerHeight(double value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MarkerComponent>(access).markerHeight = value;
  InvalidateMarker(handle_);
}

double SVGMarkerElement::markerHeight() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MarkerComponent>();
  return component ? component->markerHeight : components::MarkerComponent().markerHeight;
}

void SVGMarkerElement::setRefX(double value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MarkerComponent>(access).refX = value;
  InvalidateMarker(handle_);
}

double SVGMarkerElement::refX() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MarkerComponent>();
  return component ? component->refX : components::MarkerComponent().refX;
}

void SVGMarkerElement::setRefY(double value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MarkerComponent>(access).refY = value;
  InvalidateMarker(handle_);
}

double SVGMarkerElement::refY() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MarkerComponent>();
  return component ? component->refY : components::MarkerComponent().refY;
}

MarkerUnits SVGMarkerElement::markerUnits() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MarkerComponent>();
  return component ? component->markerUnits : components::MarkerComponent().markerUnits;
}

void SVGMarkerElement::setMarkerUnits(MarkerUnits value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MarkerComponent>(access).markerUnits = value;
  InvalidateMarker(handle_);
}

void SVGMarkerElement::setOrient(MarkerOrient value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MarkerComponent>(access).orient = value;
  InvalidateMarker(handle_);
}

MarkerOrient SVGMarkerElement::orient() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MarkerComponent>();
  return component ? component->orient : components::MarkerComponent().orient;
}

}  // namespace donner::svg
