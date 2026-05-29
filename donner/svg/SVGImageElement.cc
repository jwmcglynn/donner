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
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::ImageComponent>(access).href = RcString(value);
  InvalidateImage(handle_);
}

RcString SVGImageElement::href() const {
  const auto* component = handle_.try_get<components::ImageComponent>();
  return component ? component->href : "";
}

void SVGImageElement::setPreserveAspectRatio(PreserveAspectRatio preserveAspectRatio) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::PreserveAspectRatioComponent>(access).preserveAspectRatio =
      preserveAspectRatio;
  InvalidateImage(handle_);
}

PreserveAspectRatio SVGImageElement::preserveAspectRatio() const {
  const auto* component = handle_.try_get<components::PreserveAspectRatioComponent>();
  return component ? component->preserveAspectRatio : PreserveAspectRatio();
}

void SVGImageElement::setX(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.x.set(
      value, css::Specificity::Override());
  InvalidateImage(handle_);
}

void SVGImageElement::setY(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.y.set(
      value, css::Specificity::Override());
  InvalidateImage(handle_);
}

void SVGImageElement::setWidth(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.width.set(
      value, css::Specificity::Override());
  InvalidateImage(handle_);
}

void SVGImageElement::setHeight(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.height.set(
      value, css::Specificity::Override());
  InvalidateImage(handle_);
}

Lengthd SVGImageElement::x() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.x.getRequired()
                   : components::SizedElementComponent().properties.x.getRequired();
}

Lengthd SVGImageElement::y() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.y.getRequired()
                   : components::SizedElementComponent().properties.y.getRequired();
}

std::optional<Lengthd> SVGImageElement::width() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.width.get() : std::nullopt;
}

std::optional<Lengthd> SVGImageElement::height() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.height.get() : std::nullopt;
}

}  // namespace donner::svg
