#include "donner/svg/SVGFilterPrimitiveStandardAttributes.h"

#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg {
namespace {

Lengthd DefaultFilterPrimitiveX() {
  return Lengthd(-10.0, Lengthd::Unit::Percent);
}

Lengthd DefaultFilterPrimitiveY() {
  return Lengthd(-10.0, Lengthd::Unit::Percent);
}

Lengthd DefaultFilterPrimitiveWidth() {
  return Lengthd(120.0, Lengthd::Unit::Percent);
}

Lengthd DefaultFilterPrimitiveHeight() {
  return Lengthd(120.0, Lengthd::Unit::Percent);
}

}  // namespace

SVGFilterPrimitiveStandardAttributes::SVGFilterPrimitiveStandardAttributes(EntityHandle handle)
    : SVGElement(handle) {
  handle.emplace<components::FilterPrimitiveComponent>();
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::Nonrenderable);
}

void SVGFilterPrimitiveStandardAttributes::invalidateFilter() const {
  [[maybe_unused]] DocumentWriteAccess access = handle_.writeAccess();
  Registry& registry = *handle_.registry();
  registry.clear<components::ComputedFilterComponent>();
  components::RenderingContext(registry).invalidateRenderTree();
}

Lengthd SVGFilterPrimitiveStandardAttributes::x() const {
  const auto* component = handle_.try_get<components::FilterPrimitiveComponent>();
  return component ? component->x.value_or(DefaultFilterPrimitiveX()) : DefaultFilterPrimitiveX();
}

Lengthd SVGFilterPrimitiveStandardAttributes::y() const {
  const auto* component = handle_.try_get<components::FilterPrimitiveComponent>();
  return component ? component->y.value_or(DefaultFilterPrimitiveY()) : DefaultFilterPrimitiveY();
}

Lengthd SVGFilterPrimitiveStandardAttributes::width() const {
  const auto* component = handle_.try_get<components::FilterPrimitiveComponent>();
  return component ? component->width.value_or(DefaultFilterPrimitiveWidth())
                   : DefaultFilterPrimitiveWidth();
}

Lengthd SVGFilterPrimitiveStandardAttributes::height() const {
  const auto* component = handle_.try_get<components::FilterPrimitiveComponent>();
  return component ? component->height.value_or(DefaultFilterPrimitiveHeight())
                   : DefaultFilterPrimitiveHeight();
}

void SVGFilterPrimitiveStandardAttributes::setX(const Lengthd& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterPrimitiveComponent>(access).x = value;
  invalidateFilter();
}

void SVGFilterPrimitiveStandardAttributes::setY(const Lengthd& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterPrimitiveComponent>(access).y = value;
  invalidateFilter();
}

void SVGFilterPrimitiveStandardAttributes::setWidth(const Lengthd& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterPrimitiveComponent>(access).width = value;
  invalidateFilter();
}

void SVGFilterPrimitiveStandardAttributes::setHeight(const Lengthd& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterPrimitiveComponent>(access).height = value;
  invalidateFilter();
}

std::optional<RcString> SVGFilterPrimitiveStandardAttributes::result() const {
  const auto* component = handle_.try_get<components::FilterPrimitiveComponent>();
  return component ? component->result : std::nullopt;
}

void SVGFilterPrimitiveStandardAttributes::setResult(const RcStringOrRef& value) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  handle_.get_or_emplace<components::FilterPrimitiveComponent>(access).result = value;
  invalidateFilter();
}

}  // namespace donner::svg
