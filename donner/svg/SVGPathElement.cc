#include "donner/svg/SVGPathElement.h"

#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"

namespace donner::svg {

SVGPathElement SVGPathElement::Create(SVGDocument& document) {
  EntityHandle handle = CreateEntity(document.registry(), Tag, Type);
  handle.emplace<components::RenderingBehaviorComponent>(
      components::RenderingBehavior::NoTraverseChildren);
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
}

std::optional<PathSpline> SVGPathElement::computedSpline() const {
  compute();
  if (const auto* computedPath = handle_.try_get<components::ComputedPathComponent>()) {
    return computedPath->spline;
  } else {
    return std::nullopt;
  }
}

void SVGPathElement::invalidate() const {
  handle_.remove<components::ComputedPathComponent>();
}

void SVGPathElement::compute() const {
  auto& path = handle_.get_or_emplace<components::PathComponent>();
  components::ShapeSystem().createComputedPath(handle_, path, FontMetrics(), nullptr);
}

}  // namespace donner::svg
