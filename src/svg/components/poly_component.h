#pragma once
/// @file

#include <vector>

#include "src/base/vector2.h"
#include "src/svg/components/computed_path_component.h"
#include "src/svg/components/computed_style_component.h"

namespace donner::svg {

/**
 * Parameters for a <polygon> or <polyline> element.
 */
struct PolyComponent {
  enum class Type { Polygon, Polyline };

  explicit PolyComponent(Type type) : type(type) {}

  Type type;
  std::vector<Vector2d> points;

  void computePathWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                       const FontMetrics& fontMetrics,
                                       std::vector<ParseError>* outWarnings) {
    PathSpline::Builder builder;
    if (!points.empty()) {
      builder.moveTo(points[0]);
    }

    for (size_t i = 1; i < points.size(); ++i) {
      builder.lineTo(points[i]);
    }

    if (type == Type::Polygon) {
      builder.closePath();
    }

    handle.emplace_or_replace<ComputedPathComponent>(builder.build());
  }

  void computePath(EntityHandle handle, const FontMetrics& fontMetrics) {
    ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
    style.computeProperties(handle);

    return computePathWithPrecomputedStyle(handle, style, fontMetrics, nullptr);
  }
};

void InstantiatePolyComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg
