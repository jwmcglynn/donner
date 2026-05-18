#include "donner/svg/SVGImageElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/PreserveAspectRatioComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {
namespace {

void InvalidateImage(EntityHandle handle) {
  components::LayoutSystem().invalidate(handle);
  handle.remove<components::LoadedImageComponent, components::LoadedSVGImageComponent>();
  components::RenderingContext(*handle.registry()).invalidateRenderTree();
}

}  // namespace

SVGImageElement SVGImageElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  handle.emplace<components::SizedElementComponent>();
  handle.emplace<components::PreserveAspectRatioComponent>();
  return SVGImageElement(handle);
}

void SVGImageElement::setHref(RcStringOrRef value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::ImageComponent>().href = RcString(value);
  InvalidateImage(handle_);
  access.bumpMutationRevision();
}

RcString SVGImageElement::href() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::ImageComponent>();
  return component ? component->href : "";
}

void SVGImageElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>().preserveAspectRatio =
      preserveAspectRatio;
  InvalidateImage(handle_);
  access.bumpMutationRevision();
}

PreserveAspectRatio SVGImageElement::preserveAspectRatio() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::PreserveAspectRatioComponent>();
  return component ? component->preserveAspectRatio : PreserveAspectRatio();
}

void SVGImageElement::setX(Lengthd value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::SizedElementComponent>().properties.x.set(
      value, css::Specificity::Override());
  InvalidateImage(handle_);
  access.bumpMutationRevision();
}

void SVGImageElement::setY(Lengthd value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::SizedElementComponent>().properties.y.set(
      value, css::Specificity::Override());
  InvalidateImage(handle_);
  access.bumpMutationRevision();
}

void SVGImageElement::setWidth(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::SizedElementComponent>().properties.width.set(
      value, css::Specificity::Override());
  InvalidateImage(handle_);
  access.bumpMutationRevision();
}

void SVGImageElement::setHeight(std::optional<Lengthd> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::SizedElementComponent>().properties.height.set(
      value, css::Specificity::Override());
  InvalidateImage(handle_);
  access.bumpMutationRevision();
}

Lengthd SVGImageElement::x() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.x.getRequired()
                   : components::SizedElementComponent().properties.x.getRequired();
}

Lengthd SVGImageElement::y() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.y.getRequired()
                   : components::SizedElementComponent().properties.y.getRequired();
}

std::optional<Lengthd> SVGImageElement::width() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.width.get() : std::nullopt;
}

std::optional<Lengthd> SVGImageElement::height() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.height.get() : std::nullopt;
}

}  // namespace donner::svg
