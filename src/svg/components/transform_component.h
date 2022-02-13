#pragma once

#include "src/base/transform.h"
#include "src/svg/components/registry.h"
#include "src/svg/core/css_transform.h"
#include "src/svg/properties/property.h"

namespace donner::svg {

struct ComputedStyleComponent;

struct TransformComponent {
  Property<CssTransform> transform{"transform",
                                   []() -> std::optional<CssTransform> { return std::nullopt; }};

  void computeWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                   const FontMetrics& fontMetrics);

  void compute(EntityHandle handle, const FontMetrics& fontMetrics);
};

struct ComputedTransformComponent {
  Transformd transform;
};

struct ViewboxTransformComponent {
  Transformd transform;
};

void computeAllTransforms(Registry& registry);

}  // namespace donner::svg
