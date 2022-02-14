#pragma once

#include <optional>

#include "src/base/length.h"
#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/properties/presentation_attribute_parsing.h"
#include "src/svg/properties/property.h"

namespace donner::svg {

/**
 * Parameters for a <rect> element.
 */
struct RectProperties {
  Property<Lengthd> x{"x",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> y{"y",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> width{
      "width", []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> height{
      "height", []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> rx{"rx", []() -> std::optional<Lengthd> { return std::nullopt; }};
  Property<Lengthd> ry{"ry", []() -> std::optional<Lengthd> { return std::nullopt; }};

  auto allProperties() { return std::forward_as_tuple(x, y, width, height, rx, ry); }

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

struct ComputedRectComponent {
  ComputedRectComponent(const RectProperties& inputProperties,
                        const std::map<RcString, UnparsedProperty>& unparsedProperties,
                        std::vector<ParseError>* outWarnings);

  RectProperties properties;
};

struct RectComponent {
  RectProperties properties;

  void computePathWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                       const FontMetrics& fontMetrics,
                                       std::vector<ParseError>* outWarnings);

  void computePath(EntityHandle handle, const FontMetrics& fontMetrics) {
    ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
    style.computeProperties(handle);

    return computePathWithPrecomputedStyle(handle, style, fontMetrics, nullptr);
  }
};

void InstantiateComputedRectComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg
