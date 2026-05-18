#include "donner/svg/SVGMaskElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {
namespace {

void InvalidateMask(EntityHandle handle) {
  components::RenderingContext(*handle.registry()).invalidateRenderTree();
}

}  // namespace

SVGMaskElement SVGMaskElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::MaskComponent>();

  auto& renderingBehavior = handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::ShadowOnlyChildren);
  renderingBehavior.inheritsParentTransform = false;
  renderingBehavior.appliesSelfTransform = false;

  return SVGMaskElement(handle);
}

MaskUnits SVGMaskElement::maskUnits() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MaskComponent>();
  return component ? component->maskUnits : components::MaskComponent().maskUnits;
}

void SVGMaskElement::setMaskUnits(MaskUnits value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::MaskComponent>().maskUnits = value;
  InvalidateMask(handle_);
  access.bumpMutationRevision();
}

MaskContentUnits SVGMaskElement::maskContentUnits() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MaskComponent>();
  return component ? component->maskContentUnits : components::MaskComponent().maskContentUnits;
}

void SVGMaskElement::setMaskContentUnits(MaskContentUnits value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::MaskComponent>().maskContentUnits = value;
  InvalidateMask(handle_);
  access.bumpMutationRevision();
}

void SVGMaskElement::setX(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::MaskComponent>().x = value;
  InvalidateMask(handle_);
  access.bumpMutationRevision();
}

void SVGMaskElement::setY(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::MaskComponent>().y = value;
  InvalidateMask(handle_);
  access.bumpMutationRevision();
}

void SVGMaskElement::setWidth(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::MaskComponent>().width = value;
  InvalidateMask(handle_);
  access.bumpMutationRevision();
}

void SVGMaskElement::setHeight(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::MaskComponent>().height = value;
  InvalidateMask(handle_);
  access.bumpMutationRevision();
}

std::optional<Lengthd> SVGMaskElement::x() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MaskComponent>();
  return component ? component->x : std::nullopt;
}

std::optional<Lengthd> SVGMaskElement::y() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MaskComponent>();
  return component ? component->y : std::nullopt;
}

std::optional<Lengthd> SVGMaskElement::width() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MaskComponent>();
  return component ? component->width : std::nullopt;
}

std::optional<Lengthd> SVGMaskElement::height() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MaskComponent>();
  return component ? component->height : std::nullopt;
}

}  // namespace donner::svg
