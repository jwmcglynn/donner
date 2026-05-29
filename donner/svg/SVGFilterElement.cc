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
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->x.value_or(DefaultFilterX()) : DefaultFilterX();
}

Lengthd SVGFilterElement::y() const {
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->y.value_or(DefaultFilterY()) : DefaultFilterY();
}

Lengthd SVGFilterElement::width() const {
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->width.value_or(DefaultFilterWidth()) : DefaultFilterWidth();
}

Lengthd SVGFilterElement::height() const {
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->height.value_or(DefaultFilterHeight()) : DefaultFilterHeight();
}

std::optional<RcString> SVGFilterElement::href() const {
  const auto* component = handle_.try_get<components::FilterComponent>();
  const std::optional<Reference> maybeHref = component ? component->href : std::nullopt;
  if (maybeHref.has_value()) {
    return maybeHref->href;
  } else {
    return std::nullopt;
  }
}

void SVGFilterElement::setX(const Lengthd& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterComponent>(access).x = value;
  InvalidateComputedFilters(handle_);
}

void SVGFilterElement::setY(const Lengthd& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterComponent>(access).y = value;
  InvalidateComputedFilters(handle_);
}

void SVGFilterElement::setWidth(const Lengthd& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterComponent>(access).width = value;
  InvalidateComputedFilters(handle_);
}

void SVGFilterElement::setHeight(const Lengthd& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterComponent>(access).height = value;
  InvalidateComputedFilters(handle_);
}

void SVGFilterElement::setHref(OptionalRef<RcStringOrRef> value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  if (value) {
    handle_.get_or_emplace<components::FilterComponent>(access).href =
        Reference(RcString(value.value()));
  } else {
    handle_.get_or_emplace<components::FilterComponent>(access).href = std::nullopt;
  }

  InvalidateComputedFilters(handle_);
}

FilterUnits SVGFilterElement::filterUnits() const {
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->filterUnits.value_or(FilterUnits::Default) : FilterUnits::Default;
}

void SVGFilterElement::setFilterUnits(FilterUnits value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterComponent>(access).filterUnits = value;
  InvalidateComputedFilters(handle_);
}

PrimitiveUnits SVGFilterElement::primitiveUnits() const {
  const auto* component = handle_.try_get<components::FilterComponent>();
  return component ? component->primitiveUnits.value_or(PrimitiveUnits::Default)
                   : PrimitiveUnits::Default;
}

void SVGFilterElement::setPrimitiveUnits(PrimitiveUnits value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterComponent>(access).primitiveUnits = value;
  InvalidateComputedFilters(handle_);
}

}  // namespace donner::svg
