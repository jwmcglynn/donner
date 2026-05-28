#include "donner/svg/SVGUseElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {
namespace {

void InvalidateUse(EntityHandle handle) {
  components::LayoutSystem().invalidate(handle);
  handle.remove<components::ComputedShadowTreeComponent, components::ExternalUseComponent>();
  components::RenderingContext(*handle.registry()).invalidateRenderTree();
}

}  // namespace

SVGUseElement SVGUseElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::SizedElementComponent>().applyTranslationForUseElement = true;
  return SVGUseElement(handle);
}

void SVGUseElement::setHref(const RcString& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  auto& shadowTree = handle_.emplace_or_replace<components::ShadowTreeComponent>(access);
  shadowTree.setMainHref(value);
  shadowTree.setsContextColors = true;

  InvalidateUse(handle_);
}

RcString SVGUseElement::href() const {
  if (const auto* component = handle_.try_get<components::ShadowTreeComponent>()) {
    if (auto maybeMainHref = component->mainHref()) {
      return maybeMainHref.value();
    }
  }

  return "";
}

void SVGUseElement::setX(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.x.set(
      value, css::Specificity::Override());
  InvalidateUse(handle_);
}

void SVGUseElement::setY(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.y.set(
      value, css::Specificity::Override());
  InvalidateUse(handle_);
}

void SVGUseElement::setWidth(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.width.set(
      value, css::Specificity::Override());
  InvalidateUse(handle_);
}

void SVGUseElement::setHeight(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.height.set(
      value, css::Specificity::Override());
  InvalidateUse(handle_);
}

Lengthd SVGUseElement::x() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.x.getRequired()
                   : components::SizedElementComponent().properties.x.getRequired();
}

Lengthd SVGUseElement::y() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.y.getRequired()
                   : components::SizedElementComponent().properties.y.getRequired();
}

std::optional<Lengthd> SVGUseElement::width() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.width.get() : std::nullopt;
}

std::optional<Lengthd> SVGUseElement::height() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.height.get() : std::nullopt;
}

}  // namespace donner::svg
