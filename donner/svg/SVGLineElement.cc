#include "donner/svg/SVGLineElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/LineComponent.h"

namespace donner::svg {

SVGLineElement SVGLineElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  return SVGLineElement(handle);
}

void SVGLineElement::setX1(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();
  handle_.get_or_emplace<components::LineComponent>(access).x1 = value;
}

void SVGLineElement::setY1(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();
  handle_.get_or_emplace<components::LineComponent>(access).y1 = value;
}

void SVGLineElement::setX2(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();
  handle_.get_or_emplace<components::LineComponent>(access).x2 = value;
}

void SVGLineElement::setY2(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();
  handle_.get_or_emplace<components::LineComponent>(access).y2 = value;
}

Lengthd SVGLineElement::x1() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::LineComponent>();
  return component ? component->x1 : Lengthd();
}

Lengthd SVGLineElement::y1() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::LineComponent>();
  return component ? component->y1 : Lengthd();
}

Lengthd SVGLineElement::x2() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::LineComponent>();
  return component ? component->x2 : Lengthd();
}

Lengthd SVGLineElement::y2() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::LineComponent>();
  return component ? component->y2 : Lengthd();
}

void SVGLineElement::invalidate() const {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.remove<components::ComputedPathComponent>(access);
}

}  // namespace donner::svg
