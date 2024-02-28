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

  void compute(EntityHandle handle, const FontMetrics& fontMetrics);

  /**
   * Get the computed transform, computing it if necessary. If the transform is not set, returns
   * nullptr, the caller should substitute the result for identity in this case.
   *
   * @param handle Entity handle.
   * @param fontMetrics Font metrics used for resolving lengths in the <transform-list>.
   * @return const ComputedTransformComponent* The computed transform component, or nullptr if the
   *  transform is not set.
   */
  static const ComputedTransformComponent* ComputedTransform(EntityHandle handle,
                                                             const FontMetrics& fontMetrics);
};

void ComputeAllTransforms(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg::components
