#include "donner/svg/SVGRadialGradientElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"

namespace donner::svg {

SVGRadialGradientElement SVGRadialGradientElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::RadialGradientComponent>();
  return SVGRadialGradientElement(handle);
}

void SVGRadialGradientElement::setCx(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.get_or_emplace<components::RadialGradientComponent>().cx = value;
  access.bumpMutationRevision();
}

void SVGRadialGradientElement::setCy(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.get_or_emplace<components::RadialGradientComponent>().cy = value;
  access.bumpMutationRevision();
}

void SVGRadialGradientElement::setR(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.get_or_emplace<components::RadialGradientComponent>().r = value;
  access.bumpMutationRevision();
}

void SVGRadialGradientElement::setFx(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.get_or_emplace<components::RadialGradientComponent>().fx = value;
  access.bumpMutationRevision();
}

void SVGRadialGradientElement::setFy(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.get_or_emplace<components::RadialGradientComponent>().fy = value;
  access.bumpMutationRevision();
}

void SVGRadialGradientElement::setFr(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  invalidate();
  handle_.get_or_emplace<components::RadialGradientComponent>().fr = value;
  access.bumpMutationRevision();
}

std::optional<Lengthd> SVGRadialGradientElement::cx() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RadialGradientComponent>();
  return component ? component->cx : std::nullopt;
}

std::optional<Lengthd> SVGRadialGradientElement::cy() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RadialGradientComponent>();
  return component ? component->cy : std::nullopt;
}

std::optional<Lengthd> SVGRadialGradientElement::r() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RadialGradientComponent>();
  return component ? component->r : std::nullopt;
}

std::optional<Lengthd> SVGRadialGradientElement::fx() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RadialGradientComponent>();
  return component ? component->fx : std::nullopt;
}

std::optional<Lengthd> SVGRadialGradientElement::fy() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RadialGradientComponent>();
  return component ? component->fy : std::nullopt;
}

std::optional<Lengthd> SVGRadialGradientElement::fr() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::RadialGradientComponent>();
  return component ? component->fr : std::nullopt;
}

}  // namespace donner::svg
