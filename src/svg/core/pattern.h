#pragma once
/// @file

#include "src/css/color.h"

namespace donner::svg {

enum class PatternUnits {
  UserSpaceOnUse,
  ObjectBoundingBox,
  Default = ObjectBoundingBox,
};

enum class PatternContentUnits {
  UserSpaceOnUse,
  ObjectBoundingBox,
  Default = UserSpaceOnUse,
};

}  // namespace donner::svg
