#pragma once
/// @file

#include "donner/svg/registry/Registry.h"

namespace donner::svg::components {

template <typename ReferenceType>
struct EvaluatedReferenceComponent {
  using Type = ReferenceType;
  EntityHandle target;
};

}  // namespace donner::svg::components
