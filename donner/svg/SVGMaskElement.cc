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
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MaskComponent>(access).maskUnits = value;
  InvalidateMask(handle_);
}

MaskContentUnits SVGMaskElement::maskContentUnits() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::MaskComponent>();
  return component ? component->maskContentUnits : components::MaskComponent().maskContentUnits;
}

void SVGMaskElement::setMaskContentUnits(MaskContentUnits value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MaskComponent>(access).maskContentUnits = value;
  InvalidateMask(handle_);
}

void SVGMaskElement::setX(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MaskComponent>(access).x = value;
  InvalidateMask(handle_);
}

void SVGMaskElement::setY(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MaskComponent>(access).y = value;
  InvalidateMask(handle_);
}

void SVGMaskElement::setWidth(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MaskComponent>(access).width = value;
  InvalidateMask(handle_);
}

void SVGMaskElement::setHeight(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::MaskComponent>(access).height = value;
  InvalidateMask(handle_);
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
