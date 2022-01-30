#pragma once

#include "src/base/transform.h"

namespace donner {

struct TransformComponent {
  Transformd transform;
};

struct ViewboxTransformComponent {
  Transformd transform;
};

}  // namespace donner
