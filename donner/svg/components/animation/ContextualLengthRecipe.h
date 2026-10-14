#pragma once
/// @file

#include <cstddef>
#include <memory>
#include <utility>
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
  /// Validate owned operands and derive immutable storage/work costs; reject excessive input.
  /// @param samples Owned sample stack, consumed only after its shape has been checked.
  /// @return Immutable admitted recipe, or nullptr when its source/storage envelope is exceeded.
  static std::shared_ptr<const ContextualLengthRecipe> Create(
      std::vector<ContextualLengthSample>&& samples);

  const std::vector<ContextualLengthSample> samples;  ///< Lower to higher sandwich layers.
  const std::size_t retainedBytes;                    ///< Conservative admitted storage.
  const std::size_t evaluationWork;                   ///< Cost charged before consumer evaluation.

private:
  ContextualLengthRecipe(std::vector<ContextualLengthSample>&& values, std::size_t bytes,
                         std::size_t work)
      : samples(std::move(values)), retainedBytes(bytes), evaluationWork(work) {}
};

}  // namespace donner::svg::components
