#pragma once
/// @file

#include "src/base/transform.h"
#include "src/svg/core/css_transform.h"
#include "src/svg/properties/property.h"
#include "src/svg/registry/registry.h"

namespace donner::svg::components {

struct ComputedStyleComponent;

struct ComputedTransformComponent {
  Transformd transform;
  CssTransform rawCssTransform;
};

struct TransformComponent {
  Property<CssTransform> transform{"transform",
                                   []() -> std::optional<CssTransform> { return std::nullopt; }};

  void computeWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                   const FontMetrics& fontMetrics,
                                   std::vector<ParseError>* outWarnings);
};

void ComputeAllTransforms(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg::components
