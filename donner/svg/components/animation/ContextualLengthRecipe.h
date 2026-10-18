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

struct ContextualLengthRecipe;

/// An admitted value declaration with its resolved time, independent of document storage.
struct ContextualLengthSample {
  /// Owned declaration operands, admitted before capture.
  std::variant<AnimateValueComponent, SetAnimationComponent> declaration;
  double progress = 0;   ///< Progress in the active iteration.
  double iteration = 0;  ///< Completed iteration count.

  /// Whether this layer's underlying value is rebuilt at its freeze boundary.
  bool usesFrozenUnderlying = false;

  /// Lower sandwich sampled at this layer's freeze boundary, for a partially frozen to-only
  /// animation. Null with usesFrozenUnderlying means the authored base. The recipe is built only
  /// from earlier layers, so ownership is acyclic.
  std::shared_ptr<const ContextualLengthRecipe> frozenUnderlying;
};

/// Ordered animation stack retained until the referencing instance's length context is known.
struct ContextualLengthRecipe {
  /// Validate owned operands and derive immutable storage/work costs; reject excessive input.
  /// @param samples Owned sample stack, consumed only after its shape has been checked.
  /// @return Immutable admitted recipe, or nullptr when its source/storage envelope is exceeded.
  static std::shared_ptr<const ContextualLengthRecipe> Create(
      std::vector<ContextualLengthSample>&& samples);

  const std::vector<ContextualLengthSample> samples;  ///< Lower to higher sandwich layers.
  const std::size_t sourceBytes;                      ///< Aggregate owned declaration bytes.
  const std::size_t retainedBytes;                    ///< Conservative admitted storage.
  const std::size_t evaluationWork;                   ///< Cost charged before consumer evaluation.
  const std::size_t nestingDepth;                     ///< Maximum frozen-boundary recipe depth.

private:
  ContextualLengthRecipe(std::vector<ContextualLengthSample>&& values, std::size_t source,
                         std::size_t bytes, std::size_t work, std::size_t depth)
      : samples(std::move(values)),
        sourceBytes(source),
        retainedBytes(bytes),
        evaluationWork(work),
        nestingDepth(depth) {}
};

}  // namespace donner::svg::components
