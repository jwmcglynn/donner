#include "donner/svg/SVGPathElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/shape/PathComponent.h"

namespace donner::svg {

SVGPathElement SVGPathElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
  handle.emplace<components::PathComponent>();
  return SVGPathElement(handle);
}

RcString SVGPathElement::d() const {
  if (const auto* path = handle_.try_get<components::PathComponent>()) {
    if (auto maybeD = path->d.get()) {
      return maybeD.value();
    }
  }

  return "";
}

void SVGPathElement::setD(RcString d) {
  invalidate();

  auto& path = handle_.get_or_emplace<components::PathComponent>();
  path.d.set(d, css::Specificity::Override());
  path.splineOverride.reset();
}

void SVGPathElement::setSpline(const PathSpline& spline) {
  invalidate();

  auto& path = handle_.get_or_emplace<components::PathComponent>();
  path.d.clear();
  path.splineOverride = spline;
}

}  // namespace donner::svg
