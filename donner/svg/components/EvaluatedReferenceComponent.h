#pragma once
/// @file

#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

/**
 * Represents a reference to another entity which has been evaluated from a \ref Reference string.
 * This is used by \ref PaintSystem for gradients and patterns which have an `href` attribute for
 * inheritance.
 *
 * @tparam ReferenceType tag which determines which subsystem the reference is for, used to avoid
 * collisions.
 */
template <typename ReferenceType>
struct EvaluatedReferenceComponent {
  using Type = ReferenceType;  ///< Tag type of this reference.
  EntityHandle target;         ///< The resolved target entity of the reference.
};

}  // namespace donner::svg::components
