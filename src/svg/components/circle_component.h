#pragma once

#include "src/base/length.h"
#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/properties/property.h"

namespace donner::svg {

struct CircleProperties {
  Property<Lengthd> cx{"cx",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> cy{"cy",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> r{"r",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};

  auto allProperties() { return std::forward_as_tuple(cx, cy, r); }
};

struct ComputedCircleComponent {
  ComputedCircleComponent(const CircleProperties& inputProperties,
                          const std::map<RcString, UnparsedProperty>& unparsedProperties);

  CircleProperties properties;
};

/**
 * Parameters for a <circle> element.
 */
struct CircleComponent {
  CircleProperties properties;

  void computePathWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                       const FontMetrics& fontMetrics) {
    const ComputedCircleComponent& computedCircle = handle.get_or_emplace<ComputedCircleComponent>(
        properties, style.properties().unparsedProperties);

    const Vector2d center(
        computedCircle.properties.cx.getRequired().toPixels(style.viewbox(), fontMetrics),
        computedCircle.properties.cy.getRequired().toPixels(style.viewbox(), fontMetrics));
    const double radius =
        computedCircle.properties.r.getRequired().toPixels(style.viewbox(), fontMetrics);

    handle.emplace_or_replace<ComputedPathComponent>().setSpline(
        PathSpline::Builder().circle(center, radius).build());
  }

  void computePath(EntityHandle handle, const FontMetrics& fontMetrics) {
    ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
    style.computeProperties(*handle.registry(), handle.entity());

    return computePathWithPrecomputedStyle(handle, style, fontMetrics);
  }
};

}  // namespace donner::svg
