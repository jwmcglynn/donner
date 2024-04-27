#include "src/svg/svg_path_element.h"

#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/components/rendering_behavior_component.h"
#include "src/svg/svg_document.h"

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
  path.computePath(handle_);
}

}  // namespace donner::svg
