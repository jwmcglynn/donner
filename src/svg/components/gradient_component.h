#pragma once

#include <vector>

#include "src/base/parser/parse_error.h"
#include "src/base/transform.h"
#include "src/svg/core/gradient.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

/**
 * Common parameters for gradient elements, <linearGradient> and <radialGradient>
 */
struct GradientComponent {
  GradientUnits gradientUnits = GradientUnits::Default;
  GradientSpreadMethod spreadMethod = GradientSpreadMethod::Default;
};

struct ComputedGradientComponent {
  std::vector<GradientStop> stops;
};

void InstantiateGradientComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg
