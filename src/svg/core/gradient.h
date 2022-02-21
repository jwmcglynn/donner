#pragma once

#include "src/css/color.h"

namespace donner::svg {

enum class GradientUnits {
  UserSpaceOnUse,
  ObjectBoundingBox,
  Default = ObjectBoundingBox,
};

enum class GradientSpreadMethod {
  Pad,
  Reflect,
  Repeat,
  Default = Pad,
};

struct GradientStop {
  float offset = 0.0;
  css::Color color{css::RGBA(0, 0, 0, 0xFF)};
  float opacity = 1.0f;
};

}  // namespace donner::svg
