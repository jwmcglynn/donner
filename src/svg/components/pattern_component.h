#pragma once
/// @file

#include <vector>

#include "src/base/parser/parse_error.h"
#include "src/svg/core/pattern.h"
#include "src/svg/graph/reference.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

/**
 * Parameters for <pattern> elements which are not captured by \ref ViewBoxComponent and \ref
 * SizedElementComponent.
 */
struct PatternComponent {
  std::optional<PatternUnits> patternUnits;
  std::optional<PatternContentUnits> patternContentUnits;
  std::optional<Reference> href;

  void compute(EntityHandle handle);
};

struct ComputedPatternComponent {
  bool initialized = false;
  PatternUnits patternUnits = PatternUnits::Default;
  PatternContentUnits patternContentUnits = PatternContentUnits::Default;

  void initialize(EntityHandle handle);

  void inheritAttributes(EntityHandle handle, EntityHandle base = EntityHandle());
};

void EvaluateConditionalPatternShadowTrees(Registry& registry);

void InstantiatePatternComponents(Registry& registry, std::vector<ParseError>* outWarnings);

}  // namespace donner::svg
