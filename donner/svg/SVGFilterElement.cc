#include "donner/svg/SVGFilterElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {
namespace {

Lengthd DefaultFilterX() {
  return Lengthd(-10.0, Lengthd::Unit::Percent);
}

Lengthd DefaultFilterY() {
  return Lengthd(-10.0, Lengthd::Unit::Percent);
}

Lengthd DefaultFilterWidth() {
  return Lengthd(120.0, Lengthd::Unit::Percent);
}

Lengthd DefaultFilterHeight() {
  return Lengthd(120.0, Lengthd::Unit::Percent);
}

void InvalidateComputedFilters(EntityHandle handle) {
  Registry& registry = *handle.registry();
  registry.clear<components::ComputedFilterComponent>();
  components::RenderingContext(registry).invalidateRenderTree();
}

}  // namespace

SVGFilterElement SVGFilterElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
  handle.emplace<components::FilterComponent>();
  return SVGFilterElement(handle);
}

Lengthd SVGFilterElement::x() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->x.value_or(DefaultFilterX()) : DefaultFilterX();
}

Lengthd SVGFilterElement::y() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->y.value_or(DefaultFilterY()) : DefaultFilterY();
}

Lengthd SVGFilterElement::width() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->width.value_or(DefaultFilterWidth()) : DefaultFilterWidth();
}

Lengthd SVGFilterElement::height() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->height.value_or(DefaultFilterHeight()) : DefaultFilterHeight();
}

std::optional<RcString> SVGFilterElement::href() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FilterComponent>();
  const std::optional<Reference> maybeHref = component ? component->href : std::nullopt;
  if (maybeHref.has_value()) {
    return maybeHref->href;
  } else {
    return std::nullopt;
  }
}

void SVGFilterElement::setX(const Lengthd& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::FilterComponent>().x = value;
  InvalidateComputedFilters(handle_);
  access.bumpMutationRevision();
}

void SVGFilterElement::setY(const Lengthd& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::FilterComponent>().y = value;
  InvalidateComputedFilters(handle_);
  access.bumpMutationRevision();
}

void SVGFilterElement::setWidth(const Lengthd& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::FilterComponent>().width = value;
  InvalidateComputedFilters(handle_);
  access.bumpMutationRevision();
}

void SVGFilterElement::setHeight(const Lengthd& value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::FilterComponent>().height = value;
  InvalidateComputedFilters(handle_);
  access.bumpMutationRevision();
}

void SVGFilterElement::setHref(OptionalRef<RcStringOrRef> value) {
  DocumentWriteAccess access = handle_.writeAccess();
  if (value) {
    handle_.get_or_emplace<components::FilterComponent>().href = Reference(RcString(value.value()));
  } else {
    handle_.get_or_emplace<components::FilterComponent>().href = std::nullopt;
  }

  InvalidateComputedFilters(handle_);
  access.bumpMutationRevision();
}

FilterUnits SVGFilterElement::filterUnits() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->filterUnits.value_or(FilterUnits::Default) : FilterUnits::Default;
}

void SVGFilterElement::setFilterUnits(FilterUnits value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::FilterComponent>().filterUnits = value;
  InvalidateComputedFilters(handle_);
  access.bumpMutationRevision();
}

PrimitiveUnits SVGFilterElement::primitiveUnits() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->primitiveUnits.value_or(PrimitiveUnits::Default)
                   : PrimitiveUnits::Default;
}

void SVGFilterElement::setPrimitiveUnits(PrimitiveUnits value) {
  DocumentWriteAccess access = handle_.writeAccess();
  handle_.get_or_emplace<components::FilterComponent>().primitiveUnits = value;
  InvalidateComputedFilters(handle_);
  access.bumpMutationRevision();
}

}  // namespace donner::svg
