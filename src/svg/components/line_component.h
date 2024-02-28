#pragma once
/// @file

#include "src/base/length.h"
#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/computed_style_component.h"

namespace donner::svg::components {

/**
 * Parameters for a \ref xml_line element.
 */
struct LineComponent {
  Lengthd x1;
  Lengthd y1;
  Lengthd x2;
  Lengthd y2;

  void computePathWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                       const FontMetrics& fontMetrics,
                                       std::vector<ParseError>* outWarnings) {
    const Vector2d start(x1.toPixels(style.viewbox(), fontMetrics),
                         y1.toPixels(style.viewbox(), fontMetrics));
    const Vector2d end(x2.toPixels(style.viewbox(), fontMetrics),
                       y2.toPixels(style.viewbox(), fontMetrics));

    handle.emplace_or_replace<ComputedPathComponent>(
        PathSpline::Builder().moveTo(start).lineTo(end).build());
  }

  void computePath(EntityHandle handle, const FontMetrics& fontMetrics) {
    ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
    style.computeProperties(handle);

    return computePathWithPrecomputedStyle(handle, style, fontMetrics, nullptr);
  }
};

void InstantiateLineComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg::components
