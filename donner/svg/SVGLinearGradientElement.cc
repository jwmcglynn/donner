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
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::LinearGradientComponent>(access).x1 = value;
}

void SVGLinearGradientElement::setY1(std::optional<Lengthd> value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::LinearGradientComponent>(access).y1 = value;
}

void SVGLinearGradientElement::setX2(std::optional<Lengthd> value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::LinearGradientComponent>(access).x2 = value;
}

void SVGLinearGradientElement::setY2(std::optional<Lengthd> value) {
  auto mutation = mutationScope([this]() { invalidate(); });
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::LinearGradientComponent>(access).y2 = value;
}

std::optional<Lengthd> SVGLinearGradientElement::x1() const {
  const auto* component = handle_.try_get<components::LinearGradientComponent>();
  return component ? component->x1 : std::nullopt;
}

std::optional<Lengthd> SVGLinearGradientElement::y1() const {
  const auto* component = handle_.try_get<components::LinearGradientComponent>();
  return component ? component->y1 : std::nullopt;
}

std::optional<Lengthd> SVGLinearGradientElement::x2() const {
  const auto* component = handle_.try_get<components::LinearGradientComponent>();
  return component ? component->x2 : std::nullopt;
}

std::optional<Lengthd> SVGLinearGradientElement::y2() const {
  const auto* component = handle_.try_get<components::LinearGradientComponent>();
  return component ? component->y2 : std::nullopt;
}

}  // namespace donner::svg
