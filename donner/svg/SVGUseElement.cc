#include "donner/svg/SVGUseElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/UseReference.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"

namespace donner::svg {

SVGUseElement SVGUseElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::SizedElementComponent>().applyTranslationForUseElement = true;
  return SVGUseElement(handle);
}

void SVGUseElement::setHref(const RcString& value) {
  // Like the other typed setters, this updates the DOM attribute without editing source text; the
  // stored `href` attribute is what the reference is resolved from.
  (void)setAttributeFromXMLMutation(xml::XMLQualifiedNameRef("href"), value);
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
  InvalidateUseInstance(handle_);
}

void SVGUseElement::setY(Lengthd value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.y.set(
      value, css::Specificity::Override());
  InvalidateUseInstance(handle_);
}

void SVGUseElement::setWidth(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.width.set(
      value, css::Specificity::Override());
  InvalidateUseInstance(handle_);
}

void SVGUseElement::setHeight(std::optional<Lengthd> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::SizedElementComponent>(access).properties.height.set(
      value, css::Specificity::Override());
  InvalidateUseInstance(handle_);
}

Lengthd SVGUseElement::x() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.x.get().value()
                   : components::SizedElementComponent().properties.x.get().value();
}

Lengthd SVGUseElement::y() const {
  const auto* component = handle_.try_get<components::SizedElementComponent>();
  return component ? component->properties.y.get().value()
                   : components::SizedElementComponent().properties.y.get().value();
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
