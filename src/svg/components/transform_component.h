#pragma once

#include "src/base/transform.h"
#include "src/svg/core/css_transform.h"
#include "src/svg/properties/property.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

struct ComputedStyleComponent;

struct TransformComponent {
  Property<CssTransform> transform{"transform",
                                   []() -> std::optional<CssTransform> { return std::nullopt; }};

  void computeWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                   const FontMetrics& fontMetrics,
                                   std::vector<ParseError>* outWarnings);

  void compute(EntityHandle handle, const FontMetrics& fontMetrics);
};

struct ComputedTransformComponent {
  Transformd transform;
};

void ComputeAllTransforms(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg
