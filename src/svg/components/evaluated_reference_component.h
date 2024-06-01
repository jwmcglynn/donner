#pragma once
/// @file

#include "src/svg/registry/registry.h"

namespace donner::svg::components {

template <typename ReferenceType>
struct EvaluatedReferenceComponent {
  using Type = ReferenceType;
  EntityHandle target;
};

}  // namespace donner::svg::components
