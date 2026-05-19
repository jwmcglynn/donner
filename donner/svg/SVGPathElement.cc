#include "donner/svg/SVGPathElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/shape/PathComponent.h"

namespace donner::svg {

SVGPathElement SVGPathElement::CreateOn(EntityHandle handle) {
  CreateEntityOn(handle, Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  handle.emplace<components::PathComponent>();
  return SVGPathElement(handle);
}

RcString SVGPathElement::d() const {
  [[maybe_unused]] DocumentReadAccess access = handle_.readAccess();
  if (const auto* path = handle_.try_get<components::PathComponent>()) {
    if (auto maybeD = path->d.get()) {
      return maybeD.value();
    }
  }

  return "";
}

void SVGPathElement::setD(RcString d) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();

  auto& path = handle_.get_or_emplace<components::PathComponent>(access);
  path.d.set(d, css::Specificity::Override());
  path.splineOverride.reset();
}

void SVGPathElement::setSpline(const Path& spline) {
  DocumentMutationBatch mutation = handle_.mutationBatch();
  DocumentWriteAccess& access = mutation.access();
  invalidate();

  auto& path = handle_.get_or_emplace<components::PathComponent>(access);
  path.d.clear();
  path.splineOverride = spline;
}

}  // namespace donner::svg
