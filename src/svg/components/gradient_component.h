#pragma once

#include <vector>

#include "src/base/parser/parse_error.h"
#include "src/base/transform.h"
#include "src/svg/core/gradient.h"
#include "src/svg/graph/reference.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

/**
 * Common parameters for gradient elements, <linearGradient> and <radialGradient>
 */
struct GradientComponent {
  std::optional<GradientUnits> gradientUnits;
  std::optional<GradientSpreadMethod> spreadMethod;
  std::optional<Reference> href;

  void compute(EntityHandle handle);
};

struct ComputedGradientComponent {
  bool initialized = false;
  GradientUnits gradientUnits = GradientUnits::Default;
  GradientSpreadMethod spreadMethod = GradientSpreadMethod::Default;
  std::vector<GradientStop> stops;

  void initialize(EntityHandle handle);

  void inheritAttributes(EntityHandle handle, EntityHandle base = EntityHandle());
};

void EvaluateConditionalGradientShadowTrees(Registry& registry);

void InstantiateGradientComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg
