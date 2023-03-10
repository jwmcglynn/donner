#pragma once
/// @file

#include "src/base/length.h"
#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/properties/property.h"
#include "src/svg/properties/rx_ry_properties.h"

namespace donner::svg {

/**
 * Parameters for a <ellipse> element.
 */
struct EllipseProperties {
  Property<Lengthd> cx{"cx",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> cy{"cy",
                       []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> rx{"rx", []() -> std::optional<Lengthd> { return std::nullopt; }};
  Property<Lengthd> ry{"ry", []() -> std::optional<Lengthd> { return std::nullopt; }};

  auto allProperties() { return std::forward_as_tuple(cx, cy, rx, ry); }

  std::tuple<Lengthd, double> calculateRx(const Boxd& viewbox,
                                          const FontMetrics& fontMetrics) const {
    return CalculateRadiusMaybeAuto(rx, ry, viewbox, fontMetrics);
  }

  std::tuple<Lengthd, double> calculateRy(const Boxd& viewbox,
                                          const FontMetrics& fontMetrics) const {
    return CalculateRadiusMaybeAuto(ry, rx, viewbox, fontMetrics);
  }
};

struct ComputedEllipseComponent {
  ComputedEllipseComponent(const EllipseProperties& inputProperties,
                           const std::map<RcString, UnparsedProperty>& unparsedProperties,
                           std::vector<ParseError>* outWarnings);

  EllipseProperties properties;
};

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
        std::get<1>(computedEllipse.properties.calculateRx(style.viewbox(), fontMetrics)),
        std::get<1>(computedEllipse.properties.calculateRy(style.viewbox(), fontMetrics)));

    if (radius.x > 0.0 && radius.y > 0.0) {
      handle.emplace_or_replace<ComputedPathComponent>(
          PathSpline::Builder().ellipse(center, radius).build());
    }
  }

  void computePath(EntityHandle handle, const FontMetrics& fontMetrics) {
    ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
    style.computeProperties(handle);

    return computePathWithPrecomputedStyle(handle, style, fontMetrics, nullptr);
  }
};

void InstantiateComputedEllipseComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg
