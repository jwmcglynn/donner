#include "donner/svg/SVGLinearGradientElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"

namespace donner::svg {

SVGLinearGradientElement SVGLinearGradientElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::LinearGradientComponent>();
  return SVGLinearGradientElement(handle);
}

void SVGLinearGradientElement::setX1(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.get_or_emplace<components::LinearGradientComponent>().x1 = value;
  access.bumpMutationRevision();
}

void SVGLinearGradientElement::setY1(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.get_or_emplace<components::LinearGradientComponent>().y1 = value;
  access.bumpMutationRevision();
}

void SVGLinearGradientElement::setX2(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.get_or_emplace<components::LinearGradientComponent>().x2 = value;
  access.bumpMutationRevision();
}

void SVGLinearGradientElement::setY2(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.get_or_emplace<components::LinearGradientComponent>().y2 = value;
  access.bumpMutationRevision();
}

std::optional<Lengthd> SVGLinearGradientElement::x1() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::LinearGradientComponent>();
  return component ? component->x1 : std::nullopt;
}

std::optional<Lengthd> SVGLinearGradientElement::y1() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::LinearGradientComponent>();
  return component ? component->y1 : std::nullopt;
}

std::optional<Lengthd> SVGLinearGradientElement::x2() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::LinearGradientComponent>();
  return component ? component->x2 : std::nullopt;
}

std::optional<Lengthd> SVGLinearGradientElement::y2() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::LinearGradientComponent>();
  return component ? component->y2 : std::nullopt;
}

}  // namespace donner::svg
