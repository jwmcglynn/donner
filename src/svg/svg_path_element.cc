#include "src/svg/svg_path_element.h"

#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/svg_document.h"

namespace donner::svg {

SVGPathElement SVGPathElement::Create(SVGDocument& document) {
  Registry& registry = document.registry();
  return SVGPathElement(CreateEntity(registry, RcString(Tag), Type));
}

RcString SVGPathElement::d() const {
  if (const auto* path = handle_.try_get<PathComponent>()) {
    if (auto maybeD = path->d.get()) {
      return maybeD.value();
    }
  }

  return "";
}

std::optional<ParseError> SVGPathElement::setD(RcString d) {
  invalidate();

  auto& path = handle_.get_or_emplace<PathComponent>();
  path.d.set(d, css::Specificity::Override());

  // TODO: Change return type.
  return std::nullopt;
}

std::optional<double> SVGPathElement::pathLength() const {
  return handle_.get_or_emplace<PathComponent>().userPathLength;
}

void SVGPathElement::setPathLength(std::optional<double> value) {
  handle_.get_or_emplace<PathComponent>().userPathLength = value;
}

std::optional<PathSpline> SVGPathElement::computedSpline() const {
  compute();
  if (const auto* computedPath = handle_.try_get<ComputedPathComponent>()) {
    return computedPath->spline;
  } else {
    return std::nullopt;
  }
}

void SVGPathElement::invalidate() const {
  handle_.remove<ComputedPathComponent>();
}

void SVGPathElement::compute() const {
  auto& path = handle_.get_or_emplace<PathComponent>();
  path.computePath(handle_);
}

}  // namespace donner::svg
