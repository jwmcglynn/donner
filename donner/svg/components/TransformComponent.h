#pragma once
/// @file

#include "donner/base/Transform.h"
#include "donner/svg/core/CssTransform.h"
#include "donner/svg/properties/Property.h"
#include "donner/svg/registry/Registry.h"

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
                                   std::vector<parser::ParseError>* outWarnings);
};

void ComputeAllTransforms(Registry& registry, std::vector<parser::ParseError>* outWarnings);

}  // namespace donner::svg::components
