#pragma once
/// @file

#include <cstddef>
#include <variant>
#include <vector>

#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/SetAnimationComponent.h"

namespace donner::svg::components {

/// An admitted value declaration with its resolved time, independent of document storage.
struct ContextualLengthSample {
  /// Owned declaration operands, admitted before capture.
  std::variant<AnimateValueComponent, SetAnimationComponent> declaration;
  double progress = 0;   ///< Progress in the active iteration.
  double iteration = 0;  ///< Completed iteration count.
};

/// Ordered animation stack retained until the referencing instance's length context is known.
struct ContextualLengthRecipe {
  std::vector<ContextualLengthSample> samples;  ///< Lower to higher sandwich layers.
  std::size_t retainedBytes = 0;   ///< Conservative admitted storage, including capacity.
  std::size_t evaluationWork = 0;  ///< Work charged before each consumer evaluates it.
};

}  // namespace donner::svg::components
