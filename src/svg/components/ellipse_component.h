#pragma once

#include "src/base/length.h"
#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/properties/property.h"

namespace donner::svg {

struct EllipseProperties {
  Property<Lengthd> cx{"cx",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> cy{"cy",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> rx{"rx", []() -> std::optional<Lengthd> { return std::nullopt; }};
  Property<Lengthd> ry{"ry", []() -> std::optional<Lengthd> { return std::nullopt; }};

  auto allProperties() { return std::forward_as_tuple(cx, cy, rx, ry); }

  Lengthd calculateRx() const {
    if (rx.hasValue()) {
      return rx.getRequired();
    } else {
      return ry.hasValue() ? ry.getRequired() : Lengthd(0, Lengthd::Unit::None);
    }
  }

  Lengthd calculateRy() const {
    if (ry.hasValue()) {
      return ry.getRequired();
    } else {
      return rx.hasValue() ? rx.getRequired() : Lengthd(0, Lengthd::Unit::None);
    }
  }
};

struct ComputedEllipseComponent {
  ComputedEllipseComponent(const EllipseProperties& inputProperties,
                           const std::map<RcString, UnparsedProperty>& unparsedProperties,
                           std::vector<ParseError>* outWarnings);

  EllipseProperties properties;
};

/**
 * Parameters for a <ellipse> element.
 */
struct EllipseComponent {
  EllipseProperties properties;

  void computePathWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                       const FontMetrics& fontMetrics,
                                       std::vector<ParseError>* outWarnings) {
    const ComputedEllipseComponent& computedEllipse =
        handle.get_or_emplace<ComputedEllipseComponent>(
            properties, style.properties().unparsedProperties, outWarnings);

    const Vector2d center(
        computedEllipse.properties.cx.getRequired().toPixels(style.viewbox(), fontMetrics),
        computedEllipse.properties.cy.getRequired().toPixels(style.viewbox(), fontMetrics));
    const Vector2d radius(
        computedEllipse.properties.calculateRx().toPixels(style.viewbox(), fontMetrics),
        computedEllipse.properties.calculateRy().toPixels(style.viewbox(), fontMetrics));

    handle.emplace_or_replace<ComputedPathComponent>(
        PathSpline::Builder().ellipse(center, radius).build());
  }

  void computePath(EntityHandle handle, const FontMetrics& fontMetrics) {
    ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
    style.computeProperties(handle);

    return computePathWithPrecomputedStyle(handle, style, fontMetrics, nullptr);
  }
};

void InstantiateComputedEllipseComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg
